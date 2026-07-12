#include "flow_table.h"
#include "flow_packet.h"
#include "stats.h"
#include "common.h"

#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <string.h>

static int
find_resolved_duplicate(const struct ipv4_5tuple_key *keys,
        const uint16_t *resolved_indices, uint16_t resolved_count,
        uint16_t key_index)
{
    for (uint16_t i = 0; i < resolved_count; i++) {
        if (memcmp(&keys[key_index], &keys[resolved_indices[i]],
                    sizeof(keys[key_index])) == 0)
            return (int)i;
    }

    return -1;
}

static void
stamp_mbuf_flow(struct rte_mbuf *m, uint32_t flow_idx, uint32_t flow_gen)
{
    m->hash.fdir.lo = flow_idx;
    m->hash.fdir.hi = flow_gen;
}

static int
publish_new_flow(struct flow_table_ctx *ft,
        const struct ipv4_5tuple_key *key, const void *key_ptr,
        int32_t flow_idx, uint64_t current_tsc,
        uint32_t *target_worker, uint32_t *flow_gen)
{
    if (unlikely((uint32_t)flow_idx >= ft->storage_entries))
        return -1;

    /*
     * Increment generation before overwriting cold[] so queued packets from
     * the previous slot owner fail worker validation instead of reading new
     * flow metadata.
     */
    *flow_gen = flow_hot_generation_next(&ft->hot[flow_idx]);
    flow_cold_from_key(&ft->cold[flow_idx], key, current_tsc);

    *target_worker = rte_hash_hash(ft->hash, key_ptr) % NUM_WORKERS;
    ft->hot[flow_idx].worker_id = (uint8_t)*target_worker;

    /* last_seen publishes the side-array slot to aging/replacement code. */
    flow_hot_last_seen_store(&ft->hot[flow_idx], current_tsc);
    return 0;
}

static int
dispatcher_evict_for_replacement(struct lcore_stats *stats,
        unsigned int lcore_id)
{
    int evicted;

    stats->replacement_attempts++;
    if (flow_table_victim_cache_count() == 0)
        stats->victim_cache_empty++;

    evicted = flow_table_evict_for_replacement(FLOW_REPLACEMENT_RETRIES,
            FLOW_EMERGENCY_SCAN_BUDGET);
    if (evicted > 0) {
        stats->replacement_success++;
        stats->victim_evicted_flows++;
        stats->flows_deleted++;
        flow_table_rcu_quiescent(lcore_id);
        stats->aging_reclaimed += flow_table_reclaim(
                FLOW_RECLAIM_REPLACEMENT_BUDGET);
        return 1;
    }

    stats->replacement_failures++;
    return 0;
}

