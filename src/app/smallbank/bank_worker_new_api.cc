// implementations of smallbank on revised codebase

#include "bank_worker.h"
#include "rtx/global_vars.h"
#include "tx_config.h"

#include "core/logging.h"
extern int ycsb_set_length;
extern int ycsb_write_num;
extern int sleep_time;

namespace nocc {

namespace oltp {

extern __thread util::fast_random   *random_generator;

namespace bank {

#if ENABLE_TXN_API

using namespace nocc::rtx;

#ifdef CALVIN_TX
txn_result_t BankWorker::ycsb_func(det_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::ycsb_func(yield_func_t &yield) {
#endif

  // LOG(3) << "ycsb" << sizeof(ycsb_record::value);
  // ASSERT(sizeof(ycsb_record::value) == 1000) << sizeof(ycsb_record::value);
  int index = -1;
  const int func_size = ycsb_set_length;

#ifdef CALVIN_TX
  assert(func_size < MAX_CALVIN_SETS_SUPPORTED);
#endif
  
  std::set<uint64_t> accounts;
  assert(rtx_ != NULL);

#ifdef CALVIN_TX
  rtx_->begin(req, yield);
#else
  rtx_->begin(yield);
  for(int i = 0; i < func_size; ++i) {
    is_write[i] = (random_generator[cor_id_].next() % ycsb_set_length) < ycsb_write_num;
    uint64_t id;
      //LOG(3) << "begin";
    GetAccount(random_generator[cor_id_],&id);
      //LOG(3) << id;
    while(accounts.find(id) != accounts.end()) {
      GetAccount(random_generator[cor_id_], &id);
    }
    accounts.insert(id);
    ids[i] = id;
  }

  for(int i = 0; i < func_size; ++i) {
    uint64_t id = ids[i];
    int pid = AcctToPid(id);
    if(is_write[i]) {
      index = rtx_->write(pid, YCSB, id, sizeof(ycsb_record::value), yield);
    }
    else {
      index = rtx_->read(pid, YCSB, id, sizeof(ycsb_record::value), yield);
    }
    if(index < 0) return txn_result_t(false, 73);
  }
#endif

#ifdef CALVIN_TX
  // static_assert(sizeof(ycsb_record::value) <= MAX_VAL_LENGTH, "ycsb_record to large to fit to be forwarded.\n");
  // fprintf(stdout, "show values before sync for request %d.\n", req->req_seq);
  ycsb_record::value* val = NULL; 
  rwsets_t* sets = (rwsets_t*)req->req_info;
  for (int i = 0; i < sets->nReads; ++i) {
      val = (ycsb_record::value*)rtx_->load_read(i, sizeof(ycsb_record::value), yield); 
  }
  for (int i = 0; i < sets->nWrites; ++i) {
      val = (ycsb_record::value*)rtx_->load_write(i, sizeof(ycsb_record::value), yield);
  }

  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  for (int i = 0; i < sets->nReads; ++i) {
      val = (ycsb_record::value*)rtx_->load_read(i, sizeof(ycsb_record::value), yield); 
  }
  for (int i = 0; i < sets->nWrites; ++i) {
      val = (ycsb_record::value*)rtx_->load_write(i, sizeof(ycsb_record::value), yield);
  }
#else
  ycsb_record::value* val = NULL;
  for(int i = 0; i < func_size; ++i) {
    if(is_write[i]) {
      val = (ycsb_record::value*)rtx_->load_write(indexes[i], sizeof(ycsb_record::value), yield);
    }
    else {
      val = (ycsb_record::value*)rtx_->load_read(indexes[i], sizeof(ycsb_record::value), yield); 
    }
    if(val == NULL) return txn_result_t(false, 73);
  }
#endif

#if SUNDIAL_TX && ONE_SIDED_READ
  if(!rtx_->prepare(yield))
      return txn_result_t(false, 73);
#endif
  // int dummy_ret = rtx_->dummy_work(10000, indexes[3]); 
  // LOG(3) << dummy_ret;
  usleep(sleep_time);

  auto ret = rtx_->commit(yield);
  return txn_result_t(ret, 73);
}

#ifdef CALVIN_TX
void BankWorker::ycsb_gen_rwsets(char* buf, util::fast_random& rand_gen, yield_func_t &yield) {
  int index = -1;
  int nReads = 0;
  int nWrites = 0;

  const int func_size = ycsb_set_length;
  assert(func_size < MAX_CALVIN_SETS_SUPPORTED);
  bool is_write[func_size];
  uint64_t ids[func_size];
  std::set<uint64_t> accounts;

  for(int i = 0; i < func_size; ++i) {
    //is_write[i] = (random_generator[cor_id_].next() % 2);
    is_write[i] = (rand_gen.next() % ycsb_set_length) < ycsb_write_num;
    uint64_t id;
    GetAccount(rand_gen, &id);
    while(accounts.find(id) != accounts.end()) {
      GetAccount(rand_gen, &id);
    }
    accounts.insert(id);
    ids[i] = id;
  }

  using namespace nocc::rtx;
  for(int i = 0; i < func_size; ++i) {
    uint64_t id = ids[i];
    int pid = AcctToPid(id);
    if(is_write[i]) {
      index = CALVIN::write2(pid, YCSB, id, sizeof(ycsb_record::value), yield);
      nWrites += 1;
      assert(index >= 0);
    }
    else {
      index = CALVIN::read2(pid, YCSB, id, sizeof(ycsb_record::value), yield);
      nReads += 1;
      assert(index >= 0);
    }
  }

  // buffer structure
  // readset size: uint8_t
  // writeset set: unit8_t
  // readset array: ReadSetItem
  // writeset array: ReadSetItem
  ((rwsets_t*)buf)->nReads = nReads;
  ((rwsets_t*)buf)->nWrites = nWrites;
  for (int j = 0; j < nReads; j++) {
    ((rwsets_t*)buf)->access[j] = CALVIN::read_set[j];
  }
  for (int j = 0; j < nWrites; j++) {
    ((rwsets_t*)buf)->access[j+nReads] = CALVIN::write_set[j]; 
  }
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_sp_new_api(det_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_sp_new_api(yield_func_t &yield) {
#endif

  int index = -1;
  assert(rtx_ != NULL);

#ifdef CALVIN_TX
  rtx_->begin(req, yield);
  // rtx_->deserialize_read_set(req->n_reads, req->read_set);
  // rtx_->deserialize_write_set(req->n_writes, req->write_set);
  // const char* buf = req->req_info;
  // struct sp_req_info_t {
  //   uint64_t id0;
  //   uint64_t id1;
  // };
  // uint64_t id0 = ((sp_req_info_t*)buf)->id0;
  // uint64_t id1 = ((sp_req_info_t*)buf)->id1;
#else
  rtx_->begin(yield);
  uint64_t id0,id1;
  GetTwoAccount(random_generator[cor_id_],&id0,&id1);
#endif

  // static ids overriding real ids for testing purpose
  // id0 = 4722155; id1 = 2941571;
  // id0 = 1850100; id1 = 3849882;
  // id0 = 2254304; id1 = 462416;
  // id0 = 2043856; id1 = 2634303;
  // id0 = 3380114; id1 = 1746621;
  // id0 = 43427; id1 = 10826;
  // id0 = 1741204; id1 = 2757632;
  // id0 = 672362; id1 = 2478470;
  // id0 = 2745895; id1 = 4752523;
  // id0 = 4028119; id1 = 2073906;
  // id0 = 2203856; id1 = 4771642;
  // id0 = 3105507; id1 = 4217067;
  // id0 = 6032; id1 = 13672;
  // id0 = 1101594; id1 = 1252123;
  // id0 = 4282979; id1 = 1232897;
  // id0 = 3912844; id1 = 3305893;
  // id0 = 3228324; id1 = 3290183;
  // id0 = 1088934; id1 = 4258699;
  // id0 = 2674508; id1 = 4171672;
  // id0 = 3762869; id1 = 4733674;
  // id0 = 1821612; id1 = 2848824;
  // id0 = 3030624; id1 = 3767453;
  // id0 = 1188591; id1 = 219745;
  // id0 = 852775; id1 = 4664234;
  // id0 = 4097369; id1 = 1088084;
  // id0 = 4722155; id1 = 2941571;

  // first account
  // int pid = AcctToPid(id0);
  // index = rtx_->write(pid,CHECK,id0,sizeof(checking::value),yield);
  // if (index < 0) return txn_result_t(false,73);
  // // second account
  // pid = AcctToPid(id1);
  // index = rtx_->write(pid,CHECK,id1,sizeof(checking::value),yield);
  // if (index < 0) return txn_result_t(false,73);

// #ifdef CALVIN_TX
//     if (!rtx_->request_locks(yield)) {
//     return txn_result_t(false,73);
//   }
// #endif

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

  // LOG(3) << req->req_seq << " " << c0 << " " << &c0->c_balance << " " << c0->c_balance;
  // LOG(3) << req->req_seq << " " << c1 << " " << &c1->c_balance << " " << c1->c_balance;
#endif

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
void BankWorker::txn_sp_new_api_gen_rwsets(char* buf, util::fast_random& rand_gen, yield_func_t &yield) {
  int index = -1;
  int nReads = 0;
  int nWrites = 0;

  uint64_t id0,id1;
  GetTwoAccount(rand_gen,&id0,&id1);  
  // uint64_t id0 = 100, id1 = 101;
  int pid = AcctToPid(id0);
  index = CALVIN::write2(pid, CHECK, id0, sizeof(checking::value), yield);
  nWrites += 1;
  assert (index >= 0);
  pid = AcctToPid(id1);
  index = CALVIN::write2(pid, CHECK, id1, sizeof(checking::value), yield);
  nWrites += 1;
  assert (index >= 0);

  // buffer structure
  // readset size: uint8_t
  // writeset set: unit8_t
  // readset array: ReadSetItem
  // writeset array: ReadSetItem
  ((rwsets_t*)buf)->nReads = nReads;
  ((rwsets_t*)buf)->nWrites = nWrites;
  for (int j = 0; j < nReads; j++) {
    ((rwsets_t*)buf)->access[j] = CALVIN::read_set[j];
  }
  for (int j = 0; j < nWrites; j++) {
    ((rwsets_t*)buf)->access[j+nReads] = CALVIN::write_set[j]; 
  }
  return;
}

#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_wc_new_api(det_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_wc_new_api(yield_func_t &yield) {
#endif

#ifdef CALVIN_TX
  rtx_->begin(req, yield);
#else
  rtx_->begin(yield);
  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);


  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->read(pid,SAV,id,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->write(pid,CHECK,id,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
#endif

#ifdef CALVIN_TX
  savings::value  *sv = (savings::value*)rtx_->load_read(0, sizeof(savings::value), yield);
  checking::value *cv = (checking::value*)rtx_->load_write(0, sizeof(checking::value), yield);

  if(!rtx_->sync_reads(req->req_seq, yield))
    return txn_result_t(true, 73);

  sv = (savings::value*)rtx_->load_read(0, sizeof(savings::value), yield);
  cv = (checking::value*)rtx_->load_write(0, sizeof(checking::value), yield);
  assert(sv != NULL && cv != NULL);
#else
  savings::value  *sv = (savings::value*)rtx_->load_read(0, sizeof(savings::value), yield);
  //[chao] is the following 2 returns necessary?
  if(sv == NULL) return txn_result_t(false,73);
  checking::value *cv = (checking::value*)rtx_->load_write(0, sizeof(checking::value), yield);
  if(cv == NULL) return txn_result_t(false, 73);
#endif

  // transactional logic
  float amount = 5.0; //from original code
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
void BankWorker::txn_wc_new_api_gen_rwsets(char* buf, util::fast_random& rand_gen, yield_func_t &yield) {
  int index = -1;
  int nReads = 0;
  int nWrites = 0;

  uint64_t id;
  GetAccount(rand_gen,&id);
  int pid = AcctToPid(id);

  index = CALVIN::read2(pid,SAV,id,sizeof(savings::value),yield);
  nReads += 1;
  assert (index >= 0);
  index = CALVIN::write2(pid,CHECK,id,sizeof(checking::value),yield);
  nWrites += 1;
  assert (index >= 0);

  ((rwsets_t*)buf)->nReads = nReads;
  ((rwsets_t*)buf)->nWrites = nWrites;
  for (int j = 0; j < nReads; j++) {
    ((rwsets_t*)buf)->access[j] = CALVIN::read_set[j];
  }
  for (int j = 0; j < nWrites; j++) {
    ((rwsets_t*)buf)->access[j+nReads] = CALVIN::write_set[j]; 
  }
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_dc_new_api(det_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_dc_new_api(yield_func_t &yield) {
#endif

#ifdef CALVIN_TX
  rtx_->begin(req, yield);
#else
  rtx_->begin(yield);
  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);

  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->write(pid,CHECK,id,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
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
void BankWorker::txn_dc_new_api_gen_rwsets(char* buf, util::fast_random& rand_gen, yield_func_t &yield) {
  int index = -1;
  int nReads = 0;
  int nWrites = 0;

  uint64_t id;
  GetAccount(rand_gen,&id);

  int pid = AcctToPid(id);
  index = CALVIN::write2(pid,CHECK,id,sizeof(checking::value),yield);
  nWrites += 1;
  assert (index >= 0);

  ((rwsets_t*)buf)->nReads = nReads;
  ((rwsets_t*)buf)->nWrites = nWrites;
  for (int j = 0; j < nReads; j++) {
    ((rwsets_t*)buf)->access[j] = CALVIN::read_set[j];
  }
  for (int j = 0; j < nWrites; j++) {
    ((rwsets_t*)buf)->access[j+nReads] = CALVIN::write_set[j]; 
  }
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_ts_new_api(det_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_ts_new_api(yield_func_t &yield) {
#endif

#ifdef CALVIN_TX
  rtx_->begin(req, yield);
#else
  rtx_->begin(yield);
  uint64_t id;
  GetAccount(random_generator[cor_id_],&id);

  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->write(pid,SAV,id,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);
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
void BankWorker::txn_ts_new_api_gen_rwsets(char* buf, util::fast_random& rand_gen, yield_func_t &yield) {
  int index = -1;
  int nReads = 0;
  int nWrites = 0;

  uint64_t id;
  GetAccount(rand_gen,&id);
  int pid = AcctToPid(id);
  index = CALVIN::write2(pid,SAV,id,sizeof(savings::value),yield);
  nWrites += 1;
  assert (index >= 0);

  ((rwsets_t*)buf)->nReads = nReads;
  ((rwsets_t*)buf)->nWrites = nWrites;
  for (int j = 0; j < nReads; j++) {
    ((rwsets_t*)buf)->access[j] = CALVIN::read_set[j];
  }
  for (int j = 0; j < nWrites; j++) {
    ((rwsets_t*)buf)->access[j+nReads] = CALVIN::write_set[j]; 
  }
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_balance_new_api(det_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_balance_new_api(yield_func_t &yield) {
#endif

#ifdef CALVIN_TX
  rtx_->begin(req, yield);  
#else
  rtx_->begin(yield);
  uint64_t id;
  GetAccount(random_generator[cor_id_],&(id));

  int pid = AcctToPid(id);
  int index = -1;
  index = rtx_->read(pid,CHECK,id,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->read(pid,SAV,id,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);
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
void BankWorker::txn_balance_new_api_gen_rwsets(char* buf, util::fast_random& rand_gen, yield_func_t &yield) {
  int index = -1;
  int nReads = 0;
  int nWrites = 0;

  uint64_t id;
  GetAccount(rand_gen,&(id));
  int pid = AcctToPid(id);
  index = CALVIN::read2(pid,CHECK,id,sizeof(checking::value),yield);
  nReads += 1;
  assert (index >= 0);
  index = CALVIN::read2(pid,SAV,id,sizeof(savings::value),yield);
  nReads += 1;
  assert (index >= 0);

  ((rwsets_t*)buf)->nReads = nReads;
  ((rwsets_t*)buf)->nWrites = nWrites;
  for (int j = 0; j < nReads; j++) {
    ((rwsets_t*)buf)->access[j] = CALVIN::read_set[j];
  }
  for (int j = 0; j < nWrites; j++) {
    ((rwsets_t*)buf)->access[j+nReads] = CALVIN::write_set[j]; 
  }
  return;
}
#endif

#ifdef CALVIN_TX
txn_result_t BankWorker::txn_amal_new_api(det_request* req, yield_func_t &yield) {
#else
txn_result_t BankWorker::txn_amal_new_api(yield_func_t &yield) {
#endif

#ifdef CALVIN_TX
  rtx_->begin(req, yield);
#else
  rtx_->begin(yield);
  uint64_t id0,id1;
  GetTwoAccount(random_generator[cor_id_],&id0,&id1);

  int pid0 = AcctToPid(id0);
  int pid1 = AcctToPid(id1);
  int index = -1;
  index = rtx_->write(pid0,SAV,id0,sizeof(savings::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->write(pid0,CHECK,id0,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
  index = rtx_->write(pid1,CHECK,id1,sizeof(checking::value),yield);
  if (index < 0) return txn_result_t(false,73);
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
void BankWorker::txn_amal_new_api_gen_rwsets(char* buf, util::fast_random& rand_gen, yield_func_t &yield) {
  int index = -1;
  int nReads = 0;
  int nWrites = 0;
  
  uint64_t id0,id1;
  GetTwoAccount(rand_gen,&id0,&id1);
  int pid0 = AcctToPid(id0);
  int pid1 = AcctToPid(id1);

  index = CALVIN::write2(pid0,SAV,id0,sizeof(savings::value),yield);
  assert (index >= 0);
  index = CALVIN::write2(pid0,CHECK,id0,sizeof(checking::value),yield);
  assert (index >= 0);
  index = CALVIN::write2(pid1,CHECK,id1,sizeof(checking::value),yield);
  assert (index >= 0);
  return;
}
#endif

#endif // ENABLE_TXN_API

}; // namespace bank

}; // namespace oltp

};
