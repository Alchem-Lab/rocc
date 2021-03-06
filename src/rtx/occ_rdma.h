#pragma once

#ifndef NOCC_RTX_OCC_RDMA_H_
#define NOCC_RTX_OCC_RDMA_H_

#include "tx_config.h"

#include "occ.h"
#include "dslr.h"
#include "core/logging.h"

#include "checker.hpp"

namespace nocc {

namespace rtx {

/**
 * Extend baseline OCC with one-sided RDMA support for execution, validation and commit.
 */
class OCCR : public OCC {
 public:
  OCCR(oltp::RWorker *worker,MemDB *db,RRpc *rpc_handler,int nid,int tid,int cid,int response_node,
          RdmaCtrl *cm,RScheduler* rdma_sched,int ms) :
      OCC(worker,db,rpc_handler,nid,tid,cid,response_node,
             cm,rdma_sched,ms)
  {
#if ENABLE_TXN_API
    // dslr_lock_manager is already included in the TxnAlg instance.
#else
    dslr_lock_manager = new DSLR(worker, db, rpc_handler, 
                                 nid, tid, cid, response_node, 
                                 cm, rdma_sched, ms);
#endif


    if(worker_id_ == 0 && cor_id_ == 0) {
#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_READ) != 0
      fprintf(stderr, "OCC uses ONE_SIDED READ.\n");
#else
      fprintf(stderr, "OCC uses RPC READ.\n");
#endif

#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_LOCK) != 0
      fprintf(stderr, "OCC uses ONE_SIDED LOCK.\n");
#else
      fprintf(stderr, "OCC uses RPC LOCK.\n");
#endif

#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_LOG) != 0
      fprintf(stderr, "OCC uses ONE_SIDED LOG.\n");
#else
      fprintf(stderr, "OCC uses RPC LOG.\n");
#endif

// #if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_2PC) != 0
//       fprintf(stderr, "OCC uses ONE_SIDED 2PC.\n");
// #else
//       fprintf(stderr, "OCC uses RPC 2PC.\n");
// #endif

#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_RELEASE) != 0
      fprintf(stderr, "OCC uses ONE_SIDED RELEASE.\n");
#else
      fprintf(stderr, "OCC uses RPC RELEASE.\n");
#endif

#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_COMMIT) != 0
      fprintf(stderr, "OCC uses ONE_SIDED COMMIT.\n");
#else
      fprintf(stderr, "OCC uses RPC COMMIT.\n");
#endif

#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_VALIDATE) != 0
      fprintf(stderr, "OCC uses ONE_SIDED VALIDATE.\n");
#else
      fprintf(stderr, "OCC uses RPC VALIDATE.\n");
#endif
    }

    // register normal RPC handlers
    register_default_rpc_handlers();

    // overwrites with default RPC handler
    /**
     * These RPC handlers does not operate on value/meta data in the index.
     * This can be slightly slower than default RPC handler for *LOCK* and *validate*.
     */
    //ROCC_BIND_STUB(rpc_,&OCCR::lock_rpc_handler2,this,RTX_LOCK_RPC_ID);
    //ROCC_BIND_STUB(rpc_,&OCCR::validate_rpc_handler2,this,RTX_VAL_RPC_ID);
    //ROCC_BIND_STUB(rpc_,&OCCR::commit_rpc_handler2,this,RTX_COMMIT_RPC_ID);
  }

  /**
   * Using RDMA one-side primitive to implement various TX operations
   */
  bool lock_writes_w_rdma(yield_func_t &yield);
  bool lock_writes_w_CAS_rdma(yield_func_t &yield);
  bool lock_writes_w_FA_rdma(yield_func_t &yield);
  void write_back_w_rdma(yield_func_t &yield);
  void write_back_w_CAS_rdma(yield_func_t &yield);
  void write_back_w_FA_rdma(yield_func_t &yield);  
  bool validate_reads_w_rdma(yield_func_t &yield);
  bool validate_reads_w_CAS_rdma(yield_func_t &yield);  
  bool validate_reads_w_FA_rdma(yield_func_t &yield);
  void release_writes_w_rdma(yield_func_t &yield);
  void release_writes_w_CAS_rdma(yield_func_t &yield);
  void release_writes_w_FA_rdma(yield_func_t &yield);