int
dispatcher_thread(__rte_unused void *arg)
{
    struct rte_mbuf *pkts_burst[BURST_SIZE];
    struct rte_mbuf *worker_buffers[NUM_WORKERS][BURST_SIZE];
    uint16_t worker_counts[NUM_WORKERS];

    struct ipv4_5tuple_key keys[BURST_SIZE];
    const void *key_ptrs[BURST_SIZE];
    int32_t positions[BURST_SIZE];
    uint16_t resolved_indices[BURST_SIZE];
    int32_t resolved_positions[BURST_SIZE];
    uint32_t resolved_workers[BURST_SIZE];
    uint32_t resolved_generations[BURST_SIZE];

    unsigned int lcore_id = rte_lcore_id();
    struct flow_table_ctx *ft = flow_table_get_ctx();
    struct lcore_stats *stats = stats_get_current();

    // Register this thread as RCU reader 
    flow_table_rcu_register(lcore_id);

    printf("Dispatcher running on lcore %u (RCU registered)\n", lcore_id);

    while (!force_quit) {
        memset(worker_counts, 0, sizeof(worker_counts));

        const uint16_t nb_rx = rte_eth_rx_burst(PORT_IN, 0, pkts_burst, BURST_SIZE);
        if (unlikely(nb_rx == 0)) {
            flow_table_rcu_quiescent(lcore_id);
            continue;
        }

        stats->rx_pkts += nb_rx;
        uint64_t current_tsc = rte_rdtsc();

        // Extract 5-tuple keys from valid IPv4/TCP/UDP packets 
        uint16_t valid_pkts = 0;
        struct rte_mbuf *valid_mbufs[BURST_SIZE];

        for (int i = 0; i < nb_rx; i++) {
            // if (i + 3 < nb_rx) {
            //     rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[i + 3], void *));
            // }
            struct rte_mbuf *m = pkts_burst[i];
            int parse_ret;

            stats->rx_bytes += m->pkt_len;

            parse_ret = flow_packet_extract_key(rte_pktmbuf_mtod(m, void *),
                    rte_pktmbuf_data_len(m), &keys[valid_pkts]);
            if (unlikely(parse_ret != FLOW_PACKET_PARSE_OK)) {
                stats->rx_filtered_pkts++;
                rte_pktmbuf_free(m);
                continue;
            }

            valid_mbufs[valid_pkts] = m;
            key_ptrs[valid_pkts] = &keys[valid_pkts];
            valid_pkts++;
        }

        if (likely(valid_pkts > 0)) {
            uint16_t resolved_count = 0;
            rte_hash_lookup_bulk(ft->hash, key_ptrs, valid_pkts, positions);

            for (int i = 0; i < valid_pkts; i++) {
                struct rte_mbuf *m = valid_mbufs[i];
                int flow_idx = positions[i];
                uint32_t target_worker;
                uint32_t flow_gen;
                int duplicate_idx = find_resolved_duplicate(keys,
                        resolved_indices, resolved_count, i);

                if (unlikely(duplicate_idx >= 0)) {
                    flow_idx = resolved_positions[duplicate_idx];
                    target_worker = resolved_workers[duplicate_idx];
                    flow_gen = resolved_generations[duplicate_idx];
                    flow_hot_last_seen_store(&ft->hot[flow_idx], current_tsc);

                    stamp_mbuf_flow(m, (uint32_t)flow_idx, flow_gen);
                    worker_buffers[target_worker][worker_counts[target_worker]++] = m;
                    continue;
                }

                if (unlikely(flow_idx < 0)) {
                    /* New flow: add to hash table.
                     * Lock-free add uses CAS internally. */
                    flow_idx = rte_hash_add_key(ft->hash, key_ptrs[i]);

                    if (unlikely(flow_idx < 0)) {
                        stats->hash_add_failures++;
                        if (dispatcher_evict_for_replacement(stats,
                                    lcore_id)) {
                            flow_idx = rte_hash_add_key(ft->hash,
                                    key_ptrs[i]);
                            if (likely(flow_idx >= 0)) {
                                stats->flow_add_retry_success++;
                            } else {
                                stats->flow_add_retry_failures++;
                            }
                        }
                    }

                    if (likely(flow_idx >= 0)) {
                        if (publish_new_flow(ft, &keys[i], key_ptrs[i],
                                    flow_idx, current_tsc, &target_worker,
                                    &flow_gen) != 0) {
                            stats->hash_add_failures++;
                            rte_pktmbuf_free(m);
                            continue;
                        }
                        stats->flows_created++;
                    } else {
                        rte_pktmbuf_free(m);
                        continue;
                    }
                } else {
                    if (unlikely((uint32_t)flow_idx >= ft->storage_entries)) {
                        stats->hash_add_failures++;
                        rte_pktmbuf_free(m);
                        continue;
                    }
                    target_worker = ft->hot[flow_idx].worker_id;
                    flow_gen = flow_hot_generation_load(&ft->hot[flow_idx]);
                    flow_hot_last_seen_store(&ft->hot[flow_idx], current_tsc);
                }

                resolved_indices[resolved_count] = i;
                resolved_positions[resolved_count] = flow_idx;
                resolved_workers[resolved_count] = target_worker;
                resolved_generations[resolved_count] = flow_gen;
                resolved_count++;

                stamp_mbuf_flow(m, (uint32_t)flow_idx, flow_gen);
                worker_buffers[target_worker][worker_counts[target_worker]++] = m;
            }

            for (int w = 0; w < NUM_WORKERS; w++) {
                if (worker_counts[w] > 0) {
                    uint16_t sent = rte_ring_enqueue_burst(
                            worker_rings[w],
                            (void **)worker_buffers[w],
                            worker_counts[w], NULL);
                    if (unlikely(sent < worker_counts[w])) {
                        stats->ring_drop_pkts += worker_counts[w] - sent;
                        for (int j = sent; j < worker_counts[w]; j++)
                            rte_pktmbuf_free(worker_buffers[w][j]);
                    }
                }
            }
        }

        flow_table_rcu_quiescent(lcore_id);
    }
    flow_table_rcu_unregister(lcore_id);
    return 0;
}
