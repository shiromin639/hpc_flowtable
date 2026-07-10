#include "flow_table.h"
#include "spi_engine.h"
#include "stats.h"
#include "common.h"

#include <rte_hash.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <stdio.h>
#include <inttypes.h>

struct lcore_stats lcore_stats[RTE_MAX_LCORE];

int
stats_thread(__rte_unused void *arg)
{
    uint64_t prev_rx_pkts = 0, prev_tx_pkts = 0;
    uint64_t prev_rx_bytes = 0, prev_tx_bytes = 0;
    uint64_t prev_flows_created = 0;
    uint64_t prev_flows_deleted = 0;
    uint64_t prev_spi_drops = 0;
    uint64_t prev_tx_drops = 0;
    uint64_t prev_worker_tx_pkts[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_tx_bytes[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_spi_drops[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_tx_drops[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_revalidations[NUM_WORKERS] = { 0 };

    struct flow_table_ctx *ft = flow_table_get_ctx();
    unsigned int lcore_id = rte_lcore_id();

    flow_table_rcu_register(lcore_id);

    printf("Stats/Aging thread on lcore %u (RCU registered)\n", lcore_id);
    printf("\033[2J");

    uint32_t tick_counter = 0;
    uint64_t total_aged_this_cycle = 0;

    while (!force_quit) {
        rte_delay_us_sleep(AGING_INTERVAL_US);

        spi_rule_engine_reload_if_needed();

        uint32_t aged = flow_table_aging_tick();
        total_aged_this_cycle += aged;

        flow_table_rcu_quiescent(lcore_id);

        for (unsigned int reclaim_round = 0; reclaim_round < 1024;
                reclaim_round++) {
            unsigned int freed = 0;

            if (rte_hash_rcu_qsbr_dq_reclaim(ft->hash, &freed,
                        NULL, NULL) != 0 || freed == 0)
                break;
        }

        tick_counter++;

        if (tick_counter < AGING_NUM_CHUNKS)
            continue;

        lcore_stats[lcore_id].flows_deleted += total_aged_this_cycle;
        total_aged_this_cycle = 0;
        tick_counter = 0;

        uint64_t total_rx_pkts = 0, total_tx_pkts = 0;
        uint64_t total_rx_bytes = 0, total_tx_bytes = 0;
        uint64_t total_flows_created = 0, total_flows_deleted = 0;
        uint64_t total_spi_drops = 0, total_tx_drops = 0;
        uint64_t total_spi_forwarded = 0, total_revalidations = 0;
        uint64_t total_http = 0, total_https = 0, total_dns = 0;
        uint64_t total_tcp = 0, total_udp = 0, total_other = 0;

        unsigned int lid;
        RTE_LCORE_FOREACH(lid) {
            total_rx_pkts  += lcore_stats[lid].rx_pkts;
            total_tx_pkts  += lcore_stats[lid].tx_pkts;
            total_rx_bytes += lcore_stats[lid].rx_bytes;
            total_tx_bytes += lcore_stats[lid].tx_bytes;
            total_flows_created += lcore_stats[lid].flows_created;
            total_flows_deleted += lcore_stats[lid].flows_deleted;
            total_spi_drops += lcore_stats[lid].spi_pkts_dropped;
            total_tx_drops += lcore_stats[lid].tx_drop_pkts;
            total_spi_forwarded += lcore_stats[lid].spi_pkts_forwarded;
            total_revalidations += lcore_stats[lid].spi_rule_revalidations;
            total_http += lcore_stats[lid].http_pkts;
            total_https += lcore_stats[lid].https_pkts;
            total_dns += lcore_stats[lid].dns_pkts;
            total_tcp += lcore_stats[lid].tcp_pkts;
            total_udp += lcore_stats[lid].udp_pkts;
            total_other += lcore_stats[lid].other_pkts;
        }

        uint64_t pps_rx  = total_rx_pkts  - prev_rx_pkts;
        uint64_t pps_tx  = total_tx_pkts  - prev_tx_pkts;
        uint64_t mbps_rx = ((total_rx_bytes - prev_rx_bytes) * 8) / 1000000;
        uint64_t mbps_tx = ((total_tx_bytes - prev_tx_bytes) * 8) / 1000000;
        uint64_t cps = total_flows_created - prev_flows_created;
        uint64_t dps = total_flows_deleted - prev_flows_deleted;
        uint64_t spi_drop_ps = total_spi_drops - prev_spi_drops;
        uint64_t tx_drop_ps = total_tx_drops - prev_tx_drops;
        int32_t active_flows = rte_hash_count(ft->hash);

        printf("\033[1;1H\033[J");
        printf("================ PERFORMANCE STATS ================\n");
        printf("RX Throughput : %10"PRIu64" PPS | %10"PRIu64" Mbps\n",
               pps_rx, mbps_rx);
        printf("TX Throughput : %10"PRIu64" PPS | %10"PRIu64" Mbps\n",
               pps_tx, mbps_tx);
        printf("Active Flows  : %10d Flows\n", active_flows);
        printf("Created Flows : %10"PRIu64" Flows\n", total_flows_created);
        printf("Deleted/Timeout: %9"PRIu64" Flows\n", total_flows_deleted);
        printf("Flow Rate     : %10"PRIu64" C/s | %10"PRIu64" D/s\n",
               cps, dps);
        printf("SPI Drops     : %10"PRIu64" Pkts | %10"PRIu64" Pkts/s\n",
               total_spi_drops, spi_drop_ps);
        printf("TX Drops      : %10"PRIu64" Pkts | %10"PRIu64" Pkts/s\n",
               total_tx_drops, tx_drop_ps);
        printf("Active Rules  : %10"PRIu32" Rules | %10"PRIu32" Version\n",
               spi_rule_engine_rule_count(), spi_rule_engine_version());
        printf("SPI Forwarded : %10"PRIu64" Pkts | %10"PRIu64" Rechecks\n",
               total_spi_forwarded, total_revalidations);
        printf("Protocols     : HTTP=%"PRIu64" HTTPS=%"PRIu64" DNS=%"PRIu64
               " TCP=%"PRIu64" UDP=%"PRIu64" OTHER=%"PRIu64"\n",
               total_http, total_https, total_dns,
               total_tcp, total_udp, total_other);
        printf("===================================================\n\n");

        printf("================== WORKERS DETAILS ==================\n");

        for (int w = 0; w < NUM_WORKERS; w++) {
            unsigned int wl = worker_lcore_ids[w];
            uint64_t w_tx_pkts  = lcore_stats[wl].tx_pkts;
            uint64_t w_tx_bytes = lcore_stats[wl].tx_bytes;
            uint64_t w_spi_drops = lcore_stats[wl].spi_pkts_dropped;
            uint64_t w_tx_drops = lcore_stats[wl].tx_drop_pkts;
            uint64_t w_revalidations = lcore_stats[wl].spi_rule_revalidations;
            uint64_t w_pps = w_tx_pkts  - prev_worker_tx_pkts[w];
            uint64_t w_mbps = ((w_tx_bytes - prev_worker_tx_bytes[w]) * 8) / 1000000;
            uint64_t w_spi_drop_ps = w_spi_drops - prev_worker_spi_drops[w];
            uint64_t w_tx_drop_ps = w_tx_drops - prev_worker_tx_drops[w];
            uint64_t w_recheck_ps =
                w_revalidations - prev_worker_revalidations[w];

            printf("Worker %-3d lcore %-3u | TX %10"PRIu64" PPS %10"PRIu64" Mbps "
                   "| SPI_DROP %8"PRIu64"/s | TX_DROP %8"PRIu64"/s | RECHECK %8"PRIu64"/s\n",
                   w, wl, w_pps, w_mbps, w_spi_drop_ps, w_tx_drop_ps,
                   w_recheck_ps);
            printf("  HTTP=%"PRIu64" HTTPS=%"PRIu64" DNS=%"PRIu64
                   " TCP=%"PRIu64" UDP=%"PRIu64" OTHER=%"PRIu64"\n",
                   lcore_stats[wl].http_pkts,
                   lcore_stats[wl].https_pkts,
                   lcore_stats[wl].dns_pkts,
                   lcore_stats[wl].tcp_pkts,
                   lcore_stats[wl].udp_pkts,
                   lcore_stats[wl].other_pkts);

            prev_worker_tx_pkts[w]  = w_tx_pkts;
            prev_worker_tx_bytes[w] = w_tx_bytes;
            prev_worker_spi_drops[w] = w_spi_drops;
            prev_worker_tx_drops[w] = w_tx_drops;
            prev_worker_revalidations[w] = w_revalidations;
        }
        printf("=====================================================\n\n");
        fflush(stdout);

        prev_rx_pkts  = total_rx_pkts;
        prev_tx_pkts  = total_tx_pkts;
        prev_rx_bytes = total_rx_bytes;
        prev_tx_bytes = total_tx_bytes;
        prev_flows_created = total_flows_created;
        prev_flows_deleted = total_flows_deleted;
        prev_spi_drops = total_spi_drops;
        prev_tx_drops = total_tx_drops;
    }
    return 0;
}
