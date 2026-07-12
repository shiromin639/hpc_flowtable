#include "stats.h"

#include <rte_lcore_var.h>

#include <string.h>

static RTE_LCORE_VAR_HANDLE(struct lcore_stats, g_lcore_stats);

int
stats_init(void)
{
    if (g_lcore_stats == NULL)
        RTE_LCORE_VAR_ALLOC(g_lcore_stats);

    if (g_lcore_stats == NULL)
        return -1;

    stats_reset_all();
    return 0;
}

void
stats_reset_all(void)
{
    if (g_lcore_stats == NULL)
        return;

    for (unsigned int lid = 0; lid < RTE_MAX_LCORE; lid++) {
        struct lcore_stats *stats =
            RTE_LCORE_VAR_LCORE(lid, g_lcore_stats);
        memset(stats, 0, sizeof(*stats));
    }
}

struct lcore_stats *
stats_get_current(void)
{
    return RTE_LCORE_VAR(g_lcore_stats);
}

struct lcore_stats *
stats_get_lcore(unsigned int lcore_id)
{
    return RTE_LCORE_VAR_LCORE(lcore_id, g_lcore_stats);
}

void
stats_collect_totals(struct lcore_stats *totals)
{
    unsigned int lid;

    memset(totals, 0, sizeof(*totals));

    RTE_LCORE_FOREACH(lid) {
        const struct lcore_stats *stats = stats_get_lcore(lid);

        totals->rx_pkts += stats->rx_pkts;
        totals->rx_bytes += stats->rx_bytes;
        totals->tx_pkts += stats->tx_pkts;
        totals->tx_bytes += stats->tx_bytes;
        totals->worker_rx_pkts += stats->worker_rx_pkts;
        totals->rx_filtered_pkts += stats->rx_filtered_pkts;
        totals->ring_drop_pkts += stats->ring_drop_pkts;
        totals->hash_add_failures += stats->hash_add_failures;
        totals->tx_drop_pkts += stats->tx_drop_pkts;
        totals->flows_created += stats->flows_created;
        totals->flows_deleted += stats->flows_deleted;
        totals->replacement_attempts += stats->replacement_attempts;
        totals->replacement_success += stats->replacement_success;
        totals->replacement_failures += stats->replacement_failures;
        totals->victim_evicted_flows += stats->victim_evicted_flows;
        totals->victim_cache_empty += stats->victim_cache_empty;
        totals->flow_add_retry_success += stats->flow_add_retry_success;
        totals->flow_add_retry_failures += stats->flow_add_retry_failures;
        totals->spi_pkts_forwarded += stats->spi_pkts_forwarded;
        totals->spi_pkts_dropped += stats->spi_pkts_dropped;
        totals->spi_rule_revalidations += stats->spi_rule_revalidations;
        totals->aging_scanned += stats->aging_scanned;
        totals->aging_expired += stats->aging_expired;
        totals->aging_deleted += stats->aging_deleted;
        totals->aging_reclaimed += stats->aging_reclaimed;
        totals->http_pkts += stats->http_pkts;
        totals->https_pkts += stats->https_pkts;
        totals->dns_pkts += stats->dns_pkts;
        totals->tcp_pkts += stats->tcp_pkts;
        totals->udp_pkts += stats->udp_pkts;
        totals->other_pkts += stats->other_pkts;
    }
}

double
stats_elapsed_seconds(uint64_t current_tsc, uint64_t previous_tsc,
        uint64_t tsc_hz)
{
    if (current_tsc <= previous_tsc || tsc_hz == 0)
        return 0.0;

    return (double)(current_tsc - previous_tsc) / (double)tsc_hz;
}

uint64_t
stats_rate_per_sec(uint64_t current, uint64_t previous, double elapsed_sec)
{
    if (current <= previous || elapsed_sec <= 0.0)
        return 0;

    return (uint64_t)((double)(current - previous) / elapsed_sec);
}

uint64_t
stats_mbps(uint64_t current_bytes, uint64_t previous_bytes,
        double elapsed_sec)
{
    if (current_bytes <= previous_bytes || elapsed_sec <= 0.0)
        return 0;

    return (uint64_t)(((double)(current_bytes - previous_bytes) * 8.0) /
            (elapsed_sec * 1000000.0));
}