  int pending_remote_read(int pid,int tableid,uint64_t key,int len,yield_func_t &yield) {

    ASSERT(RDMA_CACHE) << "Currently RTX only supports pending remote read for value in cache.";

    char *data_ptr = (char *)Rmalloc(sizeof(MemNode) + len);
    assert(data_ptr != NULL);

    auto off = pending_rdma_read_val(pid,tableid,key,len,data_ptr,yield,sizeof(RdmaValHeader));
    data_ptr += sizeof(RdmaValHeader);

    read_set_.emplace_back(tableid,key,(MemNode *)off,data_ptr,
                           0,
                           len,pid);
    return read_set_.size() - 1;
  }

  int remote_read(int pid,int tableid,uint64_t key,int len,yield_func_t &yield) {
#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_READ) != 0
    START(read_lat);
    CYCLE_START(read);
    char *data_ptr = (char *)Rmalloc(sizeof(MemNode) + len);
    ASSERT(data_ptr != NULL);

    uint64_t off = 0;
#if INLINE_OVERWRITE
    CYCLE_PAUSE(read);
    off = rdma_lookup_op(pid,tableid,key,data_ptr,yield);
    CYCLE_RESUME(read);
    MemNode *node = (MemNode *)data_ptr;
    auto seq = node->seq;
    data_ptr = data_ptr + sizeof(MemNode);
#else
    CYCLE_PAUSE(read);
    off = rdma_read_val(pid,tableid,key,len,data_ptr,yield,sizeof(RdmaValHeader));
    CYCLE_RESUME(read);
    RdmaValHeader *header = (RdmaValHeader *)data_ptr;
    auto seq = header->seq;
    data_ptr = data_ptr + sizeof(RdmaValHeader);
#endif
    
    ASSERT(off != 0) << "RDMA remote read key error: tab " << tableid << " key " << key;

    read_set_.emplace_back(tableid,key,(MemNode *)off,data_ptr,
                           seq,
                           len,pid);
    CYCLE_END(read);
    END(read_lat);
    return read_set_.size() - 1;

#elif ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_VALIDATE) != 0

    char *data_ptr = (char *)Rmalloc(sizeof(MemNode) + len);
    ASSERT(data_ptr != NULL);
    uint64_t off = rdma_lookup_op(pid,tableid,key,data_ptr,yield);
    int index = OCC::remote_read(pid,tableid,key,len,yield);
    auto it = read_set_.begin() + index;
    it->off = off;
    return index;
#else
    return OCC::remote_read(pid,tableid,key,len,yield);
#endif
  }

  int pending_remote_write(int pid,int tableid,uint64_t key,int len,yield_func_t &yield) {

    ASSERT(RDMA_CACHE) << "Currently RTX only supports pending remote read for value in cache.";

    char *data_ptr = (char *)Rmalloc(sizeof(MemNode) + len);
    assert(data_ptr != NULL);

    auto off = pending_rdma_read_val(pid,tableid,key,len,data_ptr,yield,sizeof(RdmaValHeader));
    data_ptr += sizeof(RdmaValHeader);

    write_set_.emplace_back(tableid,key,(MemNode *)off,data_ptr,
                           0,
                           len,pid);
    return write_set_.size() - 1;
  }

