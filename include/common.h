#ifndef COMMON_H
#define COMMON_H

#include <rte_common.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <stdint.h>

/* ============================================================
 * Cấu hình hệ thống
 * ============================================================ */
#define NUM_MBUFS      262143 
#define NUM_WORKERS     4
#define MBUF_CACHE_SIZE 250
#define RX_DESC_PER_QUEUE 1024
#define TX_DESC_PER_QUEUE 1024
#define RING_SIZE       4096
#define HASH_ENTRIES    (1024 * 1024)   /* 1M entries */
#define BURST_SIZE      32

#define PORT_IN         0
#define PORT_OUT        1

#define SPI_RULES_PATH  "rules.cfg"
#define SPI_MAX_RULES   256

/* ============================================================
 * Flow aging configuration
 * ============================================================ */
#define FLOW_TIMEOUT_SEC    1

/*
 * AGING_NUM_CHUNKS: Chia flow table thành N chunks cho aging.
 * Mỗi tick (125ms), aging thread scan 1 chunk → toàn bộ table
 * được scan hết trong N × 125ms = 1 giây (với N=8).
 * 
 * Lợi ích: Thay vì scan toàn bộ 1M entries mỗi giây (gây latency spike),
 * mỗi tick chỉ scan ~131K entries → latency đều, không block stats display.
 */
#define AGING_NUM_CHUNKS    8

/*
 * AGING_BATCH_SIZE: Max expired keys collected before batch delete.
 * Increased to 1024 to handle large flow tables (1M entries / 8 chunks
 * = 131K per chunk, may have thousands of expired entries).
 */
#define AGING_BATCH_SIZE    1024

/*
 * RCU_DQ_SIZE: Deferred queue size for RCU reclamation.
 * Must be large enough to hold all pending deletions between
 * quiescent cycles. With 1M max flows, worst case is all flows
 * expiring at once. We set it equal to HASH_ENTRIES.
 */
#define RCU_DQ_SIZE         HASH_ENTRIES

/* Aging interval: 1 giây / AGING_NUM_CHUNKS = 125ms per chunk */
#define AGING_INTERVAL_US   (1000000 / AGING_NUM_CHUNKS)

/* ============================================================
 * Shared globals (defined in main.c)
 * ============================================================ */
extern volatile uint8_t force_quit;
extern struct rte_mempool *mbuf_pool;
extern struct rte_ring *worker_rings[NUM_WORKERS];
extern unsigned int worker_lcore_ids[NUM_WORKERS];

#endif /* COMMON_H */
