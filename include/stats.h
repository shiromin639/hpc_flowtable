#ifndef STATS_H
#define STATS_H

#include "common.h"

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

extern struct lcore_stats lcore_stats[RTE_MAX_LCORE];

int stats_thread(void *arg);

#endif /* STATS_H */