  int remote_write(int pid,int tableid,uint64_t key,int len,yield_func_t &yield) {
#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_READ) != 0

    START(read_lat);
    CYCLE_START(read);
    char *data_ptr = (char *)Rmalloc(sizeof(MemNode) + len);
    ASSERT(data_ptr != NULL);

    uint64_t off = 0;
#if INLINE_OVERWRITE
    CYCLE_PAUSE(read);
    off = rdma_lookup_op(pid,tableid,key,data_ptr,yield);
    CYCLE_RESUME(read);
    MemNode *node = (MemNode *)data_ptr;
    auto seq = node->seq;
    data_ptr = data_ptr + sizeof(MemNode);
#else
    CYCLE_PAUSE(read);
    off = rdma_read_val(pid,tableid,key,len,data_ptr,yield,sizeof(RdmaValHeader));
    CYCLE_RESUME(read);
    RdmaValHeader *header = (RdmaValHeader *)data_ptr;
    auto seq = header->seq;
    data_ptr = data_ptr + sizeof(RdmaValHeader);
#endif
    
    ASSERT(off != 0) << "RDMA remote read key error: tab " << tableid << " key " << key;

    write_set_.emplace_back(tableid,key,(MemNode *)off,data_ptr,
                           seq,
                           len,pid);
    CYCLE_END(read);
    END(read_lat);
    return write_set_.size() - 1;

#elif ONE_SIDED_READ == 2 && ((HYBRID_CODE & RCC_USE_ONE_SIDED_LOCK) != 0 || (HYBRID_CODE & RCC_USE_ONE_SIDED_RELEASE) != 0 || (HYBRID_CODE & RCC_USE_ONE_SIDED_COMMIT) != 0)
    
    char *data_ptr = (char *)Rmalloc(sizeof(MemNode) + len);
    ASSERT(data_ptr != NULL);    
    uint64_t off = rdma_lookup_op(pid,tableid,key,data_ptr,yield);
    int index = OCC::remote_write(pid,tableid,key,len,yield);
    auto it = write_set_.begin() + index;
    it->off = off;
    return index;
#else

    return OCC::remote_write(pid,tableid,key,len,yield);
#endif
  }

