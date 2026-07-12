#include "flow_packet.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>

#include <netinet/in.h>
#include <string.h>

int
flow_packet_extract_key(const void *data, uint32_t data_len,
        struct ipv4_5tuple_key *key)
{
    const struct rte_ether_hdr *eth_hdr;
    const struct rte_ipv4_hdr *ipv4_hdr;
    const uint16_t *ports;
    uint8_t l3_len;

    if (data_len < sizeof(*eth_hdr))
        return FLOW_PACKET_PARSE_TRUNCATED;

    eth_hdr = (const struct rte_ether_hdr *)data;
    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return FLOW_PACKET_PARSE_UNSUPPORTED;

    if (data_len < sizeof(*eth_hdr) + sizeof(*ipv4_hdr))
        return FLOW_PACKET_PARSE_TRUNCATED;

    ipv4_hdr = (const struct rte_ipv4_hdr *)(eth_hdr + 1);
    l3_len = rte_ipv4_hdr_len(ipv4_hdr);
    if (l3_len < sizeof(*ipv4_hdr))
        return FLOW_PACKET_PARSE_TRUNCATED;

    if ((rte_be_to_cpu_16(ipv4_hdr->fragment_offset) &
                (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0)
        return FLOW_PACKET_PARSE_UNSUPPORTED;

    if (ipv4_hdr->next_proto_id != IPPROTO_TCP &&
        ipv4_hdr->next_proto_id != IPPROTO_UDP)
        return FLOW_PACKET_PARSE_UNSUPPORTED;

    if (data_len < sizeof(*eth_hdr) + l3_len + sizeof(uint16_t) * 2)
        return FLOW_PACKET_PARSE_TRUNCATED;

    ports = (const uint16_t *)((const unsigned char *)ipv4_hdr + l3_len);

    memset(key, 0, sizeof(*key));
    key->ip_src = ipv4_hdr->src_addr;
    key->ip_dst = ipv4_hdr->dst_addr;
    key->port_src = ports[0];
    key->port_dst = ports[1];
    key->proto = ipv4_hdr->next_proto_id;

    return FLOW_PACKET_PARSE_OK;
}

int
flow_packet_extract_cold(const void *data, uint32_t data_len,
        struct flow_cold_data *cold)
{
    struct ipv4_5tuple_key key;
    int ret = flow_packet_extract_key(data, data_len, &key);

    if (ret != FLOW_PACKET_PARSE_OK)
        return ret;

    flow_cold_from_key(cold, &key, 0);
    return FLOW_PACKET_PARSE_OK;
}

void
flow_cold_from_key(struct flow_cold_data *cold,
        const struct ipv4_5tuple_key *key, uint64_t create_time)
{
    memset(cold, 0, sizeof(*cold));
    cold->src_ip = key->ip_src;
    cold->dst_ip = key->ip_dst;
    cold->src_port = key->port_src;
    cold->dst_port = key->port_dst;
    cold->protocol = key->proto;
    cold->create_time = create_time;
}

void
flow_stats_account_protocol(struct lcore_stats *stats,
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
