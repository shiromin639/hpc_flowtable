#include "flow_table.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
cold_equals(const struct flow_cold_data *a, const struct flow_cold_data *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static int
worker_accepts_slot(const struct flow_hot_data *hot, uint32_t packet_gen)
{
    return hot->flow_gen == packet_gen;
}

static void
set_cold(struct flow_cold_data *cold, uint32_t src_ip, uint32_t dst_ip,
        uint16_t src_port, uint16_t dst_port, uint8_t protocol,
        uint64_t create_time)
{
    memset(cold, 0, sizeof(*cold));
    cold->src_ip = src_ip;
    cold->dst_ip = dst_ip;
    cold->src_port = src_port;
    cold->dst_port = dst_port;
    cold->protocol = protocol;
    cold->create_time = create_time;
}

static int
unsafe_publication_order_has_stale_window(void)
{
    struct flow_hot_data hot;
    struct flow_cold_data cold;
    struct flow_cold_data old_cold;
    struct flow_cold_data new_cold;
    uint32_t queued_packet_gen = 10;

    memset(&hot, 0, sizeof(hot));
    hot.flow_gen = queued_packet_gen;
    set_cold(&old_cold, 0x0a000001, 0xc0a80001, 12345, 80, 6, 100);
    set_cold(&new_cold, 0x0a000002, 0xc0a80002, 23456, 443, 6, 200);
    cold = old_cold;

    /*
     * This mirrors the unsafe order this test guards against: reusable
     * side-array data is overwritten before flow_gen changes.
     */
    cold = new_cold;

    return worker_accepts_slot(&hot, queued_packet_gen) &&
        cold_equals(&cold, &new_cold);
}

static int
generation_first_order_rejects_stale_packet(void)
{
    struct flow_hot_data hot;
    struct flow_cold_data cold;
    struct flow_cold_data old_cold;
    struct flow_cold_data new_cold;
    uint32_t queued_packet_gen = 10;

    memset(&hot, 0, sizeof(hot));
    hot.flow_gen = queued_packet_gen;
    set_cold(&old_cold, 0x0a000001, 0xc0a80001, 12345, 80, 6, 100);
    set_cold(&new_cold, 0x0a000002, 0xc0a80002, 23456, 443, 6, 200);
    cold = old_cold;

    /*
     * A safer reuse order invalidates queued metadata before replacing
     * side-array contents. A real implementation should publish this with
     * appropriate atomic ordering or an explicit slot state.
     */
    hot.flow_gen++;
    cold = new_cold;

    return cold_equals(&cold, &new_cold) &&
        !worker_accepts_slot(&hot, queued_packet_gen);
}

int
main(void)
{
    if (!unsafe_publication_order_has_stale_window()) {
        fprintf(stderr, "expected unsafe order to expose stale window\n");
        return 1;
    }

    if (!generation_first_order_rejects_stale_packet()) {
        fprintf(stderr, "expected generation-first order to reject stale packet\n");
        return 1;
    }

    printf("flow slot lifecycle interleaving test passed\n");
    return 0;
}
