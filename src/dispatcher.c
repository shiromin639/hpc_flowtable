#include "flow_table.h"
#include "flow_packet.h"
#include "spi_engine.h"
#include "stats.h"
#include "common.h"

#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <string.h>

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
                int duplicate_idx = -1;

                for (uint16_t j = 0; j < resolved_count; j++) {
                    if (memcmp(&keys[i], &keys[resolved_indices[j]], sizeof(keys[i])) == 0) {
                        duplicate_idx = j;
                        break;
                    }
                }

                if (unlikely(duplicate_idx >= 0)) {
                    flow_idx = resolved_positions[duplicate_idx];
                    target_worker = resolved_workers[duplicate_idx];
                    flow_gen = resolved_generations[duplicate_idx];
                    flow_hot_last_seen_store(&ft->hot[flow_idx], current_tsc);

                    m->hash.fdir.lo = (uint32_t)flow_idx;
                    m->hash.fdir.hi = flow_gen;
                    worker_buffers[target_worker][worker_counts[target_worker]++] = m;
                    continue;
                }

                if (unlikely(flow_idx < 0)) {
                    /* New flow: add to hash table.
                     * Lock-free add uses CAS internally. */
                    flow_idx = rte_hash_add_key(ft->hash, key_ptrs[i]);

                    if (likely(flow_idx >= 0)) {
                        if (unlikely((uint32_t)flow_idx >= ft->storage_entries)) {
                            stats->hash_add_failures++;
                            rte_pktmbuf_free(m);
                            continue;
                        }

                        /*
                         * Invalidate any queued mbuf metadata for an older
                         * occupant before replacing the side-array contents.
                         */
                        flow_gen = flow_hot_generation_next(&ft->hot[flow_idx]);

                        flow_hot_last_seen_store(&ft->hot[flow_idx], current_tsc);

                        flow_cold_from_key(&ft->cold[flow_idx], &keys[i],
                                current_tsc);

                        hash_sig_t hash_val = rte_hash_hash(ft->hash, key_ptrs[i]);
                        target_worker = hash_val % NUM_WORKERS;

                        ft->hot[flow_idx].worker_id = target_worker;
                        ft->hot[flow_idx].spi_action = SPI_ACTION_UNKNOWN;
                        ft->hot[flow_idx].action_version = 0;
                        stats->flows_created++;
                    } else {
                        stats->hash_add_failures++;
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

                m->hash.fdir.lo = (uint32_t)flow_idx;
                m->hash.fdir.hi = flow_gen;
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
