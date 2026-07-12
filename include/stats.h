#ifndef STATS_H
#define STATS_H

#include "common.h"

struct lcore_stats {
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t worker_rx_pkts;
    uint64_t rx_filtered_pkts;
    uint64_t ring_drop_pkts;
    uint64_t hash_add_failures;
    uint64_t tx_drop_pkts;
    uint64_t flows_created;
    uint64_t flows_deleted;
    uint64_t spi_pkts_forwarded;
    uint64_t spi_pkts_dropped;
    uint64_t spi_rule_revalidations;
    uint64_t aging_scanned;
    uint64_t aging_expired;
    uint64_t aging_deleted;
    uint64_t aging_reclaimed;
    uint64_t http_pkts;
    uint64_t https_pkts;
    uint64_t dns_pkts;
    uint64_t tcp_pkts;
    uint64_t udp_pkts;
    uint64_t other_pkts;
} __rte_cache_aligned;

int stats_init(void);
void stats_reset_all(void);
struct lcore_stats *stats_get_current(void);
struct lcore_stats *stats_get_lcore(unsigned int lcore_id);
void stats_collect_totals(struct lcore_stats *totals);
double stats_elapsed_seconds(uint64_t current_tsc, uint64_t previous_tsc,
        uint64_t tsc_hz);
uint64_t stats_rate_per_sec(uint64_t current, uint64_t previous,
        double elapsed_sec);
uint64_t stats_mbps(uint64_t current_bytes, uint64_t previous_bytes,
        double elapsed_sec);
int stats_thread(void *arg);

#endif /* STATS_H */
