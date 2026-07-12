#include "flow_table.h"
#include "common.h"
#include <rte_build_config.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_hash_crc.h>
#include <rte_rcu_qsbr.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_lcore.h>
#include <rte_spinlock.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <rte_random.h>

static struct flow_table_ctx g_ft_ctx;

struct flow_victim_cache {
    rte_spinlock_t lock;
    struct flow_victim_candidate entries[FLOW_VICTIM_CACHE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};

static struct flow_victim_cache g_victim_cache;

static uint32_t
flow_pct(uint32_t value, uint32_t pct)
{
    return (uint32_t)(((uint64_t)value * pct) / 100);
}

static void
victim_cache_reset(void)
{
    rte_spinlock_init(&g_victim_cache.lock);
    memset(g_victim_cache.entries, 0, sizeof(g_victim_cache.entries));
    g_victim_cache.head = 0;
    g_victim_cache.tail = 0;
    g_victim_cache.count = 0;
}

static int
victim_cache_push(const struct flow_victim_candidate *candidate)
{
    int ret = 0;

    rte_spinlock_lock(&g_victim_cache.lock);
    if (g_victim_cache.count >= FLOW_VICTIM_CACHE_SIZE) {
        ret = -ENOSPC;
    } else {
        g_victim_cache.entries[g_victim_cache.tail] = *candidate;
        g_victim_cache.tail = (g_victim_cache.tail + 1) %
            FLOW_VICTIM_CACHE_SIZE;
        g_victim_cache.count++;
    }
    rte_spinlock_unlock(&g_victim_cache.lock);

    return ret;
}

static int
victim_cache_pop(struct flow_victim_candidate *candidate)
{
    int ret = 0;

    rte_spinlock_lock(&g_victim_cache.lock);
    if (g_victim_cache.count == 0) {
        ret = -ENOENT;
    } else {
        *candidate = g_victim_cache.entries[g_victim_cache.head];
        g_victim_cache.head = (g_victim_cache.head + 1) %
            FLOW_VICTIM_CACHE_SIZE;
        g_victim_cache.count--;
    }
    rte_spinlock_unlock(&g_victim_cache.lock);

    return ret;
}

static int
candidate_delete_if_valid(const struct flow_victim_candidate *candidate,
        uint64_t now_tsc, uint64_t min_idle_cycles)
{
    int32_t position;
    uint64_t last_seen;

    position = rte_hash_lookup(g_ft_ctx.hash, &candidate->key);
    if (position < 0 || (uint32_t)position != candidate->position)
        return 0;

    if (unlikely((uint32_t)position >= g_ft_ctx.storage_entries))
        return 0;

    if (flow_hot_generation_load(&g_ft_ctx.hot[position]) !=
            candidate->flow_gen)
        return 0;

    last_seen = flow_hot_last_seen_load(&g_ft_ctx.hot[position]);
    if (last_seen == 0)
        return 0;

    if (min_idle_cycles > 0 &&
        (!(now_tsc > last_seen) ||
         (now_tsc - last_seen) <= min_idle_cycles))
        return 0;

    return rte_hash_del_key(g_ft_ctx.hash, &candidate->key) >= 0;
}

static int
candidate_collect_if_eligible(struct flow_victim_candidate *candidate,
        const void *next_key, uint32_t position, uint64_t now_tsc,
        uint64_t min_idle_cycles)
{
    uint64_t last_seen;

    if (unlikely(position >= g_ft_ctx.storage_entries))
        return 0;

    last_seen = flow_hot_last_seen_load(&g_ft_ctx.hot[position]);
    if (last_seen == 0)
        return 0;

    if (min_idle_cycles > 0 &&
        (!(now_tsc > last_seen) ||
         (now_tsc - last_seen) <= min_idle_cycles))
        return 0;

    memset(candidate, 0, sizeof(*candidate));
    memcpy(&candidate->key, next_key, sizeof(candidate->key));
    candidate->position = position;
    candidate->flow_gen = flow_hot_generation_load(&g_ft_ctx.hot[position]);
    candidate->last_seen = last_seen;
    return 1;
}

static uint64_t
pressure_min_idle_cycles(enum flow_pressure_mode mode)
{
    uint64_t timeout_cycles = (uint64_t)FLOW_TIMEOUT_SEC * rte_get_tsc_hz();

    switch (mode) {
    case FLOW_PRESSURE_PRESSURE:
        return timeout_cycles / 2;
    case FLOW_PRESSURE_AGGRESSIVE:
        return timeout_cycles / 4;
    case FLOW_PRESSURE_CRITICAL:
        return 0;
    case FLOW_PRESSURE_NORMAL:
    default:
        return timeout_cycles;
    }
}

static uint32_t
pressure_scan_budget(enum flow_pressure_mode mode)
{
    uint32_t base = (g_ft_ctx.storage_entries + AGING_NUM_CHUNKS - 1) /
        AGING_NUM_CHUNKS;
    uint64_t budget = base;

    if (base == 0)
        base = 1;

    switch (mode) {
    case FLOW_PRESSURE_PRESSURE:
        budget = base;
        break;
    case FLOW_PRESSURE_AGGRESSIVE:
        budget = (uint64_t)base * 2;
        break;
    case FLOW_PRESSURE_CRITICAL:
        budget = (uint64_t)base * 4;
        break;
    case FLOW_PRESSURE_NORMAL:
    default:
        budget = 0;
        break;
    }

    if (budget > g_ft_ctx.storage_entries)
        budget = g_ft_ctx.storage_entries;

    return (uint32_t)budget;
}

static uint32_t
pressure_evict_budget(enum flow_pressure_mode mode, uint32_t active_flows)
{
    uint32_t capacity = flow_table_capacity();
    uint32_t target = flow_pct(capacity, FLOW_PRESSURE_TARGET_PCT);

    if (mode == FLOW_PRESSURE_NORMAL || active_flows <= target)
        return 0;

    return active_flows - target;
}

static uint32_t
flush_expired_batch(const void **expired_keys, const uint32_t *expired_positions,
        uint32_t expired_count, uint64_t t_now, uint64_t timeout_cycles)
{
    uint32_t total_deleted = 0;

    for (uint32_t i = 0; i < expired_count; i++) {
        uint32_t position = expired_positions[i];
        uint64_t last_seen;
        int ret;

        if (unlikely(position >= g_ft_ctx.storage_entries))
            continue;

        last_seen = flow_hot_last_seen_load(&g_ft_ctx.hot[position]);
        if (last_seen == 0)
            continue;

        if (!(t_now > last_seen) || (t_now - last_seen) <= timeout_cycles)
            continue;

        ret = rte_hash_del_key(g_ft_ctx.hash, expired_keys[i]);
        if (likely(ret >= 0))
            total_deleted++;
    }

    return total_deleted;
}

int
flow_table_init(int socket_id)
{
    memset(&g_ft_ctx, 0, sizeof(g_ft_ctx));
    victim_cache_reset();

    struct rte_hash_parameters hash_params = {
        .name = "flow_table_lf",
        .entries = HASH_ENTRIES,
        .key_len = sizeof(struct ipv4_5tuple_key),
        .hash_func = rte_hash_crc,
        .hash_func_init_val = 0,
        .socket_id = socket_id,
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF | RTE_HASH_EXTRA_FLAGS_EXT_TABLE
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

    struct rte_hash_rcu_config rcu_cfg = {
        .v = g_ft_ctx.qsv,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .dq_size = RCU_DQ_SIZE,
    };

    if (rte_hash_rcu_qsbr_add(g_ft_ctx.hash, &rcu_cfg) != 0) {
        printf("Cannot attach RCU to hash table\n");
        goto fail_qsv;
    }

    g_ft_ctx.aging_iter = 0;
    g_ft_ctx.pressure_iter = 0;
    g_ft_ctx.emergency_iter = 0;
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
    victim_cache_reset();
}

void
flow_table_rcu_register(unsigned int thread_id)
{
    if (g_ft_ctx.qsv == NULL)
        return;

    rte_rcu_qsbr_thread_register(g_ft_ctx.qsv, thread_id);
    rte_rcu_qsbr_thread_online(g_ft_ctx.qsv, thread_id);
}

void
flow_table_rcu_online(unsigned int thread_id)
{
    if (g_ft_ctx.qsv == NULL)
        return;

    rte_rcu_qsbr_thread_online(g_ft_ctx.qsv, thread_id);
}

void
flow_table_rcu_offline(unsigned int thread_id)
{
    if (g_ft_ctx.qsv == NULL)
        return;

    rte_rcu_qsbr_thread_offline(g_ft_ctx.qsv, thread_id);
}

void
flow_table_rcu_quiescent(unsigned int thread_id)
{
    if (g_ft_ctx.qsv == NULL)
        return;

    rte_rcu_qsbr_quiescent(g_ft_ctx.qsv, thread_id);
}

void
flow_table_rcu_unregister(unsigned int thread_id)
{
    if (g_ft_ctx.qsv == NULL)
        return;

    rte_rcu_qsbr_thread_offline(g_ft_ctx.qsv, thread_id);
    rte_rcu_qsbr_thread_unregister(g_ft_ctx.qsv, thread_id);
}

struct flow_aging_result
flow_table_aging_tick(void)
{
    uint64_t timeout_cycles = (uint64_t)FLOW_TIMEOUT_SEC * rte_get_tsc_hz();
    uint64_t t_now = rte_rdtsc();
    struct flow_aging_result result = { 0 };

    const void *expired_keys[AGING_BATCH_SIZE];
    uint32_t expired_positions[AGING_BATCH_SIZE];
    uint32_t expired_count = 0;
    uint32_t total_deleted = 0;
    uint32_t scan_budget;

    const void *next_key;
    void *next_data;
    int32_t position;

    scan_budget = (g_ft_ctx.storage_entries + AGING_NUM_CHUNKS - 1) /
        AGING_NUM_CHUNKS;
    if (scan_budget == 0)
        scan_budget = 1;

    while (result.scanned < scan_budget) {
        position = rte_hash_iterate(g_ft_ctx.hash,
                &next_key, &next_data, &g_ft_ctx.aging_iter);

        if (position == -ENOENT) {
            g_ft_ctx.aging_iter = 0;
            break;
        }

        if (position < 0)
            break;

        result.scanned++;

        if (unlikely((uint32_t)position >= g_ft_ctx.storage_entries))
            continue;

        uint64_t last_seen = flow_hot_last_seen_load(&g_ft_ctx.hot[position]);

        if (likely(t_now > last_seen) &&
            unlikely((t_now - last_seen) > timeout_cycles)) {

            expired_keys[expired_count++] = next_key;
            expired_positions[expired_count - 1] = (uint32_t)position;
            result.expired++;

            /* Flush batch when buffer full */
            if (expired_count >= AGING_BATCH_SIZE) {
                total_deleted += flush_expired_batch(expired_keys,
                        expired_positions, expired_count, t_now,
                        timeout_cycles);
                expired_count = 0;
            }
        }
    }

    total_deleted += flush_expired_batch(expired_keys, expired_positions,
            expired_count, t_now, timeout_cycles);

    result.deleted = total_deleted;
    return result;
}

enum flow_pressure_mode
flow_table_pressure_mode(uint32_t active_flows)
{
    uint32_t capacity = flow_table_capacity();

    if (capacity == 0)
        return FLOW_PRESSURE_NORMAL;

    if (active_flows >= flow_pct(capacity, FLOW_CRITICAL_THRESHOLD_PCT))
        return FLOW_PRESSURE_CRITICAL;
    if (active_flows >= flow_pct(capacity, FLOW_AGGRESSIVE_THRESHOLD_PCT))
        return FLOW_PRESSURE_AGGRESSIVE;
    if (active_flows >= flow_pct(capacity, FLOW_PRESSURE_THRESHOLD_PCT))
        return FLOW_PRESSURE_PRESSURE;

    return FLOW_PRESSURE_NORMAL;
}

const char *
flow_table_pressure_mode_name(enum flow_pressure_mode mode)
{
    switch (mode) {
    case FLOW_PRESSURE_PRESSURE:
        return "PRESSURE";
    case FLOW_PRESSURE_AGGRESSIVE:
        return "AGGRESSIVE";
    case FLOW_PRESSURE_CRITICAL:
        return "CRITICAL";
    case FLOW_PRESSURE_NORMAL:
    default:
        return "NORMAL";
    }
}

struct flow_pressure_result
flow_table_pressure_maintenance(enum flow_pressure_mode mode,
        uint32_t active_flows)
{
    struct flow_pressure_result result = { 0 };
    uint64_t now_tsc = rte_rdtsc();
    uint64_t min_idle_cycles;
    uint32_t scan_budget;
    uint32_t evict_budget;
    uint32_t candidate_budget = FLOW_VICTIM_FILL_BATCH;
    const void *next_key;
    void *next_data;
    int32_t position;

    if (mode == FLOW_PRESSURE_NORMAL || g_ft_ctx.hash == NULL)
        return result;

    scan_budget = pressure_scan_budget(mode);
    evict_budget = pressure_evict_budget(mode, active_flows);
    min_idle_cycles = pressure_min_idle_cycles(mode);

    while (result.scanned < scan_budget) {
        struct flow_victim_candidate candidate;

        position = rte_hash_iterate(g_ft_ctx.hash,
                &next_key, &next_data, &g_ft_ctx.pressure_iter);

        if (position == -ENOENT) {
            g_ft_ctx.pressure_iter = 0;
            break;
        }

        if (position < 0)
            break;

        result.scanned++;

        if (!candidate_collect_if_eligible(&candidate, next_key,
                    (uint32_t)position, now_tsc, min_idle_cycles))
            continue;

        if (result.evicted < evict_budget) {
            if (candidate_delete_if_valid(&candidate, now_tsc,
                        min_idle_cycles)) {
                result.evicted++;
                if (active_flows > 0)
                    active_flows--;
            } else {
                result.stale++;
            }
            continue;
        }

        if (result.candidates < candidate_budget &&
            victim_cache_push(&candidate) == 0) {
            result.candidates++;
        }
    }

    return result;
}


int
flow_table_evict_for_replacement(uint32_t max_cached_attempts,
        uint32_t emergency_scan_budget)
{
    uint64_t now_tsc = rte_rdtsc();
    struct flow_victim_candidate candidate;
    uint32_t pop_attempts = max_cached_attempts;

    if (pop_attempts == 0)
        pop_attempts = 1;

    for (uint32_t i = 0; i < pop_attempts; i++) {
        if (victim_cache_pop(&candidate) != 0)
            break;

        if (candidate_delete_if_valid(&candidate, now_tsc, 0))
            return 1;
    }

    if (emergency_scan_budget > 0) {
        int batch_evicted = 0;
        for (int b = 0; b < 16; b++) {
            uint32_t best_pos = 0;
            uint64_t oldest_time = (uint64_t)-1;
            int found = 0;

            for (int i = 0; i < 4; i++) {
                uint32_t pos = rte_rand() % g_ft_ctx.storage_entries;
                uint64_t last_seen = flow_hot_last_seen_load(&g_ft_ctx.hot[pos]);
                if (last_seen != 0 && last_seen < oldest_time) {
                    oldest_time = last_seen;
                    best_pos = pos;
                    found = 1;
                }
            }

            if (found) {
                memset(&candidate, 0, sizeof(candidate));
                candidate.key.ip_src = g_ft_ctx.cold[best_pos].src_ip;
                candidate.key.ip_dst = g_ft_ctx.cold[best_pos].dst_ip;
                candidate.key.port_src = g_ft_ctx.cold[best_pos].src_port;
                candidate.key.port_dst = g_ft_ctx.cold[best_pos].dst_port;
                candidate.key.proto = g_ft_ctx.cold[best_pos].protocol;
                candidate.position = best_pos;
                candidate.flow_gen = flow_hot_generation_load(&g_ft_ctx.hot[best_pos]);
                candidate.last_seen = oldest_time;
                
                if (candidate_delete_if_valid(&candidate, now_tsc, 0)) {
                    batch_evicted++;
                }
            }
        }
        if (batch_evicted > 0)
            return 1;
    }

    return 0;
}

uint64_t
flow_table_reclaim(uint32_t max_rounds)
{
    uint64_t reclaimed = 0;

    if (g_ft_ctx.hash == NULL)
        return 0;

    for (uint32_t round = 0; round < max_rounds; round++) {
        unsigned int freed = 0;

        if (rte_hash_rcu_qsbr_dq_reclaim(g_ft_ctx.hash, &freed,
                    NULL, NULL) != 0 || freed == 0)
            break;

        reclaimed += freed;
    }

    return reclaimed;
}

uint32_t
flow_table_victim_cache_count(void)
{
    uint32_t count;

    rte_spinlock_lock(&g_victim_cache.lock);
    count = g_victim_cache.count;
    rte_spinlock_unlock(&g_victim_cache.lock);

    return count;
}

uint32_t
flow_table_capacity(void)
{
    return g_ft_ctx.storage_entries;
}

struct flow_table_ctx *
flow_table_get_ctx(void)
{
    return &g_ft_ctx;
}
