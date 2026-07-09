/*
 * main.c - DPDK Flow Table Application Entry Point
 *
 * Initializes EAL, ports, flow table (with RCU), and launches threads.
 * All flow table logic is in flow_table.c, dispatcher in dispatcher.c, etc.
 */

#include "common.h"
#include "flow_table.h"
#include "spi_engine.h"
#include "stats.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

/* Global state (declared extern in common.h) */
volatile uint8_t force_quit;
struct rte_mempool *mbuf_pool;
struct rte_ring *worker_rings[NUM_WORKERS];
unsigned int worker_lcore_ids[NUM_WORKERS];

static const struct rte_eth_conf port_conf_default = {
    .rxmode = { .mq_mode = RTE_ETH_MQ_RX_NONE },
    .txmode = { .mq_mode = RTE_ETH_MQ_TX_NONE },
};

/* External thread functions */
extern int dispatcher_thread(void *arg);
extern int worker_thread(void *arg);

/* Worker args - needs to match worker.c definition */
struct main_worker_args {
    uint32_t worker_id;
};

static uint32_t
getenv_u32_or_default(const char *name, uint32_t default_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || *value == '\0')
        return default_value;

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
        parsed > UINT32_MAX) {
        printf("Ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (uint32_t)parsed;
}

static const char *
getenv_str_or_default(const char *name, const char *default_value)
{
    const char *value = getenv(name);

    if (value == NULL || *value == '\0')
        return default_value;

    return value;
}

static inline int
ports_init(void)
{
    int ret;
    uint16_t rx_rings = 1, tx_rings = 1;
    struct rte_eth_conf port_conf = port_conf_default;
    uint16_t nb_rxd = RX_DESC_PER_QUEUE;
    uint16_t nb_txd = TX_DESC_PER_QUEUE;
    uint16_t q;
    int socket_id_in = rte_eth_dev_socket_id(PORT_IN);
    int socket_id_out = rte_eth_dev_socket_id(PORT_OUT);

    /* Setup PORT_IN */
    ret = rte_eth_dev_configure(PORT_IN, rx_rings, tx_rings, &port_conf);
    if (ret != 0)
        return ret;
    for (q = 0; q < rx_rings; q++) {
        ret = rte_eth_rx_queue_setup(PORT_IN, q, nb_rxd,
                socket_id_in, NULL, mbuf_pool);
        if (ret < 0) {
            printf("Cannot setup rx queue for port in\n");
            return ret;
        }
    }
    rte_eth_tx_queue_setup(PORT_IN, 0, nb_txd, socket_id_in, NULL);

    /* Setup PORT_OUT with NUM_WORKERS TX queues */
    tx_rings = NUM_WORKERS;
    ret = rte_eth_dev_configure(PORT_OUT, rx_rings, tx_rings, &port_conf);
    if (ret != 0) {
        printf("Cannot configure port out\n");
        return ret;
    }
    rte_eth_rx_queue_setup(PORT_OUT, 0, nb_rxd, socket_id_out,
            NULL, mbuf_pool);
    for (q = 0; q < tx_rings; q++) {
        ret = rte_eth_tx_queue_setup(PORT_OUT, q, nb_txd,
                socket_id_out, NULL);
        if (ret != 0)
            printf("Cannot setup tx queue %d for port out\n", q);
    }

    /* Start ports */
    ret = rte_eth_dev_start(PORT_IN);
    if (ret != 0) {
        printf("Cannot start port in\n");
        return ret;
    }
    ret = rte_eth_dev_start(PORT_OUT);
    if (ret != 0) {
        printf("Cannot start port out\n");
        return ret;
    }

    rte_eth_promiscuous_enable(PORT_IN);
    rte_eth_promiscuous_enable(PORT_OUT);
    return 0;
}

static void
signal_handler(int sig_num)
{
    if (sig_num == SIGUSR1) {
        spi_rule_engine_request_reload();
        return;
    }

    /* 
     * Only set the flag - printf is not async-signal-safe,
     * but in DPDK apps it's commonly used and works in practice.
     */
    if (force_quit)
        /* Second signal: force exit immediately */
        _exit(128 + sig_num);
    printf("\nExiting on signal %d (press again to force)...\n", sig_num);
    force_quit = 1;
}

int
main(int argc, char *argv[])
{
    int ret;
    unsigned int nb_ports;
    unsigned int lcore_id;
    uint32_t mbuf_count = getenv_u32_or_default("FLOWCORE_NUM_MBUFS",
            NUM_MBUFS);
    const char *rules_path = getenv_str_or_default("FLOWCORE_RULES_PATH",
            SPI_RULES_PATH);

    /*
     * sigaction() instead of signal():
     * - signal() behavior is undefined in multi-threaded programs
     * - sigaction() is reliable and portable
     * - SA_RESETHAND: second Ctrl+C uses default handler (kills process)
     */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot init EAL\n");

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 1)
        rte_exit(EXIT_FAILURE, "Not enough ports\n");

    if (rte_lcore_count() < NUM_WORKERS + 2)
        rte_exit(EXIT_FAILURE, "Need at least %d cores "
                 "(1 dispatcher + %d workers + 1 stats)\n",
                 NUM_WORKERS + 2, NUM_WORKERS);

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
            mbuf_count * nb_ports, MBUF_CACHE_SIZE, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    ret = ports_init();
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init ports\n");

    /* Initialize flow table: lock-free hash + RCU + NUMA-aware arrays */
    ret = flow_table_init(rte_socket_id());
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init flow table\n");

    ret = spi_rule_engine_init(rules_path);
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init SPI rule engine\n");

    /* Create per-worker rings (SP/SC for single-producer/consumer) */
    for (int i = 0; i < NUM_WORKERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "worker_ring_%d", i);
        worker_rings[i] = rte_ring_create(name, RING_SIZE,
                rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (worker_rings[i] == NULL)
            rte_exit(EXIT_FAILURE, "Cannot create ring %d\n", i);
    }

    force_quit = 0;
    static struct main_worker_args wargs[NUM_WORKERS];
    uint32_t w_id = 0;

    /* Launch stats/aging thread on first worker lcore */
    unsigned int stats_lcore_id = rte_get_next_lcore(-1, 1, 0);
    rte_eal_remote_launch(stats_thread, NULL, stats_lcore_id);

    /* Launch worker threads */
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (lcore_id == stats_lcore_id)
            continue;
        if (w_id >= NUM_WORKERS)
            break;
        wargs[w_id].worker_id = w_id;
        worker_lcore_ids[w_id] = lcore_id;
        rte_eal_remote_launch(worker_thread, &wargs[w_id], lcore_id);
        w_id++;
    }

    /* Dispatcher runs on main lcore */
    dispatcher_thread(NULL);

    /* Cleanup */
    rte_eal_mp_wait_lcore();
    for (int i = 0; i < NUM_WORKERS; i++)
        rte_ring_free(worker_rings[i]);

    flow_table_destroy();
    spi_rule_engine_destroy();

    return 0;
}
