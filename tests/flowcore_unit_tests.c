#include "flow_packet.h"
#include "stats.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PACKET_LEN 64

static int failures;

static void
expect_true(bool condition, const char *message)
{
    if (condition)
        return;

    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void
build_ipv4_l4_packet(uint8_t *packet, uint8_t proto,
        uint32_t src_ip, uint32_t dst_ip,
        uint16_t src_port, uint16_t dst_port)
{
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)packet;
    struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    uint16_t *ports = (uint16_t *)(ipv4_hdr + 1);

    memset(packet, 0, TEST_PACKET_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    ipv4_hdr->version_ihl = RTE_IPV4_VHL_DEF;
    ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(*ipv4_hdr) + 20);
    ipv4_hdr->time_to_live = 64;
    ipv4_hdr->next_proto_id = proto;
    ipv4_hdr->src_addr = src_ip;
    ipv4_hdr->dst_addr = dst_ip;

    ports[0] = src_port;
    ports[1] = dst_port;
}

static void
test_extract_tcp_key(void)
{
    uint8_t packet[TEST_PACKET_LEN];
    struct ipv4_5tuple_key key;
    uint32_t src_ip = rte_cpu_to_be_32(0x0a000001);
    uint32_t dst_ip = rte_cpu_to_be_32(0xc0a80001);
    uint16_t src_port = rte_cpu_to_be_16(12345);
    uint16_t dst_port = rte_cpu_to_be_16(80);

    build_ipv4_l4_packet(packet, IPPROTO_TCP, src_ip, dst_ip,
            src_port, dst_port);

    expect_true(flow_packet_extract_key(packet, sizeof(packet), &key) ==
            FLOW_PACKET_PARSE_OK, "TCP packet should parse");
    expect_true(key.ip_src == src_ip, "TCP src IP should match");
    expect_true(key.ip_dst == dst_ip, "TCP dst IP should match");
    expect_true(key.port_src == src_port, "TCP src port should match");
    expect_true(key.port_dst == dst_port, "TCP dst port should match");
    expect_true(key.proto == IPPROTO_TCP, "TCP protocol should match");
}

static void
test_extract_udp_cold_and_protocol_stats(void)
{
    uint8_t packet[TEST_PACKET_LEN];
    struct flow_cold_data cold;
    struct lcore_stats stats;

    build_ipv4_l4_packet(packet, IPPROTO_UDP,
            rte_cpu_to_be_32(0x0a000002),
            rte_cpu_to_be_32(0x08080808),
            rte_cpu_to_be_16(53000),
            rte_cpu_to_be_16(53));

    expect_true(flow_packet_extract_cold(packet, sizeof(packet), &cold) ==
            FLOW_PACKET_PARSE_OK, "UDP packet should parse as cold data");

    memset(&stats, 0, sizeof(stats));
    flow_stats_account_protocol(&stats, &cold);
    expect_true(stats.dns_pkts == 1, "UDP/53 should increment DNS");
    expect_true(stats.udp_pkts == 0, "UDP/53 should not increment UDP");
}

static void
test_unsupported_and_truncated_packets(void)
{
    uint8_t packet[TEST_PACKET_LEN];
    struct ipv4_5tuple_key key;

    memset(packet, 0, sizeof(packet));
    ((struct rte_ether_hdr *)packet)->ether_type = rte_cpu_to_be_16(0x0806);
    expect_true(flow_packet_extract_key(packet, sizeof(packet), &key) ==
            FLOW_PACKET_PARSE_UNSUPPORTED, "ARP should be unsupported");

    build_ipv4_l4_packet(packet, IPPROTO_ICMP,
            rte_cpu_to_be_32(0x0a000001),
            rte_cpu_to_be_32(0x0a000002), 0, 0);
    expect_true(flow_packet_extract_key(packet, sizeof(packet), &key) ==
            FLOW_PACKET_PARSE_UNSUPPORTED, "ICMP should be unsupported");

    build_ipv4_l4_packet(packet, IPPROTO_TCP,
            rte_cpu_to_be_32(0x0a000001),
            rte_cpu_to_be_32(0x0a000002),
            rte_cpu_to_be_16(1000),
            rte_cpu_to_be_16(2000));
    expect_true(flow_packet_extract_key(packet,
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + 1,
                &key) == FLOW_PACKET_PARSE_TRUNCATED,
            "short TCP/UDP ports should be truncated");
}

static void
test_stats_rates(void)
{
    double elapsed = stats_elapsed_seconds(3000, 1000, 1000);

    expect_true(elapsed == 2.0, "elapsed seconds should use TSC delta");
    expect_true(stats_rate_per_sec(300, 100, elapsed) == 100,
            "packet rate should divide by elapsed seconds");
    expect_true(stats_mbps(500000, 0, 2.0) == 2,
            "Mbps should divide bit delta by elapsed seconds");
    expect_true(stats_rate_per_sec(100, 300, elapsed) == 0,
            "counter rollback should return zero rate");
}

int
main(void)
{
    test_extract_tcp_key();
    test_extract_udp_cold_and_protocol_stats();
    test_unsupported_and_truncated_packets();
    test_stats_rates();

    if (failures != 0) {
        fprintf(stderr, "flowcore unit tests failed: %d\n", failures);
        return 1;
    }

    printf("flowcore unit tests passed\n");
    return 0;
}
