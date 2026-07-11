/*
 * worker.c - Worker threads: SPI enforcement + protocol stats + TX
 */

#include "app_threads.h"
#include "common.h"
#include "flow_table.h"
#include "spi_engine.h"
#include "stats.h"

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_lcore.h>
#include <rte_byteorder.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static inline void
account_protocol_stats(struct lcore_stats *stats,
        const struct flow_cold_data *cold)
{
    if (cold->protocol == IPPROTO_TCP) {
        if (cold->dst_port == rte_cpu_to_be_16(80))
            stats->http_pkts++;
        else if (cold->dst_port == rte_cpu_to_be_16(443))
            stats->https_pkts++;
        else
            stats->tcp_pkts++;
        return;
    }

    if (cold->protocol == IPPROTO_UDP) {
        if (cold->dst_port == rte_cpu_to_be_16(53))
            stats->dns_pkts++;
        else
            stats->udp_pkts++;
        return;
    }

    stats->other_pkts++;
}

static inline int
extract_packet_cold(struct rte_mbuf *m, struct flow_cold_data *cold)
{
    struct rte_ether_hdr *eth_hdr =
        rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ipv4_hdr;
    uint8_t l3_len;
    uint16_t *ports;

    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return -1;

    ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    l3_len = rte_ipv4_hdr_len(ipv4_hdr);
    if (ipv4_hdr->next_proto_id != IPPROTO_TCP &&
        ipv4_hdr->next_proto_id != IPPROTO_UDP)
        return -1;

    if (l3_len < sizeof(struct rte_ipv4_hdr) ||
        rte_pktmbuf_data_len(m) <
        sizeof(*eth_hdr) + l3_len + sizeof(uint16_t) * 2)
        return -1;

    memset(cold, 0, sizeof(*cold));
    cold->src_ip = ipv4_hdr->src_addr;
    cold->dst_ip = ipv4_hdr->dst_addr;
    cold->protocol = ipv4_hdr->next_proto_id;

    ports = (uint16_t *)((unsigned char *)ipv4_hdr + l3_len);
    cold->src_port = ports[0];
    cold->dst_port = ports[1];

    return 0;
}

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
    struct lcore_stats *stats = &lcore_stats[lcore_id];

    printf("Worker %u running on lcore %u\n", worker_id, lcore_id);

    while (!force_quit) {
        nb_rx = rte_ring_dequeue_burst(worker_rings[worker_id],
                (void **)pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx == 0))
            continue;

        uint16_t tx_count = 0;
        uint32_t active_rule_version = spi_rule_engine_version();

        for (uint16_t i = 0; i < nb_rx; i++) {
            uint32_t flow_idx = pkts[i]->hash.fdir.lo;
            uint32_t flow_gen = pkts[i]->hash.fdir.hi;
            struct flow_cold_data parsed_cold;
            const struct flow_cold_data *cold;
            uint8_t action;

            if (likely(flow_idx < ft->storage_entries &&
                       flow_hot_generation_load(&ft->hot[flow_idx]) == flow_gen)) {
                cold = &ft->cold[flow_idx];
                if (unlikely(ft->hot[flow_idx].action_version !=
                             active_rule_version))
                    stats->spi_rule_revalidations++;
                action = spi_rule_engine_eval(ft, flow_idx);
            } else {
                if (extract_packet_cold(pkts[i], &parsed_cold) != 0) {
                    stats->spi_pkts_dropped++;
                    rte_pktmbuf_free(pkts[i]);
                    continue;
                }

                cold = &parsed_cold;
                stats->spi_rule_revalidations++;
                action = spi_rule_engine_match_cold(cold);
            }

            account_protocol_stats(stats, cold);

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
