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
#include <rte_pause.h>
#include <stdio.h>
#include <string.h>

struct worker_tx_pending {
    struct rte_mbuf *pkts[BURST_SIZE];
    uint32_t pkt_lens[BURST_SIZE];
    uint16_t count;
};

static uint16_t
worker_flush_tx_pending(uint32_t worker_id, struct worker_tx_pending *pending,
        struct lcore_stats *stats)
{
    uint16_t sent_total = 0;
    uint16_t no_progress_retries = 0;

    while (sent_total < pending->count) {
        uint16_t sent = rte_eth_tx_burst(PORT_OUT, worker_id,
                &pending->pkts[sent_total], pending->count - sent_total);

        if (sent > 0) {
            stats->tx_pkts += sent;
            for (uint16_t i = 0; i < sent; i++)
                stats->tx_bytes += pending->pkt_lens[sent_total + i];

            sent_total += sent;
            no_progress_retries = 0;
            continue;
        }

        if (++no_progress_retries >= TX_RETRY_BUDGET)
            break;

        rte_pause();
    }

    if (sent_total == 0)
        return 0;

    if (sent_total < pending->count) {
        uint16_t remaining = pending->count - sent_total;

        memmove(pending->pkts, &pending->pkts[sent_total],
                remaining * sizeof(pending->pkts[0]));
        memmove(pending->pkt_lens, &pending->pkt_lens[sent_total],
                remaining * sizeof(pending->pkt_lens[0]));
        pending->count = remaining;
    } else {
        pending->count = 0;
    }

    return sent_total;
}

static void
worker_stage_tx_burst(struct worker_tx_pending *pending,
        struct rte_mbuf **tx_pkts, uint16_t tx_count,
        struct lcore_stats *stats)
{
    uint16_t space = BURST_SIZE - pending->count;
    uint16_t staged = tx_count < space ? tx_count : space;

    for (uint16_t i = 0; i < staged; i++) {
        pending->pkts[pending->count] = tx_pkts[i];
        pending->pkt_lens[pending->count] = tx_pkts[i]->pkt_len;
        pending->count++;
    }

    if (unlikely(staged < tx_count)) {
        stats->tx_drop_pkts += tx_count - staged;
        for (uint16_t i = staged; i < tx_count; i++)
            rte_pktmbuf_free(tx_pkts[i]);
    }
}

static void
worker_drop_tx_pending(struct worker_tx_pending *pending,
        struct lcore_stats *stats)
{
    if (pending->count == 0)
        return;

    stats->tx_drop_pkts += pending->count;
    for (uint16_t i = 0; i < pending->count; i++)
        rte_pktmbuf_free(pending->pkts[i]);

    pending->count = 0;
}

static void
worker_drain_tx_pending_on_exit(uint32_t worker_id,
        struct worker_tx_pending *pending, struct lcore_stats *stats)
{
    for (uint16_t i = 0; i < TX_RETRY_BUDGET && pending->count > 0; i++) {
        if (worker_flush_tx_pending(worker_id, pending, stats) == 0)
            rte_pause();
    }

    worker_drop_tx_pending(pending, stats);
}

int
worker_thread(void *arg)
{
    struct worker_args *wargs = (struct worker_args *)arg;
    uint32_t worker_id = wargs->worker_id;
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_mbuf *tx_pkts[BURST_SIZE];
    struct worker_tx_pending tx_pending = { 0 };
    uint16_t nb_rx;
    unsigned int lcore_id = rte_lcore_id();
    struct flow_table_ctx *ft = flow_table_get_ctx();
    struct lcore_stats *stats = stats_get_current();

    printf("Worker %u running on lcore %u\n", worker_id, lcore_id);

    while (!force_quit) {
        if (tx_pending.count > 0) {
            worker_flush_tx_pending(worker_id, &tx_pending, stats);
            if (tx_pending.count > 0) {
                rte_pause();
                continue;
            }
        }

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

        worker_stage_tx_burst(&tx_pending, tx_pkts, tx_count, stats);
        worker_flush_tx_pending(worker_id, &tx_pending, stats);
    }
    worker_drain_tx_pending_on_exit(worker_id, &tx_pending, stats);
    return 0;
}
