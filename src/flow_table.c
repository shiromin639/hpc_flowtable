/*
 * flow_table.c - Flow Table with RCU + Hot/Cold split + Chunked Aging
 *
 * Concurrency: rte_hash (lock-free) + rte_rcu_qsbr
 * Data layout: hot_data[] (64B aligned) + cold_data[] (64B aligned)
 * Aging: chunked iteration + batch delete
 */

#include "flow_table.h"
#include "common.h"
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_rcu_qsbr.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_lcore.h>
#include <stdio.h>
#include <string.h>

static struct flow_table_ctx g_ft_ctx;

int
flow_table_init(int socket_id)
{
    memset(&g_ft_ctx, 0, sizeof(g_ft_ctx));

    /*
     * Lock-free hash: RW_CONCURRENCY_LF uses CAS instead of spinlock.
     * CRC32 hardware hash (~5 cycles) vs jhash software (~15 cycles).
     */
    struct rte_hash_parameters hash_params = {
        .name = "flow_table_lf",
        .entries = HASH_ENTRIES,
        .key_len = sizeof(struct ipv4_5tuple_key),
        .hash_func = rte_hash_crc,
        .hash_func_init_val = 0,
        .socket_id = socket_id,
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF
    };

    g_ft_ctx.hash = rte_hash_create(&hash_params);
    if (!g_ft_ctx.hash) {
        printf("Cannot create lock-free hash table\n");
        return -1;
    }

    int32_t max_key_id = rte_hash_max_key_id(g_ft_ctx.hash);
    if (max_key_id < 0) {
        printf("Cannot query max key id from hash table\n");
        rte_hash_free(g_ft_ctx.hash);
        return -1;
    }
    g_ft_ctx.storage_entries = (uint32_t)max_key_id + 1;

    /* Allocate hot/cold arrays sized to the hash key-id range. */
    g_ft_ctx.hot = rte_zmalloc_socket("flow_hot",
            sizeof(struct flow_hot_data) * g_ft_ctx.storage_entries,
            RTE_CACHE_LINE_SIZE, socket_id);
    if (!g_ft_ctx.hot) {
        printf("Cannot allocate flow hot data\n");
        rte_hash_free(g_ft_ctx.hash);
        return -1;
    }

    g_ft_ctx.cold = rte_zmalloc_socket("flow_cold",
            sizeof(struct flow_cold_data) * g_ft_ctx.storage_entries,
            RTE_CACHE_LINE_SIZE, socket_id);
    if (!g_ft_ctx.cold) {
        printf("Cannot allocate flow cold data\n");
        rte_free(g_ft_ctx.hot);
        rte_hash_free(g_ft_ctx.hash);
        return -1;
    }

    /*
     * RCU QSBR setup: each reader thread reports quiescent state
     * after each burst. Writer (aging) can only reclaim entries
     * after ALL readers have gone quiescent.
     */
    size_t qsv_sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    g_ft_ctx.qsv = rte_zmalloc_socket("rcu_qsbr", qsv_sz,
            RTE_CACHE_LINE_SIZE, socket_id);
    if (!g_ft_ctx.qsv) {
        printf("Cannot allocate RCU QSBR\n");
        rte_hash_free(g_ft_ctx.hash);
        rte_free(g_ft_ctx.cold);
        rte_free(g_ft_ctx.hot);
        return -1;
    }

    if (rte_rcu_qsbr_init(g_ft_ctx.qsv, RTE_MAX_LCORE) != 0) {
        printf("Cannot init RCU QSBR\n");
        goto fail_qsv;
    }

    /*
     * Attach RCU to rte_hash: after this, del_key() defers reclamation
     * until all readers report quiescent state.
     * DQ mode = hash manages its own deferred queue internally.
     */
    struct rte_hash_rcu_config rcu_cfg = {
        .v = g_ft_ctx.qsv,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .dq_size = RCU_DQ_SIZE,
    };

    if (rte_hash_rcu_qsbr_add(g_ft_ctx.hash, &rcu_cfg) != 0) {
        printf("Cannot attach RCU to hash table\n");
        goto fail_qsv;
    }

    g_ft_ctx.current_chunk = 0;
    printf("[FlowTable] Init OK: %d hash entries, %u storage slots, LF+RCU, %d aging chunks\n",
           HASH_ENTRIES, g_ft_ctx.storage_entries, AGING_NUM_CHUNKS);
    return 0;

fail_qsv:
    rte_free(g_ft_ctx.qsv);
    rte_hash_free(g_ft_ctx.hash);
    rte_free(g_ft_ctx.cold);
    rte_free(g_ft_ctx.hot);
    return -1;
}

