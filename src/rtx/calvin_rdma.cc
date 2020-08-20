#include "calvin_rdma.h"
#include "framework/bench_worker.h"

namespace nocc {

namespace rtx {


std::vector<ReadSetItem> CALVIN::read_set = std::move(std::vector<ReadSetItem>());
std::vector<ReadSetItem> CALVIN::write_set = std::move(std::vector<ReadSetItem>());

// return false I am not supposed to execute
// the actual transaction logic. (i.e., I am either not participating or am just a passive participant)
bool CALVIN::sync_reads(int req_seq, yield_func_t &yield) {
  // sync_reads accomplishes phase 3 and phase 4: 
  // serving remote reads and collecting remote reads result.
  // LOG(3) << "rsize in sync: " << read_set_.size();
  // LOG(3) << "wsize in sync: " << write_set_.size();
  assert (!read_set_.empty() || !write_set_.empty());
  
  std::set<int> passive_participants;
  std::set<int> active_participants;
  for (int i = 0; i < write_set_.size(); ++i) {
    // fprintf(stdout, "wpid = %d\n", write_set_[i].pid);
    // LOG(3) << "wset pid:" << write_set_[i].pid;
    active_participants.insert(write_set_[i].pid);
  }
  for (int i = 0; i < read_set_.size(); ++i) {
    // fprintf(stdout, "rpid = %d\n", read_set_[i].pid);  
    // LOG(3) << "rset pid:" << read_set_[i].pid;
    if (active_participants.find(read_set_[i].pid) == active_participants.end()) {
        passive_participants.insert(read_set_[i].pid);
      }
  }

  // fprintf(stdout, "active participants: \n");
  // for (auto itr = active_participants.begin(); itr != active_participants.end(); itr++) {
  //   fprintf(stdout, "%d ", *itr);
  // }
  // fprintf(stdout, "\n");
  
  // fprintf(stdout, "passive participants: \n");
  // for (auto itr = passive_participants.begin(); itr != passive_participants.end(); itr++) {
  //   fprintf(stdout, "%d ", *itr);
  // }
  // fprintf(stdout, "\n");
  
  bool am_I_active_participant = active_participants.find(response_node_) != active_participants.end();
  bool am_I_passive_participant = passive_participants.find(response_node_) != passive_participants.end();

  if (!am_I_passive_participant && !am_I_active_participant) {
    release_reads(yield);
    release_writes(yield);
    gc_readset();
    gc_writeset();
    return false; // false means that I am neither a active nor passive participant. I should skip instead.
  }

// #if 0
#if ONE_SIDED_READ == 0 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_VALUE_FORWARDING) == 0

  // phase 3: serving remote reads to active participants
  // If I am an active participant, only send to *other* active participants

  start_batch_rpc_op(read_batch_helper_);
  // broadcast the to active participants ONLY.
  
  for (auto itr = active_participants.begin(); itr != active_participants.end(); itr++) {
    if (*itr != cm_->get_nodeid())
      add_mac(read_batch_helper_, *itr);
  }


  if (!read_batch_helper_.mac_set_.empty()) {
    for (int i = 0; i < read_set_.size(); ++i) {
      if (read_set_[i].pid == cm_->get_nodeid()) {
        // fprintf(stdout, "forward read idx %d\n", i);
        assert(read_set_[i].data_ptr != NULL);
        add_batch_entry_wo_mac<read_val_t>(read_batch_helper_,
                                     read_set_[i].pid,
                                     /* init read_val_t */ 
                                     req_seq, 0, i, read_set_[i].len, 
                                     read_set_[i].data_ptr);
      }
    }
  
    // fprintf(stdout, "forward read start to machine: \n");
    // for (auto m : read_batch_helper_.mac_set_)
    //    fprintf(stdout, "%d ", m);
    // fprintf(stdout, "\n");
    auto replies = send_batch_rpc_op(read_batch_helper_,cor_id_,RTX_CALVIN_FORWARD_RPC_ID);
    assert(replies > 0);
    worker_->indirect_yield(yield);
    // fprintf(stdout, "forward read done.\n");
  } else {
    // fprintf(stdout, "No need to forward.\n");
  }


  start_batch_rpc_op(read_batch_helper_);

  for (auto itr = active_participants.begin(); itr != active_participants.end(); itr++) {
    if (*itr != cm_->get_nodeid())
      add_mac(read_batch_helper_, *itr);
  }

  if (!read_batch_helper_.mac_set_.empty()) {
    for (int i = 0; i < write_set_.size(); ++i) {
      if (write_set_[i].pid == cm_->get_nodeid()) {
        // fprintf(stdout, "forward write idx %d\n", i);
        assert(write_set_[i].data_ptr != NULL);
        add_batch_entry_wo_mac<read_val_t>(read_batch_helper_,
                                     write_set_[i].pid,
                                     /* init read_val_t */ 
                                     req_seq, 1, i, write_set_[i].len, 
                                     write_set_[i].data_ptr);
      }
    }
  
    // fprintf(stdout, "forward write start to machine: \n");
    // for (auto m : read_batch_helper_.mac_set_)
    //   fprintf(stdout, "%d ", m);
    // fprintf(stdout, "\n");
    auto replies = send_batch_rpc_op(read_batch_helper_,cor_id_,RTX_CALVIN_FORWARD_RPC_ID);
    assert(replies > 0);
    worker_->indirect_yield(yield);
    // fprintf(stdout, "forward write done.\n");
  } else {
    // fprintf(stdout, "No need to forward.\n");
  }


  if (!am_I_active_participant) {
    release_reads(yield);
    release_writes(yield);
    gc_readset();
    gc_writeset();
    return false;
  }

  // phase 4: check if all read_set and write_set has been collected.
  //          if not, wait.
  // fprintf(stdout, "collecting missing reads and writes...\n");
  std::map<uint64_t, read_val_t>& fv = static_cast<BenchWorker*>(worker_)->forwarded_values[cor_id_];
  while (true) {
    bool has_collected_all = true;
    for (auto i = 0; i < read_set_.size(); ++i) {
      if (read_set_[i].data_ptr == NULL) {
        uint64_t key = req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
        key |= (i<<1);
        auto it = fv.find(key);
        if (it != fv.end()) {
          read_set_[i].data_ptr = (char*)malloc(it->second.len);
          // fprintf(stderr, "read Malloc %p.\n", read_set_[i].data_ptr);
          memcpy(read_set_[i].data_ptr, it->second.value, it->second.len);
          // fprintf(stdout, "key %d read idx %d found.\n", key, i);
        } else {
          has_collected_all = false;
          break;
        }
      }
    }
    for (auto i = 0; i < write_set_.size(); ++i) {
      if (write_set_[i].data_ptr == NULL) {
        uint64_t key = req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
        key |= ((i<<1) + 1);
        auto it = fv.find(key);
        if (it != fv.end()) {
          write_set_[i].data_ptr = (char*)malloc(it->second.len);
          // fprintf(stderr, "write Malloc %p.\n", write_set_[i].data_ptr);
          memcpy(write_set_[i].data_ptr, it->second.value, it->second.len);
          // fprintf(stdout, "key %d write idx %d found.\n", key, i);
        } else {
          has_collected_all = false;
          break;
        }
      }
    }

    if (has_collected_all) break;
    else {
      // fprintf(stdout, "waiting for read/write set ready.\n");
      worker_->yield_next(yield);
    }
  }

#else

  // phase 3: serving remote reads to active participants
  // If I am an active participant, only send to *other* active participants

  std::set<int> mac_set;
  for (auto itr = active_participants.begin(); itr != active_participants.end(); itr++) {
    if (*itr != response_node_) {
      mac_set.insert(*itr);
      // fprintf(stdout, "forwarding dest %d\n", *itr);
    }
  }

  auto forward_offsets_ = static_cast<BenchWorker*>(worker_)->forward_offsets_;
  
  if (mac_set.size() > 0) {
    rtx::RDMAWriteReq req1(cor_id_, PA);
    for (int i = 0; i < read_set_.size(); ++i) {
        if (read_set_[i].pid == response_node_) {
          for (auto mac : mac_set) {
            Qp *qp = get_qp(mac);
            assert(qp != NULL);
            int forward_idx = req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
            forward_idx |= (i<<1);
            uint64_t remote_off = forward_offsets_[cor_id_] + forward_idx * sizeof(read_compact_val_t);

            // fprintf(stdout, "forwarding read of index %d at %p to remote off %lu of length %u.\n", forward_idx, read_set_[i].data_ptr, remote_off, read_set_[i].len);
            assert(read_set_[i].len <= MAX_VAL_LENGTH);
            req1.set_write_meta_for<0>(remote_off + OFFSETOF(read_compact_val_t, value), read_set_[i].data_ptr, read_set_[i].len);
            uint32_t* len = new uint32_t;
            *len = read_set_[i].len;
            req1.set_write_meta_for<1>(remote_off + OFFSETOF(read_compact_val_t, len), (char*)len, sizeof(uint32_t));
            req1.post_reqs(scheduler_, qp);
            if (unlikely(qp->rc_need_poll())) {
              worker_->indirect_yield(yield);
            }
          }
          worker_->indirect_yield(yield);
        }
    }

    for (int i = 0; i < write_set_.size(); ++i) {
        if (write_set_[i].pid == response_node_) {
          for (auto mac : mac_set) {
            Qp *qp = get_qp(mac);
            assert(qp != NULL);
            int forward_idx = req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
            forward_idx |= (i<<1) + 1;
            uint64_t remote_off = forward_offsets_[cor_id_] + forward_idx * sizeof(read_compact_val_t);

            // fprintf(stdout, "forwarding write of index %d at %p to remote off %lu of length %u.\n", forward_idx, write_set_[i].data_ptr, remote_off, write_set_[i].len);
            assert(write_set_[i].len <= MAX_VAL_LENGTH);
            req1.set_write_meta_for<0>(remote_off + OFFSETOF(read_compact_val_t, value), 
                    write_set_[i].data_ptr, write_set_[i].len);
            uint32_t* len = new uint32_t;
            *len = write_set_[i].len;
            req1.set_write_meta_for<1>(remote_off + OFFSETOF(read_compact_val_t, len), (char*)len, sizeof(uint32_t));
            req1.post_reqs(scheduler_, qp);
            if (unlikely(qp->rc_need_poll())) {
              worker_->indirect_yield(yield);
            }
          }
          worker_->indirect_yield(yield);
        }
    }
  }

  if (!am_I_active_participant) {
    release_reads(yield);
    release_writes(yield);
    gc_readset();
    gc_writeset();
    return false;
  }
  // phase 4: check if all read_set and write_set has been collected.
  //          if not, wait.

  auto forward_addresses = static_cast<BenchWorker*>(worker_)->forward_addresses;
  
  while (true) {
    bool has_collected_all = true;
    for (auto i = 0; i < read_set_.size(); ++i) {
      if (read_set_[i].data_ptr == NULL) {
        uint64_t key = req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
        key |= (i<<1);
        read_compact_val_t* fv = (read_compact_val_t*)forward_addresses[cor_id_] + key;
        if (fv->len != 0) {
          read_set_[i].data_ptr = (char*)malloc(fv->len);
          memcpy(read_set_[i].data_ptr, fv->value, fv->len);
          // fprintf(stdout, "key %d read idx %d found.\n", key, i);
        } else {
          has_collected_all = false;
          break;
        }
      }
    }
    for (auto i = 0; i < write_set_.size(); ++i) {
      if (write_set_[i].data_ptr == NULL) {
        uint64_t key = req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
        key |= ((i<<1) + 1);
        read_compact_val_t* fv = (read_compact_val_t*)forward_addresses[cor_id_] + key;
        if (fv->len != 0) {
          write_set_[i].data_ptr = (char*)malloc(fv->len);
          memcpy(write_set_[i].data_ptr, fv->value, fv->len);
          // fprintf(stdout, "key %d write idx %d found.\n", key, i);
        } else {
          has_collected_all = false;
          break;
        }
      }
    }

    if (has_collected_all) break;
    else {
      // fprintf(stderr, "%d %d %d waiting for read/write set ready.\n", worker_->worker_id_, cor_id_, req_seq);
      // for (int i = 0; i < read_set_.size(); ++i)
        // fprintf(stderr, "%d %d read %d key %lu data_ptr = %p.\n", worker_->worker_id_, cor_id_, i, read_set_[i].key, read_set_[i].data_ptr);        
      // for (int i = 0; i < write_set_.size(); ++i)
        // fprintf(stderr, "%d %d write %d key %lu data_ptr = %p.\n", worker_->worker_id_, cor_id_, i, write_set_[i].key, write_set_[i].data_ptr);
      worker_->yield_next(yield);
    }
  }

#endif // ONE_SIDED_READ

  // fprintf(stderr, "%d %d sync reads done.\n", worker_->worker_id_, cor_id_);
  return am_I_active_participant;
}

void CALVIN::release_reads(yield_func_t &yield) {
  using namespace rwlock;

  for(auto it = read_set_.begin();it != read_set_.end();++it) {
    if((*it).pid != cm_->get_nodeid())  // remote case
      continue;
    else {
      assert(it->node != NULL);
      volatile uint64_t* lock_ptr = &it->node->lock;
      assert ((*lock_ptr & 0x1) == W_LOCKED);
      // fprintf(stderr, "releasing read %d %d\n", it->tableid, it->key);
      *lock_ptr = 0;
    }
  }
}

void CALVIN::release_writes(yield_func_t &yield) {
  using namespace rwlock;

  for(auto it = write_set_.begin();it != write_set_.end();++it) {
    if((*it).pid != cm_->get_nodeid())  // remote case
      continue;
    else {
      assert(it->node != NULL);
      volatile uint64_t* lock_ptr = &it->node->lock;
      assert ((*lock_ptr & 0x1) == W_LOCKED);
      // fprintf(stderr, "releasing read %d %d\n", it->tableid, it->key);
      *lock_ptr = 0;
    }
  }
}

bool CALVIN::prepare_commit(yield_func_t &yield) {
    BatchOpCtrlBlock& clk = write_batch_helper_;
    start_batch_rpc_op(clk);

    for (auto it = write_set_.begin();it != write_set_.end();++it)
      clk.add_mac(it->pid);

    if (clk.mac_set_.size() == 0) {
      // LOG(3) << "no 2pc prepare message sent due to read-only txn.";
      return true;
    }
    // LOG(3) << "sending prepare messages to " << clk.mac_set_.size() << " macs";
    return two_phase_committer_->prepare(this, clk, cor_id_, yield);
}

void CALVIN::broadcast_decision(bool commit_or_abort, yield_func_t &yield) {
    BatchOpCtrlBlock& clk = write_batch_helper_;
    start_batch_rpc_op(clk);

    for (auto it = write_set_.begin();it != write_set_.end();++it)
      clk.add_mac(it->pid);
    if (clk.mac_set_.size() == 0) {
      // LOG(3) << "no 2pc decision message sent due to read-only txn.";
      return;
    }
    // LOG(3) << "sending decision messages to " << clk.mac_set_.size() << " macs";
    two_phase_committer_->broadcast_global_decision(this, clk, commit_or_abort ? 
                                                   TwoPhaseCommitMemManager::TWO_PHASE_DECISION_COMMIT : 
                                                   TwoPhaseCommitMemManager::TWO_PHASE_DECISION_ABORT, cor_id_, yield);
}

void CALVIN::write_back(yield_func_t &yield) {
  // step 5: applying writes
  // ignore remote writes since they will be viewed as local writes
  // at some apropriate node.
  for(auto it = write_set_.begin();it != write_set_.end();++it) {
    if((*it).pid != cm_->get_nodeid()) // ignore remote write
      continue;
    else {
      // fprintf(stdout, "write back %f@%p with len = %d for table %d key %d\n", *(float*)(it->data_ptr), it->data_ptr, it->len, it->tableid, it->key);
      assert(it->node != NULL);
      //the meta_len para cannot be ignored since it defaults to 0!
      inplace_write_op(it->node,it->data_ptr,it->len, db_->_schemas[it->tableid].meta_len);
    }
  }
}

/* RPC handlers */
void CALVIN::forward_rpc_handler(int id,int cid,char *msg,void *arg) {
  char* reply_msg = rpc_->get_reply_buf();
  char *reply = reply_msg + sizeof(ReplyHeader);

  assert(static_cast<BenchWorker*>(worker_)->forwarded_values != NULL);
  std::map<uint64_t, read_val_t>& fv = static_cast<BenchWorker*>(worker_)->forwarded_values[cid];
  
  // fprintf(stdout, "in calvin forward rpc handler.\n");
  
  assert(id != cm_->get_nodeid());

  int num_returned(0);
  RTX_ITER_ITEM(msg,sizeof(read_val_t)) {

    read_val_t *item = (read_val_t *)ttptr;

    // fprintf(stdout, "got forwarded value: len=%d, val=%s", item->len, item->value);
    // find the read/write set of the corresponding coroutine
    // and update the value using the forwarded value.
    
    if (item->read_or_write == 0)  { // READ
      // ReadSetItem& set_item = (*(static_cast<BenchWorker*>(worker_))->read_set_ptr[cid])[item->index_in_set];
      // assert(set_item.pid != response_node_);
      // // assert(data_ptr == NULL);
      // if (set_item.data_ptr == NULL)
      //   fprintf(stdout, "data_ptr @ %p updated.\n", &set_item.data_ptr);
      // else
      //   fprintf(stdout, "data_ptr @ %p re-updated.\n", &set_item.data_ptr);

      // set_item.data_ptr = (char*)malloc(item->len);
      // memcpy(set_item.data_ptr, item->value, item->len);

      uint64_t key = item->req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
      key |= ((item->index_in_set << 1));
      fv[key] = *item;

      // fprintf(stdout, "key %u installed for read idx %d.\n", key, item->index_in_set);
    } else if (item->read_or_write == 1) { // WRITE
      // ReadSetItem& set_item = (*(static_cast<BenchWorker*>(worker_))->write_set_ptr[cid])[item->index_in_set];
      // assert(set_item.pid != response_node_);
      // assert(data_ptr == NULL);
      // if (set_item.data_ptr == NULL)
      //   fprintf(stdout, "data_ptr @ %p updated.\n", &set_item.data_ptr);
      // else
      //   fprintf(stdout, "data_ptr @ %p re-updated.\n", &set_item.data_ptr);

      // set_item.data_ptr = (char*)malloc(item->len);
      // memcpy(set_item.data_ptr, item->value, item->len);
      uint64_t key = item->req_seq << MAX_CALVIN_SETS_SUPPRTED_IN_BITS;
      key |= ((item->index_in_set << 1) + 1);
      fv[key] = *item;

      // fprintf(stdout, "key %u installed for write idx %d.\n", key, item->index_in_set);
    } else
      assert(false);

    // num_returned += 1;
  } // end for

  num_returned = 1;
  ((ReplyHeader *)reply_msg)->num = num_returned;
  assert(num_returned > 0);
  // fprintf(stdout, "forward handler reply.\n");
  rpc_->send_reply(reply_msg,reply - reply_msg,id,cid);
  // send reply
}

void CALVIN::prepare_write_contents() {
    write_batch_helper_.clear_buf();

    for(auto it = write_set_.begin();it != write_set_.end();++it) {
        if(it->pid != node_id_) {
            add_batch_entry_wo_mac<RtxWriteItem>(write_batch_helper_,
                    (*it).pid,
                    /* init write item */ (*it).pid,(*it).tableid,(*it).key,(*it).len);
            memcpy(write_batch_helper_.req_buf_end_,(*it).data_ptr,(*it).len);
            write_batch_helper_.req_buf_end_ += (*it).len;
        }
    }
}

void CALVIN::log_remote(yield_func_t &yield) {

  if(write_set_.size() > 0 && global_view->rep_factor_ > 0) {

    // re-use write_batch_helper_'s data structure
    BatchOpCtrlBlock cblock(write_batch_helper_.req_buf_,write_batch_helper_.reply_buf_);
    cblock.batch_size_  = write_batch_helper_.batch_size_;
    cblock.req_buf_end_ = write_batch_helper_.req_buf_end_;

#if EM_FASST
    global_view->add_backup(response_node_,cblock.mac_set_);
    ASSERT(cblock.mac_set_.size() == global_view->rep_factor_)
        << "FaSST should uses rep-factor's log entries, current num "
        << cblock.mac_set_.size() << "; rep-factor " << global_view->rep_factor_;
#else
    for(auto it = write_batch_helper_.mac_set_.begin();
        it != write_batch_helper_.mac_set_.end();++it) {
      global_view->add_backup(*it,cblock.mac_set_);
    }
    // add local server
    global_view->add_backup(current_partition,cblock.mac_set_);
#endif

#if CHECKS
    LOG(3) << "log to " << cblock.mac_set_.size() << " macs";
#endif

    START(log);
    logger_->log_remote(cblock,cor_id_);
    abort_cnt[18]++;
    worker_->indirect_yield(yield);
    END(log);
#if 1
    cblock.req_buf_ = rpc_->get_fly_buf(cor_id_);
    memcpy(cblock.req_buf_,write_batch_helper_.req_buf_,write_batch_helper_.batch_msg_size());
    cblock.req_buf_end_ = cblock.req_buf_ + write_batch_helper_.batch_msg_size();
    //log ack
    logger_->log_ack(cblock,cor_id_); // need to yield
    abort_cnt[18]++;
    worker_->indirect_yield(yield);
#endif
  } // end check whether it is necessary to log
}

void CALVIN::register_default_rpc_handlers() {
  // register rpc handlers
  ROCC_BIND_STUB(rpc_,&CALVIN::forward_rpc_handler,this,RTX_CALVIN_FORWARD_RPC_ID);
}

} // namespace rtx

} // namespace nocc
