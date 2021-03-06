#pragma once

#include <cstddef>

// tx parameters

// whether to emulate FaSST's protocol
/* #undef EM_FASST */
#ifndef EM_FASST
#define EM_FASST 0
#endif

// whether to only execute execution phase
/* #undef INLINE_OVERWRITE */
#ifndef INLINE_OVERWRITE
#define INLINE_OVERWRITE 0 // in default, not store value in the index
#endif
#define INLINE_OVERWRITE_MAX_PAYLOAD 16 // only can inline 16 byte data in index

// whether to only execute execution phase
/* #undef TX_ONLY_EXE */
#ifndef TX_ONLY_EXE
#define TX_ONLY_EXE 0
#endif

/**
 * 1: use RPC as logging
 * 2: use one-sided WRITE as logging
 */
#define TX_LOG_STYLE 2
#ifndef TX_LOG_STYLE
#define TX_LOG_STYLE 1
#endif

// whether to use a backup store
#define TX_BACKUP_STORE 1


// Data store releated configurations
// whether offload read to one-sided operation
#define ONE_SIDED_READ

// whether to use cache for one-sided operations
#define RDMA_CACHE_
#ifndef RDMA_CACHE
#define RDMA_CACHE 0
#endif

#define USE_RDMA_COMMIT NULL
/* #undef USE_DSLR */
#ifndef USE_DSLR
#define USE_DSLR 0
#endif

// whether to use the common transaction algorithm interface
/* #undef ENABLE_TXN_API */
#ifndef ENABLE_TXN_API
#define ENABLE_TXN_API 1
#endif

#define RDMA_STORE_SIZE 10000
#ifndef RDMA_STORE_SIZE
#define RDMA_STORE_SIZE 8
#endif

#define PA NULL
#ifndef PA
#define PA 0 // in default, not using passive ack optimization, since its more tricky in buffer management
#endif

/* #undef OR */
#ifndef OR
#define OR 0 // by defaut, outstanding requests are not used
#endif


// other TX specific configurations
#define RECORD_STALE NULL

/* #undef LARGE_CONNECTION */
#ifndef LARGE_CONNECTION
#define LARGE_CONNECTION 0 // in default, we donot use more QPs to connect to other servers
#endif

#define QP_NUMS 5
#if !LARGE_CONNECTION
#undef QP_NUMS
#define QP_NUMS 1
#endif

// whether to check the functionality of RTX
/* #undef CHECKS */
#ifndef CHECKS
#define CHECKS 0
#endif

// execuation related configuration
#define NO_ABORT 0           // does not retry if the TX is abort, and do the execution
#define OCC_RETRY            // OCC does not issue another round of checks
#define OCC_RO_CHECK 1       // whether do ro validation of OCC

#define PROFILE_RW_SET 0     // whether profile TX's read/write set
#define PROFILE_SERVER_NUM 0 // whether profile number of server accessed per TX

/*****some sanity setting *********/
#if RDMA_CACHE
static_assert(ONE_SIDED_READ,"RTX's RDMA location cache must work with an RDMA friendly store.");
#endif

#if ENABLE_TXN_API
static_assert(!EM_FASST, "The RTX's transactional algorithm api support for EM_FASST has not been implemented.");
#endif
