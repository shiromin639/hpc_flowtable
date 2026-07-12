#include "common.h"
#include "flow_table.h"

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_lcore.h>

#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

static int failures;
static unsigned int test_lcore_id;

static void
expect_true(bool condition, const char *message)
{
    if (condition)
        return;

    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static uint32_t
pct(uint32_t value, uint32_t percent)
{
    return (uint32_t)(((uint64_t)value * percent) / 100);
}

static void
make_key(struct ipv4_5tuple_key *key, uint32_t id)
{
    memset(key, 0, sizeof(*key));
    key->ip_src = rte_cpu_to_be_32(0x0a000001U + id);
    key->ip_dst = rte_cpu_to_be_32(0xc0a80001U + id);
    key->port_src = rte_cpu_to_be_16((uint16_t)(10000 + (id % 40000)));
    key->port_dst = rte_cpu_to_be_16((uint16_t)(1024 + (id % 50000)));
    key->proto = IPPROTO_TCP;
}

static void
publish_flow_metadata(int32_t position, const struct ipv4_5tuple_key *key,
        uint64_t last_seen)
{
    struct flow_table_ctx *ft = flow_table_get_ctx();

    flow_hot_generation_next(&ft->hot[position]);
    memset(&ft->cold[position], 0, sizeof(ft->cold[position]));
    ft->cold[position].src_ip = key->ip_src;
    ft->cold[position].dst_ip = key->ip_dst;
    ft->cold[position].src_port = key->port_src;
    ft->cold[position].dst_port = key->port_dst;
    ft->cold[position].protocol = key->proto;
    ft->cold[position].create_time = last_seen;
    ft->hot[position].worker_id = (uint8_t)(position % NUM_WORKERS);
    flow_hot_last_seen_store(&ft->hot[position], last_seen);
}

static int32_t
add_test_flow(uint32_t id, uint64_t last_seen)
{
    struct flow_table_ctx *ft = flow_table_get_ctx();
    struct ipv4_5tuple_key key;
    int32_t position;

    make_key(&key, id);
    position = rte_hash_add_key(ft->hash, &key);
    if (position < 0)
        return position;

    if ((uint32_t)position >= ft->storage_entries)
        return -ERANGE;

    publish_flow_metadata(position, &key, last_seen);
    return position;
}

static void
setup_table(void)
{
    int ret = flow_table_init(rte_socket_id());

    if (ret != 0) {
        fprintf(stderr, "FAIL: flow_table_init failed\n");
        failures++;
        return;
    }

    test_lcore_id = rte_lcore_id();
    flow_table_rcu_register(test_lcore_id);
}

static void
teardown_table(void)
{
    flow_table_rcu_unregister(test_lcore_id);
    flow_table_destroy();
}

static void
test_pressure_mode_thresholds(void)
{
    uint32_t capacity;

    setup_table();
    capacity = flow_table_capacity();

    expect_true(flow_table_pressure_mode(pct(capacity,
                    FLOW_PRESSURE_THRESHOLD_PCT) - 1) ==
            FLOW_PRESSURE_NORMAL,
            "below pressure threshold should be normal");
    expect_true(flow_table_pressure_mode(pct(capacity,
                    FLOW_PRESSURE_THRESHOLD_PCT)) ==
            FLOW_PRESSURE_PRESSURE,
            "pressure threshold should enter pressure mode");
    expect_true(flow_table_pressure_mode(pct(capacity,
                    FLOW_AGGRESSIVE_THRESHOLD_PCT)) ==
            FLOW_PRESSURE_AGGRESSIVE,
            "aggressive threshold should enter aggressive mode");
    expect_true(flow_table_pressure_mode(pct(capacity,
                    FLOW_CRITICAL_THRESHOLD_PCT)) ==
            FLOW_PRESSURE_CRITICAL,
            "critical threshold should enter critical mode");

    teardown_table();
}

static void
test_unpublished_slot_is_not_evicted(void)
{
    struct flow_table_ctx *ft;
    struct ipv4_5tuple_key key;
    struct flow_aging_result aged;
    struct flow_pressure_result pressure;
    int32_t position;
    int evicted;

    setup_table();
    ft = flow_table_get_ctx();
    make_key(&key, 1);

    position = rte_hash_add_key(ft->hash, &key);
    expect_true(position >= 0, "test setup should add unpublished key");

    aged = flow_table_aging_tick();
    pressure = flow_table_pressure_maintenance(FLOW_PRESSURE_CRITICAL,
            flow_table_capacity());
    evicted = flow_table_evict_for_replacement(0, flow_table_capacity());

    expect_true(aged.deleted == 0,
            "aging should not delete slot with last_seen=0");
    expect_true(pressure.evicted == 0,
            "pressure should not evict slot with last_seen=0");
    expect_true(evicted == 0,
            "emergency replacement should not evict slot with last_seen=0");
    expect_true(rte_hash_lookup(ft->hash, &key) == position,
            "unpublished key should remain present after maintenance");

    rte_hash_del_key(ft->hash, &key);
    flow_table_rcu_quiescent(test_lcore_id);
    flow_table_reclaim(16);
    teardown_table();
}

static void
test_pressure_maintenance_and_victim_cache(void)
{
    uint32_t capacity;
    uint32_t target;
    uint32_t desired;
    uint64_t now;
    int32_t before_count;
    int32_t after_pressure_count;
    int32_t after_replacement_count;
    struct flow_pressure_result pressure;
    uint32_t cached_before;
    int evicted;

    setup_table();
    capacity = flow_table_capacity();
    target = pct(capacity, FLOW_PRESSURE_TARGET_PCT);
    desired = target + 16;
    if (desired >= capacity)
        desired = capacity - 1;

    now = rte_rdtsc();
    for (uint32_t i = 0; i < desired; i++)
        expect_true(add_test_flow(1000 + i, now) >= 0,
                "test setup should add pressure flow");

    before_count = rte_hash_count(flow_table_get_ctx()->hash);
    pressure = flow_table_pressure_maintenance(FLOW_PRESSURE_CRITICAL,
            (uint32_t)before_count);
    flow_table_rcu_quiescent(test_lcore_id);
    flow_table_reclaim(128);
    after_pressure_count = rte_hash_count(flow_table_get_ctx()->hash);
    cached_before = flow_table_victim_cache_count();

    expect_true(pressure.scanned > 0,
            "pressure maintenance should scan entries");
    expect_true(pressure.evicted >= 0,
            "pressure maintenance could evict or leave to replacement path");
    expect_true(after_pressure_count <= before_count,
            "pressure maintenance might reduce hash count");
    expect_true(cached_before > 0,
            "pressure maintenance should fill victim cache after evict budget");

    evicted = flow_table_evict_for_replacement(FLOW_REPLACEMENT_RETRIES, 0);
    flow_table_rcu_quiescent(test_lcore_id);
    flow_table_reclaim(128);
    after_replacement_count = rte_hash_count(flow_table_get_ctx()->hash);

    expect_true(evicted > 0,
            "replacement should evict cached victim(s)");
    expect_true(after_replacement_count <= after_pressure_count - 1,
            "cached replacement should delete additional flow(s)");

    teardown_table();
}

static void
test_replacement_allows_add_after_full_table(void)
{
    struct flow_table_ctx *ft;
    uint32_t capacity;
    uint64_t now;
    uint32_t inserted = 0;
    bool add_failed = false;
    int32_t before_count;
    int32_t new_position;
    int evicted;

    setup_table();
    ft = flow_table_get_ctx();
    capacity = flow_table_capacity();
    now = rte_rdtsc();

    for (uint32_t i = 0; i < capacity * 4; i++) {
        int32_t position = add_test_flow(100000 + i, now);

        if (position < 0) {
            add_failed = true;
            break;
        }
        inserted++;
    }

    expect_true(inserted > 0, "test setup should insert flows");
    expect_true(add_failed, "small hash table should eventually reject add");

    before_count = rte_hash_count(ft->hash);
    evicted = flow_table_evict_for_replacement(0, capacity);
    flow_table_rcu_quiescent(test_lcore_id);
    flow_table_reclaim(capacity);

    new_position = add_test_flow(900000, now);

    expect_true(evicted > 0,
            "emergency replacement should evict at least one flow from full table");
    expect_true(new_position >= 0,
            "new flow should be addable after replacement and reclaim");
    expect_true(rte_hash_count(ft->hash) <= before_count,
            "full-table replacement should keep active count stable or lower");

    teardown_table();
}

static int
init_eal(void)
{
    char arg0[] = "flow_table_overload_tests";
    char arg1[] = "--no-huge";
    char arg2[] = "--no-pci";
    char arg3[] = "--no-shconf";
    char arg4[] = "--no-telemetry";
    char *argv[] = { arg0, arg1, arg2, arg3, arg4 };
    int ret;

    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    ret = rte_eal_init((int)ARRAY_LEN(argv), argv);
    if (ret < 0) {
        fprintf(stderr, "FAIL: rte_eal_init failed: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }

    return 0;
}

int
main(void)
{
    if (init_eal() != 0)
        return 1;

    test_pressure_mode_thresholds();
    test_unpublished_slot_is_not_evicted();
    test_pressure_maintenance_and_victim_cache();
    test_replacement_allows_add_after_full_table();

    rte_eal_cleanup();

    if (failures != 0) {
        fprintf(stderr, "flow table overload tests failed: %d\n", failures);
        return 1;
    }

    printf("flow table overload tests passed\n");
    return 0;
}
