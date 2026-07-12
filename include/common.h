#ifndef COMMON_H
#define COMMON_H

#include <rte_common.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <stdint.h>
#include <signal.h>

#define NUM_MBUFS 32768 
#define NUM_WORKERS     4
#define MBUF_CACHE_SIZE 250
#define RX_DESC_PER_QUEUE 1024
#define TX_DESC_PER_QUEUE 1024
#define RING_SIZE       4096
#ifndef HASH_ENTRIES
#define HASH_ENTRIES    (1024 * 1024)   /* 1M entries */
#endif
#define BURST_SIZE      32

#define PORT_IN         0
#define PORT_OUT        1

#define SPI_RULES_PATH  "rules.cfg"
#define SPI_MAX_RULES   256

#define FLOW_TIMEOUT_SEC  5 

#define AGING_NUM_CHUNKS    8
#define AGING_BATCH_SIZE    1024

#ifndef RCU_DQ_SIZE
#define RCU_DQ_SIZE         HASH_ENTRIES
#endif

#define AGING_INTERVAL_US   (1000000 / AGING_NUM_CHUNKS)

#define FLOW_PRESSURE_THRESHOLD_PCT    92
#define FLOW_AGGRESSIVE_THRESHOLD_PCT  96
#define FLOW_CRITICAL_THRESHOLD_PCT    99
#define FLOW_PRESSURE_TARGET_PCT       92
#define FLOW_VICTIM_CACHE_SIZE         8192
#define FLOW_VICTIM_FILL_BATCH         512
#define FLOW_REPLACEMENT_RETRIES       8
#define FLOW_EMERGENCY_SCAN_BUDGET     512
#define FLOW_RECLAIM_REPLACEMENT_BUDGET 4
#define FLOW_RECLAIM_BUDGET            64

extern volatile sig_atomic_t force_quit;
extern struct rte_mempool *mbuf_pool;
extern struct rte_ring *worker_rings[NUM_WORKERS];
extern unsigned int worker_lcore_ids[NUM_WORKERS];

#endif /* COMMON_H */
