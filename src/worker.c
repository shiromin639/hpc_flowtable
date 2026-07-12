/*
 * worker.c - Worker threads: SPI enforcement + protocol stats + TX
 */

#include "app_threads.h"
#include "common.h"
#include "flow_packet.h"
#include "flow_table.h"
#include "spi_engine.h"
#include "stats.h"

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_lcore.h>
#include <stdio.h>

int
worker_thread(void *arg)
{
    struct worker_args *wargs = (struct worker_args *)arg;
    uint32_t worker_id = wargs->worker_id;
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_mbuf *tx_pkts[BURST_SIZE];
    uint16_t nb_rx, nb_tx;
    unsigned int lcore_id = rte_lcore_id();
    struct flow_table_ctx *ft = flow_table_get_ctx();
    struct lcore_stats *stats = stats_get_current();

    printf("Worker %u running on lcore %u\n", worker_id, lcore_id);

    while (!force_quit) {
        nb_rx = rte_ring_dequeue_burst(worker_rings[worker_id],
                (void **)pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx == 0))
            continue;

        stats->worker_rx_pkts += nb_rx;
        uint16_t tx_count = 0;

        for (uint16_t i = 0; i < nb_rx; i++) {
            uint32_t flow_idx = pkts[i]->hash.fdir.lo;
            uint32_t flow_gen = pkts[i]->hash.fdir.hi;
            struct flow_cold_data parsed_cold;
            struct flow_cold_data slot_cold;
            const struct flow_cold_data *cold;
            uint8_t action;

            if (likely(flow_idx < ft->storage_entries &&
                       flow_hot_generation_load(&ft->hot[flow_idx]) == flow_gen)) {
                slot_cold = ft->cold[flow_idx];
                if (unlikely(flow_hot_generation_load(&ft->hot[flow_idx]) !=
                             flow_gen)) {
                    if (flow_packet_extract_cold(rte_pktmbuf_mtod(pkts[i],
                                    void *), rte_pktmbuf_data_len(pkts[i]),
                                &parsed_cold) != FLOW_PACKET_PARSE_OK) {
                        stats->spi_pkts_dropped++;
                        rte_pktmbuf_free(pkts[i]);
                        continue;
                    }

                    cold = &parsed_cold;
                } else {
                    cold = &slot_cold;
                }

                stats->spi_rule_checks++;
                action = spi_rule_engine_match_cold(cold);
            } else {
                if (flow_packet_extract_cold(rte_pktmbuf_mtod(pkts[i], void *),
                            rte_pktmbuf_data_len(pkts[i]),
                            &parsed_cold) != FLOW_PACKET_PARSE_OK) {
                    stats->spi_pkts_dropped++;
                    rte_pktmbuf_free(pkts[i]);
                    continue;
                }

                cold = &parsed_cold;
                stats->spi_rule_checks++;
                action = spi_rule_engine_match_cold(cold);
            }

            flow_stats_account_protocol(stats, cold);

            if (action == SPI_ACTION_DROP) {
                stats->spi_pkts_dropped++;
                rte_pktmbuf_free(pkts[i]);
                continue;
            }

            stats->spi_pkts_forwarded++;
            tx_pkts[tx_count++] = pkts[i];
        }

        if (unlikely(tx_count == 0))
            continue;

        nb_tx = rte_eth_tx_burst(PORT_OUT, worker_id, tx_pkts, tx_count);

        stats->tx_pkts += nb_tx;
        for (uint16_t i = 0; i < nb_tx; i++)
            stats->tx_bytes += tx_pkts[i]->pkt_len;

        if (unlikely(nb_tx < tx_count)) {
            stats->tx_drop_pkts += tx_count - nb_tx;
            for (uint16_t i = nb_tx; i < tx_count; i++)
                rte_pktmbuf_free(tx_pkts[i]);
        }
    }
    return 0;
}