void
flow_table_destroy(void)
{
    if (g_ft_ctx.hash)
        rte_hash_free(g_ft_ctx.hash);
    if (g_ft_ctx.qsv)
        rte_free(g_ft_ctx.qsv);
    if (g_ft_ctx.hot)
        rte_free(g_ft_ctx.hot);
    if (g_ft_ctx.cold)
        rte_free(g_ft_ctx.cold);
    memset(&g_ft_ctx, 0, sizeof(g_ft_ctx));
}

void
flow_table_rcu_register(unsigned int thread_id)
{
    rte_rcu_qsbr_thread_register(g_ft_ctx.qsv, thread_id);
    rte_rcu_qsbr_thread_online(g_ft_ctx.qsv, thread_id);
}

void
flow_table_rcu_quiescent(unsigned int thread_id)
{
    rte_rcu_qsbr_quiescent(g_ft_ctx.qsv, thread_id);
}

/*
 * Chunked aging: scan 1/AGING_NUM_CHUNKS of the table per tick.
 *
 * Instead of scanning all 1M entries every second (latency spike),
 * we divide into 8 chunks, scan 1 chunk every 125ms.
 * Expired keys are collected into a batch buffer, then deleted together.
 */
uint32_t
flow_table_aging_tick(void)
{
    uint64_t timeout_cycles = (uint64_t)FLOW_TIMEOUT_SEC * rte_get_tsc_hz();
    uint64_t t_now = rte_rdtsc();

    const void *expired_keys[AGING_BATCH_SIZE];
    uint32_t expired_count = 0;
    uint32_t total_deleted = 0;
    uint32_t chunk = g_ft_ctx.current_chunk;

    const void *next_key;
    void *next_data;
    uint32_t iter = 0;
    int32_t position;

    while ((position = rte_hash_iterate(g_ft_ctx.hash,
                    &next_key, &next_data, &iter)) >= 0) {

        /* Only process entries belonging to current chunk */
        if (((uint32_t)position % AGING_NUM_CHUNKS) != chunk)
            continue;

        uint64_t last_seen = g_ft_ctx.hot[position].last_seen;

        if (likely(t_now > last_seen) &&
            unlikely((t_now - last_seen) > timeout_cycles)) {

            expired_keys[expired_count++] = next_key;

            /* Flush batch when buffer full */
            if (expired_count >= AGING_BATCH_SIZE) {
                for (uint32_t i = 0; i < expired_count; i++) {
                    int ret = rte_hash_del_key(g_ft_ctx.hash,
                                               expired_keys[i]);
                    if (likely(ret >= 0))
                        total_deleted++;
                }
                expired_count = 0;
            }
        }
    }

    /* Flush remaining */
    for (uint32_t i = 0; i < expired_count; i++) {
        int ret = rte_hash_del_key(g_ft_ctx.hash, expired_keys[i]);
        if (likely(ret >= 0))
            total_deleted++;
    }

    g_ft_ctx.current_chunk = (chunk + 1) % AGING_NUM_CHUNKS;
    return total_deleted;
}

struct flow_table_ctx *
flow_table_get_ctx(void)
{
    return &g_ft_ctx;
}