  bool commit(yield_func_t &yield) {

#if TX_ONLY_EXE
    return dummy_commit();
#endif

#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_LOCK) != 0
#if USE_DSLR
    if(!lock_writes_w_FA_rdma(yield)) {
#else
    if(!lock_writes_w_rdma(yield)) {
#endif
#if !NO_ABORT
      // goto ABORT;
      release_writes(yield);
      gc_readset();
      gc_writeset();
      write_batch_helper_.clear();
      abort_cnt[25]++;
      return false;
#endif
    }
#else
    if(!lock_writes(yield)) {
#if !NO_ABORT
      goto ABORT;
#endif
    }
#endif

#if CHECKS
    RdmaChecker::check_lock_content(this,yield);
#endif

    asm volatile("" ::: "memory");

#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_VALIDATE) != 0
#if USE_DSLR
    if(!validate_reads_w_FA_rdma(yield)) {
#else
    if(!validate_reads_w_rdma(yield)) {
#endif
#if !NO_ABORT
      // goto ABORT;
      release_writes(yield);
      gc_readset();
      gc_writeset();
      write_batch_helper_.clear();
      abort_cnt[11]++;
      return false;
#endif
    }
#else
    if(!validate_reads(yield)) {
#if !NO_ABORT
      goto ABORT;
#endif
    }
#endif

#if TX_TWO_PHASE_COMMIT_STYLE > 0
    if(!do_2pc(yield)) {
      abort_cnt[12]++;
      return false;
    }
#endif

#if 1
    asm volatile("" ::: "memory");
    prepare_write_contents();
    log_remote(yield); // log remote using *logger_*
#if CHECKS
    RdmaChecker::check_log_content(this,yield);
#endif

    asm volatile("" ::: "memory");
#endif

    do_commit(yield);
    // clear the mac_set, used for the next time
    write_batch_helper_.clear();
    abort_cnt[10]++;
    return true;

ABORT:
    release_writes(yield);

    gc_readset();
    gc_writeset();
    // clear the mac_set, used for the next time
    write_batch_helper_.clear();
    // END(commit);
    abort_cnt[13]++;
    return false;
  }

  inline bool release_writes(yield_func_t &yield) {
    #if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_RELEASE) != 0
    #if USE_DSLR
        release_writes_w_FA_rdma(yield);
    #else
        release_writes_w_rdma(yield);
    #endif
    #else
        OCC::release_writes(yield);
    #endif
  }

  inline void do_commit(yield_func_t &yield) {
#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && (HYBRID_CODE & RCC_USE_ONE_SIDED_COMMIT) != 0
#if USE_DSLR
    write_back_w_FA_rdma(yield);    
#else
    write_back_w_rdma(yield);
#endif
#else
    /**
     * Fixme! write back w RPC now can only work with *lock_w_rpc*.
     * This is because lock_w_rpc helps fill the mac_set used in write_back.
     */
    write_back_oneshot(yield);
#endif

#if CHECKS
    RdmaChecker::check_backup_content(this,yield);
#endif
    gc_readset();
    gc_writeset();
  }

  inline bool do_2pc(yield_func_t &yield) {
    START(twopc)
    bool vote_commit = prepare_commit(yield); // broadcasting prepare messages and collecting votes
    // broadcast_decision(vote_commit, yield);
    END(twopc);
    if (!vote_commit) {
      // goto ABORT;
      release_writes(yield);
      gc_readset();
      gc_writeset();
      write_batch_helper_.clear();
      return false;
    }
    return true;
  }

  /**
   * GC the read/write set is a little complex using RDMA.
   * Since some pointers are allocated from the RDMA heap, not from local heap.
   */
  void gc_helper(std::vector<ReadSetItem> &set) {
    assert(false);
  }

  // overwrite GC functions, to use Rfree
  void gc_readset() {
    auto& set = read_set_;
    for(auto it = set.begin();it != set.end();++it) {
      if(it->pid != node_id_) {
#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && ((HYBRID_CODE & RCC_USE_ONE_SIDED_READ) != 0 || (HYBRID_CODE & RCC_USE_ONE_SIDED_VALIDATE) != 0)
#if INLINE_OVERWRITE
        Rfree((*it).data_ptr - sizeof(MemNode));
#else
        Rfree((*it).data_ptr - sizeof(RdmaValHeader));
#endif
#else
        free((*it).data_ptr);
#endif
      }
      else
        free((*it).data_ptr);
    }
  }

  void gc_writeset() {
    auto& set = write_set_;
    for(auto it = set.begin();it != set.end();++it) {
      if(it->pid != node_id_) {
#if ONE_SIDED_READ == 1 || ONE_SIDED_READ == 2 && ((HYBRID_CODE & RCC_USE_ONE_SIDED_READ) != 0 || (HYBRID_CODE & RCC_USE_ONE_SIDED_LOCK) != 0 || (HYBRID_CODE & RCC_USE_ONE_SIDED_RELEASE) != 0 || (HYBRID_CODE & RCC_USE_ONE_SIDED_COMMIT) != 0)
#if INLINE_OVERWRITE
        Rfree((*it).data_ptr - sizeof(MemNode));
#else
        Rfree((*it).data_ptr - sizeof(RdmaValHeader));
#endif
#else
        free((*it).data_ptr);
#endif
      }
      else
        free((*it).data_ptr);
    }
  }

  bool dummy_commit() {
    // clean remaining resources
    gc_readset();
    gc_writeset();
    return true;
  }

  /**
   * A specific lock handler, use the meta data encoded in value
   * This is because we only cache one address, so it's not so easy to
   * to encode meta data in the index (so that we need to cache the index).
   */
  void lock_rpc_handler2(int id,int cid,char *msg,void *arg);
  void commit_rpc_handler2(int id,int cid,char *msg,void *arg);
  void release_rpc_handler2(int id,int cid,char *msg,void *arg);
  void validate_rpc_handler2(int id,int cid,char *msg,void *arg);

private:
#if ENABLE_TXN_API
#else
  DSLR* dslr_lock_manager;
#endif
};

} // namespace rtx
} // namespace nocc

#endif
