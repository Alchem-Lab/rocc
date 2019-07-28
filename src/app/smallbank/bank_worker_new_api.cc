// implementations of smallbank on revised codebase

#include "bank_worker.h"
#include "tx_config.h"

#include "core/logging.h"

namespace nocc {

namespace oltp {

extern __thread util::fast_random   *random_generator;

namespace bank {

#if ENABLE_TXN_API

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_sp_new_api(calvin_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_sp_new_api(yield_func_t &yield) {
#endif

  rtx_->begin(yield);

#ifdef CALVIN_TX
  // rtx_->deserialize_read_set(req->n_reads, req->read_set);
  // rtx_->deserialize_write_set(req->n_writes, req->write_set);
  const char* buf = req->req_info;
  struct sp_req_info_t {
    uint64_t id0;
    uint64_t id1;
  };
  uint64_t id0 = ((sp_req_info_t*)buf)->id0;
  uint64_t id1 = ((sp_req_info_t*)buf)->id1;
#else
  uint64_t id0,id1;
  GetTwoAccount(random_generator[cor_id_],&id0,&id1);  
  // uint64_t id0 = 100, id1 = 101;
#endif

  int index = -1;
  // first account
  int pid = AcctToPid(id0);
  index = rtx_->write(pid,CHECK,id0,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
  // second account
  pid = AcctToPid(id1);
  index = rtx_->write(pid,CHECK,id1,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);

#ifdef CALVIN_TX
    if (!rtx_->request_locks(yield)) {
    return txn_result_t(false,73);
  }
#endif

  float amount = 5.0;

  // checking::value *&c0, *&c1;

  //Actual loads of values
  checking::value * c0 = (checking::value*)rtx_->load_write(0,sizeof(checking::value),yield);
  checking::value * c1 = (checking::value*)rtx_->load_write(1,sizeof(checking::value),yield);

#ifdef CALVIN_TX
  // fprintf(stdout, "before sync.\n");
  // fprintf(stdout, "@%p=%f", c0, c0 == NULL ? 0 : c0->c_balance);
  // fprintf(stdout, "@%p=%f", c1, c1 == NULL ? 0 : c1->c_balance);
  // sync_reads accomplishes phase 3 and phase 4: 
  // serving remote reads and collecting remote reads result.
  // if return false, it means that current transaction does not need
  // to execute transaction logic, i.e., the logic was executed at
  // some other active participating node.
  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  c0 = (checking::value*)rtx_->load_write(0,sizeof(checking::value),yield);
  c1 = (checking::value*)rtx_->load_write(1,sizeof(checking::value),yield);  

  // fprintf(stdout, "after sync.\n");  
  // fprintf(stdout, "@%p=%f", c0, c0 == NULL ? 0 : c0->c_balance);
  // fprintf(stdout, "@%p=%f", c1, c1 == NULL ? 0 : c1->c_balance);
#endif

  // LOG(3) << req->req_seq << " " << c0 << " " << &c0->c_balance << " " << c0->c_balance;
  // LOG(3) << req->req_seq << " " << c1 << " " << &c1->c_balance << " " << c1->c_balance;

  // transactional logic
  if(c0->c_balance < amount) {
  } else {
    c0->c_balance -= amount;
    c1->c_balance += amount;
  }
  // LOG(3) << sizeof(c0->c_balance) << " " << sizeof(c1->c_balance);
  // LOG(3) << req->req_seq << " " << c0 << " " << &c0->c_balance << " " << c0->c_balance;
  // LOG(3) << req->req_seq << " " << c1 << " " << &c1->c_balance << " " << c1->c_balance;
  // fprintf(stdout, "after execution.\n");  
  // fprintf(stdout, "@%p=%f", c0, c0 == NULL ? 0 : c0->c_balance);
  // fprintf(stdout, "@%p=%f", c1, c1 == NULL ? 0 : c1->c_balance);

  // transaction commit
  auto ret = rtx_->commit(yield);
  return txn_result_t(ret,73);
}

#ifdef CALVIN_TX
void BankWorker::txn_sp_new_api_gen_rwsets(char* buf, yield_func_t &yield) {
  int index = -1;

  // rtx_->clear_read_set();
  // rtx_->clear_write_set();

  // uint64_t id0,id1;
  // GetTwoAccount(random_generator[cor_id_],&id0,&id1);  
  uint64_t id0 = 100, id1 = 101;

  //serialize read set and write set
  // rtx_->serialize_read_set(buf);
  // rtx_->serialize_write_set(buf);
  struct sp_req_info_t {
    uint64_t id0;
    uint64_t id1;
  };
  ((sp_req_info_t*)buf)->id0 = id0;
  ((sp_req_info_t*)buf)->id1 = id1;
  return;
}

#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_wc_new_api(calvin_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_wc_new_api(yield_func_t &yield) {
#endif
  rtx_->begin(yield);

#ifdef CALVIN_TX
  // rtx_->deserialize_read_set(req->n_reads, req->read_set);
  // rtx_->deserialize_write_set(req->n_writes, req->write_set);
  const char* buf = req->req_info;
  struct wc_req_info_t {
    uint64_t id;
  };
  uint64_t id = ((wc_req_info_t*)buf)->id;
#else
  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);
#endif

  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->read(pid,SAV,id,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->write(pid,CHECK,id,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);

#ifdef CALVIN_TX
  if (!rtx_->request_locks(yield)) {
    return txn_result_t(false, 73);
  }
#endif

  float amount = 5.0; //from original code
  savings::value  *sv = (savings::value*)rtx_->load_read(0, sizeof(savings::value), yield);
  checking::value *cv = (checking::value*)rtx_->load_write(0, sizeof(checking::value), yield);

#ifdef CALVIN_TX
  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  sv = (savings::value*)rtx_->load_read(0, sizeof(savings::value), yield);
  cv = (checking::value*)rtx_->load_write(0, sizeof(checking::value), yield);  
#endif

  // transactional logic
  auto total = sv->s_balance + cv->c_balance;
  if(total < amount) {
    cv->c_balance -= (amount - 1);
  } else {
    cv->c_balance -= amount;
  }

  // transaction commit
  auto   ret = rtx_->commit(yield);
  return txn_result_t(ret,1);
}

#ifdef CALVIN_TX
void BankWorker::txn_wc_new_api_gen_rwsets(char* buf, yield_func_t &yield) {
  int index = -1;

  // rtx_->clear_read_set();
  // rtx_->clear_write_set();

  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);
  struct wc_req_info_t {
    uint64_t id;
  };
  ((wc_req_info_t*)buf)->id = id;
  //serialize read set and write set
  // rtx_->serialize_read_set(buf);
  // rtx_->serialize_write_set(buf);

  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_dc_new_api(calvin_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_dc_new_api(yield_func_t &yield) {
#endif
  rtx_->begin(yield);

#ifdef CALVIN_TX
  // rtx_->deserialize_read_set(req->n_reads, req->read_set);
  // rtx_->deserialize_write_set(req->n_writes, req->write_set);
  const char* buf = req->req_info;
  struct dc_req_info_t {
    uint64_t id;
  };
  uint64_t id = ((dc_req_info_t*)buf)->id;
#else
  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);
#endif

  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->write(pid,CHECK,id,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);

#ifdef CALVIN_TX
  if (!rtx_->request_locks(yield)) {
    return txn_result_t(false, 73);
  }
#endif

  float amount = 1.3;
  checking::value *cv = (checking::value*)rtx_->load_write(0,sizeof(checking::value),yield);

#ifdef CALVIN_TX
  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  cv = (checking::value*)rtx_->load_write(0,sizeof(checking::value),yield);    
#endif

  // transactional logic
  assert(cv != NULL);
  cv->c_balance += amount;

  // transaction commit
  bool ret = rtx_->commit(yield);
  return txn_result_t(ret,1);
}

#ifdef CALVIN_TX
void BankWorker::txn_dc_new_api_gen_rwsets(char* buf, yield_func_t &yield) {
  int index = -1;

  // rtx_->clear_read_set();
  // rtx_->clear_write_set();

  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);

  struct dc_req_info_t {
    uint64_t id;
  };
  ((dc_req_info_t*)buf)->id = id;

  //serialize read set and write set
  // rtx_->serialize_read_set(buf);
  // rtx_->serialize_write_set(buf);
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_ts_new_api(calvin_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_ts_new_api(yield_func_t &yield) {
#endif
  rtx_->begin(yield);

#ifdef CALVIN_TX
  // rtx_->deserialize_read_set(req->n_reads, req->read_set);
  // rtx_->deserialize_write_set(req->n_writes, req->write_set);
  const char* buf = req->req_info;
  struct ts_req_info_t {
    uint64_t id;
  };
  uint64_t id = ((ts_req_info_t*)buf)->id;
#else
  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);
#endif

  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->write(pid,SAV,id,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);

#ifdef CALVIN_TX
  if(!rtx_->request_locks(yield)) {
    return txn_result_t(false,73);  
  }
#endif

  float amount   = 20.20; //from original code
  auto sv = (savings::value*)rtx_->load_write(0, sizeof(savings::value), yield);

#ifdef CALVIN_TX
  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  sv = (savings::value*)rtx_->load_write(0, sizeof(savings::value), yield);  
#endif

  // transaction logic
  sv->s_balance += amount;

  // transaction commit
  auto ret = rtx_->commit(yield);
  return txn_result_t(ret,73);
}

#ifdef CALVIN_TX
void BankWorker::txn_ts_new_api_gen_rwsets(char* buf, yield_func_t &yield) {
  int index = -1;
  
  // rtx_->clear_read_set();
  // rtx_->clear_write_set();

  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);
  struct ts_req_info_t {
    uint64_t id;
  };
  ((ts_req_info_t*)buf)->id = id;

  //serialize read set and write set
  // rtx_->serialize_read_set(buf);
  // rtx_->serialize_write_set(buf);
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_balance_new_api(calvin_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_balance_new_api(yield_func_t &yield) {
#endif
  rtx_->begin(yield);

#ifdef CALVIN_TX
  // rtx_->deserialize_read_set(req->n_reads, req->read_set);
  // rtx_->deserialize_write_set(req->n_writes, req->write_set);

  const char* buf = req->req_info;
  struct balance_req_info_t {
    uint64_t id;
  };
  uint64_t id = ((balance_req_info_t*)buf)->id;
#else
  uint64_t id;
  GetAccount(random_generator[cor_id_],&(id));
#endif

  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->read(pid,CHECK,id,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->read(pid,SAV,id,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);

#ifdef CALVIN_TX
  if (!rtx_->request_locks(yield)) {
    return txn_result_t(false,73);
  }
#endif

  auto cv = (checking::value*)rtx_->load_read(0,sizeof(checking::value),yield);
  auto sv = (savings::value*)rtx_->load_read(1,sizeof(savings::value),yield);

#ifdef CALVIN_TX
  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  cv = (checking::value*)rtx_->load_read(0,sizeof(checking::value),yield);
  sv = (savings::value*)rtx_->load_read(1,sizeof(savings::value),yield);
#endif

  // transactional logic
  double res = 0.0;
  res = cv->c_balance + sv->s_balance;

  // commit transaction
  bool ret = rtx_->commit(yield);
  return txn_result_t(ret,(uint64_t)0);
}

#ifdef CALVIN_TX
void BankWorker::txn_balance_new_api_gen_rwsets(char* buf, yield_func_t &yield) {
  int index = -1;
  // rtx_->clear_read_set();
  // rtx_->clear_write_set();

  uint64_t id;
  GetAccount(random_generator[cor_id_],&(id));
  struct balance_req_info_t {
    uint64_t id;
  };
  ((balance_req_info_t*)buf)->id = id;

  //serialize read set and write set
  // rtx_->serialize_read_set(buf);
  // rtx_->serialize_write_set(buf);
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_amal_new_api(calvin_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_amal_new_api(yield_func_t &yield) {
#endif
  rtx_->begin(yield);

#ifdef CALVIN_TX
  // rtx_->deserialize_read_set(req->n_reads, req->read_set);
  // rtx_->deserialize_write_set(req->n_writes, req->write_set);
  const char* buf = req->req_info;
  struct amal_req_info_t {
    uint64_t id0;
    uint64_t id1;
  };
  uint64_t id0 = ((amal_req_info_t*)buf)->id0;
  uint64_t id1 = ((amal_req_info_t*)buf)->id1;
#else
  uint64_t id0,id1;
  GetTwoAccount(random_generator[cor_id_],&id0,&id1);
#endif

  int pid0 = AcctToPid(id0);
  int pid1 = AcctToPid(id1);
  int index = -1;
  index = rtx_->write(pid0,SAV,id0,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->write(pid0,CHECK,id0,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->write(pid1,CHECK,id1,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);

#ifdef CALVIN_TX
  if (!rtx_->request_locks(yield)) {
    return txn_result_t(false,73);
  }
#endif

  auto s0 = (savings::value*)rtx_->load_write(0,sizeof(savings::value),yield);
  auto c0 = (checking::value*)rtx_->load_write(1,sizeof(checking::value),yield);
  auto c1 = (checking::value*)rtx_->load_write(2,sizeof(checking::value),yield);

#ifdef CALVIN_TX
  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  s0 = (savings::value*)rtx_->load_write(0,sizeof(savings::value),yield);
  c0 = (checking::value*)rtx_->load_write(1,sizeof(checking::value),yield);
  c1 = (checking::value*)rtx_->load_write(2,sizeof(checking::value),yield);  
#endif

  // transactional logic
  double total = 0;
  total = s0->s_balance + c0->c_balance;
  s0->s_balance = 0;
  c0->c_balance = 0;
  c1->c_balance += total;

  // commit transaction
  auto ret = rtx_->commit(yield);
  return txn_result_t(ret,73); // since readlock success, so no need to abort
}

#ifdef CALVIN_TX
void BankWorker::txn_amal_new_api_gen_rwsets(char* buf, yield_func_t &yield) {
  int index = -1;
  // rtx_->clear_read_set();
  // rtx_->clear_write_set();
  
  uint64_t id0,id1;
  GetTwoAccount(random_generator[cor_id_],&id0,&id1);

  struct amal_req_info_t {
    uint64_t id0;
    uint64_t id1;
  };
  ((amal_req_info_t*)buf)->id0 = id0;
  ((amal_req_info_t*)buf)->id1 = id1;

  // int pid0 = AcctToPid(id0);
  // int pid1 = AcctToPid(id1);
  // index = rtx_->write(pid0,SAV,id0,sizeof(savings::value),yield);
  // assert(index >= 0);
  // index = rtx_->write(pid0,CHECK,id0,sizeof(checking::value),yield);
  // assert(index >= 0);
  // index = rtx_->write(pid1,CHECK,id1,sizeof(checking::value),yield);
  // assert(index >= 0);

  //serialize read set and write set
  // rtx_->serialize_read_set(buf);
  // rtx_->serialize_write_set(buf);
  return;
}
#endif

#endif // ENABLE_TXN_API

}; // namespace bank

}; // namespace oltp

};
