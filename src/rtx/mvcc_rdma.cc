#include "mvcc_rdma.h"
#include "rdma_req_helper.hpp"
namespace nocc {

namespace rtx {


void MVCC::release_reads(yield_func_t &yield) {
  return; // no need release read, there is no read lock
}

void MVCC::release_writes(yield_func_t &yield, bool all) {
  int release_num = write_set_.size();
  if(!all) {
    release_num -= 1;
}
}

bool MVCC::try_read_rpc(int index, yield_func_t &yield) {
  std::vector<ReadSetItem> &set = read_set_;
  assert(index < set.size());
  auto it = set.begin() + index;
  START(temp);
  if((*it).pid != node_id_) {
    rpc_op<RTXMVCCWriteRequestItem>(cor_id_, RTX_READ_RPC_ID, (*it).pid,
                                 rpc_op_send_buf_,reply_buf_,
                                 /*init RTXMVCCWriteRequestItem*/
                                 (*it).pid, (*it).key, (*it).tableid,(*it).len, txn_start_time);
    worker_->indirect_yield(yield);
    uint8_t resp_status = *(uint8_t*)reply_buf_;
    if(resp_status == LOCK_SUCCESS_MAGIC)
      return true;
    else if (resp_status == LOCK_FAIL_MAGIC)
      return false;
    assert(false);
  }
  else {
    if((*it).node == NULL) {
      (*it).node = local_lookup_op((*it).tableid, (*it).key);
    }
    MVCCHeader* header = (MVCCHeader*)((*it).node->value);
    int pos = -1;
    if((pos = check_read(header, txn_start_time)) == -1) return false; // cannot read
    while(true) {
      volatile uint64_t rts = header->rts;
      volatile uint64_t* rts_ptr = &(header->rts);
      if(txn_start_time > rts) {
        if(!__sync_bool_compare_and_swap(rts_ptr, rts, txn_start_time)) {
          continue;
        }
        else {
          break;
        }
      }
      else {
        break;
      }
    }
    uint64_t before_reading_wts = header->wts[pos];
    // read the data here
    char* raw_data = (char*)((*it).node->value) + sizeof(MVCCHeader);
    memcpy((*it).data_ptr, raw_data + pos * (*it).len, (*it).len);
    int new_pos = check_read(header, txn_start_time);
    if(new_pos != pos || header->wts[new_pos] != before_reading_wts) {
      return false;
    }
  }
  END(temp);
  return true;
}


// rpc
bool MVCC::try_lock_read_rpc(int index, yield_func_t &yield) {
  START(lock);
  std::vector<ReadSetItem> &set = write_set_;
  auto it = set.begin() + index;
  if((*it).pid != node_id_) {//
    rpc_op<RTXMVCCWriteRequestItem>(cor_id_, RTX_LOCK_READ_RPC_ID, (*it).pid,
                               rpc_op_send_buf_,reply_buf_,
                               /*init RTXMVCCWriteRequestItem*/
                               (*it).pid, (*it).key, (*it).tableid,(*it).len,txn_start_time);
    worker_->indirect_yield(yield);
    END(lock);
    // got the response
    uint8_t resp_lock_status = *(uint8_t*)reply_buf_;
    if(resp_lock_status == LOCK_SUCCESS_MAGIC) {
      return true;
    }
    else if (resp_lock_status == LOCK_FAIL_MAGIC){
      return false;
    }
    assert(false);
  } 
  else {
    if((*it).node == NULL) {
      (*it).node = local_lookup_op((*it).tableid, (*it).key);
    }
    MVCCHeader *header = (MVCCHeader*)((*it).node->value);
    if(!check_write(header, txn_start_time)) return false;
    volatile uint64_t l = header->lock;
    if(l > txn_start_time) return false;
    while (true) {
      volatile uint64_t* lockptr = &(header->lock);
      if(unlikely(!__sync_bool_compare_and_swap(lockptr, 0, txn_start_time))) {
        worker_->yield_next(yield);
        continue;
      }
      else { // get the lock
        if(header->rts > txn_start_time) {
          *lockptr = 0;
          return false;
        }
        uint64_t max_wts = 0, min_wts = 0x7fffffffffffffff;
        int pos = -1;
        for(int i = 0; i < MVCC_VERSION_NUM; ++i) {
          if(header->wts[i] > max_wts) {
            max_wts = header->wts[i];
          }
          if(header->wts[i] < min_wts) {
            min_wts = header->wts[i];
            pos = i;
          }
        }
        if(max_wts > txn_start_time) {
          *lockptr = 0;
          return false;
        }
        (*it).seq = (uint64_t)pos;
        if((*it).data_ptr == NULL) 
          (*it).data_ptr = (char*)malloc((*it).len);
        char* raw_data = (char*)((*it).node->value) + sizeof(MVCCHeader);
        memcpy((*it).data_ptr, raw_data + pos * (*it).len, (*it).len);
      }
    }
  }
  return true;
}


void MVCC::read_rpc_handler(int id,int cid,char *msg,void *arg) {
  char* reply_msg = rpc_->get_reply_buf();
  uint8_t res = LOCK_SUCCESS_MAGIC; // success
  int request_item_parsed = 0;
  MemNode *node = NULL;
  size_t nodelen = 0;

  RTX_ITER_ITEM(msg,sizeof(RTXMVCCWriteRequestItem)) {
    auto item = (RTXMVCCWriteRequestItem *)ttptr;
    request_item_parsed++;
    assert(request_item_parsed <= 1); // no batching of lock request.
    if(item->pid != response_node_)
      continue;
    node = local_lookup_op(item->tableid, item->key);
    assert(node != NULL && node->value != NULL);
    MVCCHeader* header = (MVCCHeader*)(node->value);
    int pos = -1;
    if((pos = check_read(header, item->txn_starting_timestamp)) == -1) {
      res = LOCK_FAIL_MAGIC;
      goto END;
    }
    while(true) {
      volatile uint64_t rts = header->rts;
      volatile uint64_t* rts_ptr = &(header->rts);
      if(item->txn_starting_timestamp > rts) {
        if(!__sync_bool_compare_and_swap(rts_ptr, rts, item->txn_starting_timestamp)) {
          continue;
        }
        else break;
      }
      else break;
    }
    uint64_t before_reading_wts = header->wts[pos];
    char* raw_data = (char*)(node->value) + sizeof(MVCCHeader);
    char* reply = reply_msg + 1;
    *(uint64_t*)reply = (uint64_t)pos;
    memcpy(reply + sizeof(uint64_t), raw_data + pos * item->len, item->len);
    int new_pos = check_read(header, item->txn_starting_timestamp);
    if(new_pos != pos || header->wts[new_pos] != before_reading_wts) {
      res = LOCK_FAIL_MAGIC;
      goto END;
    }
    nodelen = sizeof(uint64_t) + item->len;
    goto END;
  }
END:
  *((uint8_t*)reply_msg) = res;
  rpc_->send_reply(reply_msg, sizeof(uint8_t) + nodelen, id, cid);
NO_REPLY:
  ;
}


void MVCC::lock_read_rpc_handler(int id,int cid,char *msg,void *arg) {
  char* reply_msg = rpc_->get_reply_buf();
  uint8_t res = LOCK_SUCCESS_MAGIC; // success
  int request_item_parsed = 0;
  MemNode *node = NULL;
  size_t nodelen = 0;

  RTX_ITER_ITEM(msg,sizeof(RTXMVCCWriteRequestItem)) {
    auto item = (RTXMVCCWriteRequestItem *)ttptr;
    request_item_parsed++;
    assert(request_item_parsed <= 1); // no batching of lock request.
    if(item->pid != response_node_)
      continue;
    node = local_lookup_op(item->tableid, item->key);
    assert(node != NULL && node->value != NULL);
    MVCCHeader* header = (MVCCHeader*)(node->value);
    if(!check_write(header, item->txn_starting_timestamp)) {
      res = LOCK_FAIL_MAGIC;
      goto END;
    }
    volatile uint64_t l = header->lock;
    if(l > item->txn_starting_timestamp) {
      res = LOCK_FAIL_MAGIC;
      goto END;
    }
    while(true) {
      volatile uint64_t* lockptr = &(header->lock);
      if(unlikely(!__sync_bool_compare_and_swap(lockptr, 0, item->txn_starting_timestamp))) {
#ifdef MVCC_NOWAIT
        res = LOCK_FAIL_MAGIC;
        goto END;
#else
        assert(false);
#endif
      }
      else {
        volatile uint64_t rts = header->rts;
        if(rts > item->txn_starting_timestamp) {
          res = LOCK_FAIL_MAGIC;
          goto END;
        }
        uint64_t max_wts = 0, min_wts = 0x7fffffffffffffff;
        int pos = -1;
        for(int i = 0; i < MVCC_VERSION_NUM; ++i) {
          if(header->wts[i] > max_wts) {
            max_wts = header->wts[i];
          }
          if(header->wts[i] < min_wts) {
            pos = i;
            min_wts = header->wts[i];
          }
        }
        if(max_wts > item->txn_starting_timestamp) {
          res = LOCK_FAIL_MAGIC;
          goto END;
        }
        char* reply = (char*)reply_msg + 1;
        *(uint64_t*)reply = (uint64_t)pos;
        char* raw_data = (char*)(node->value) + sizeof(MVCCHeader);
        memcpy(reply + sizeof(uint64_t), raw_data + pos * item->len, item->len);
        nodelen = sizeof(uint64_t) + item->len;
        goto END;
      }
    }
  }
END:
  *((uint8_t*)reply_msg) = res;
  rpc_->send_reply(reply_msg, sizeof(uint8_t) + nodelen, id, cid);
NO_REPLY:
  ;
}

void MVCC::register_default_rpc_handlers() {
  // register rpc handlers
  ROCC_BIND_STUB(rpc_,&MVCC::read_rpc_handler,this,RTX_READ_RPC_ID);
  ROCC_BIND_STUB(rpc_,&MVCC::lock_read_rpc_handler,this,RTX_LOCK_READ_RPC_ID);
}


}
}