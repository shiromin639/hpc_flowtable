#ifndef STATS_H
#define STATS_H

#include "common.h"

/*
 * Per-lcore statistics.
 *
 * __rte_cache_aligned đảm bảo mỗi lcore_stats chiếm cache line riêng.
 * Nếu không align, 2 lcores' stats có thể nằm chung cache line
 * → false sharing: core A update rx_pkts invalidate cache của core B
 * đang update tx_pkts → thrashing.
 */
struct lcore_stats {
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t tx_drop_pkts;
    uint64_t flows_created;
    uint64_t flows_deleted;
    uint64_t spi_pkts_forwarded;
    uint64_t spi_pkts_dropped;
    uint64_t spi_rule_revalidations;
    uint64_t http_pkts;
    uint64_t https_pkts;
    uint64_t dns_pkts;
    uint64_t tcp_pkts;
    uint64_t udp_pkts;
    uint64_t other_pkts;
} __rte_cache_aligned;

extern struct lcore_stats port_stats[RTE_MAX_LCORE];

/* Thread functions */
int stats_thread(void *arg);

#endif /* STATS_H */
