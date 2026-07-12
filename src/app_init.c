#include "app_init.h"

#include "app_threads.h"
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

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t force_quit;
struct rte_mempool *mbuf_pool;
struct rte_ring *worker_rings[NUM_WORKERS];
unsigned int worker_lcore_ids[NUM_WORKERS];

static const struct rte_eth_conf port_conf_default = {
    .rxmode = { .mq_mode = RTE_ETH_MQ_RX_NONE },
    .txmode = { .mq_mode = RTE_ETH_MQ_TX_NONE },
};

static struct worker_args g_worker_args[NUM_WORKERS];

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

static int
ports_init(void)
{
    int ret;
    uint16_t rx_rings = 1;
    uint16_t tx_rings = 1;
    struct rte_eth_conf port_conf = port_conf_default;
    uint16_t nb_rxd = RX_DESC_PER_QUEUE;
    uint16_t nb_txd = TX_DESC_PER_QUEUE;
    uint16_t q;
    int socket_id_in = rte_eth_dev_socket_id(PORT_IN);
    int socket_id_out = rte_eth_dev_socket_id(PORT_OUT);

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
    ret = rte_eth_tx_queue_setup(PORT_IN, 0, nb_txd, socket_id_in, NULL);
    if (ret < 0) {
        printf("Cannot setup tx queue for port in\n");
        return ret;
    }

    tx_rings = NUM_WORKERS;
    ret = rte_eth_dev_configure(PORT_OUT, rx_rings, tx_rings, &port_conf);
    if (ret != 0) {
        printf("Cannot configure port out\n");
        return ret;
    }

    ret = rte_eth_rx_queue_setup(PORT_OUT, 0, nb_rxd, socket_id_out,
            NULL, mbuf_pool);
    if (ret < 0) {
        printf("Cannot setup rx queue for port out\n");
        return ret;
    }
    for (q = 0; q < tx_rings; q++) {
        ret = rte_eth_tx_queue_setup(PORT_OUT, q, nb_txd,
                socket_id_out, NULL);
        if (ret != 0) {
            printf("Cannot setup tx queue %u for port out\n", q);
            return ret;
        }
    }

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

static int
rings_init(void)
{
    for (int i = 0; i < NUM_WORKERS; i++) {
        char name[32];

        snprintf(name, sizeof(name), "worker_ring_%d", i);
        worker_rings[i] = rte_ring_create(name, RING_SIZE,
                rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (worker_rings[i] == NULL) {
            printf("Cannot create ring %d\n", i);
            return -1;
        }
    }

    return 0;
}

static int
launch_threads(void)
{
    unsigned int lcore_id;
    uint32_t w_id = 0;
    unsigned int stats_lcore_id = rte_get_next_lcore(-1, 1, 0);

    if (rte_eal_remote_launch(stats_thread, NULL, stats_lcore_id) != 0) {
        printf("Cannot launch stats thread on lcore %u\n", stats_lcore_id);
        return -1;
    }

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (lcore_id == stats_lcore_id)
            continue;
        if (w_id >= NUM_WORKERS)
            break;

        g_worker_args[w_id].worker_id = w_id;
        worker_lcore_ids[w_id] = lcore_id;
        if (rte_eal_remote_launch(worker_thread, &g_worker_args[w_id],
                    lcore_id) != 0) {
            printf("Cannot launch worker %u on lcore %u\n", w_id, lcore_id);
            return -1;
        }
        w_id++;
    }

    if (w_id != NUM_WORKERS) {
        printf("Started %u workers, expected %u\n", w_id, NUM_WORKERS);
        return -1;
    }

    return 0;
}

static void
signal_handler(int sig_num)
{
    if (sig_num == SIGUSR1) {
        spi_rule_engine_request_reload();
        return;
    }

    if (force_quit)
        _exit(128 + sig_num);
    printf("\nExiting on signal %d (press again to force)...\n", sig_num);
    force_quit = 1;
}

int
app_install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0 ||
        sigaction(SIGUSR1, &sa, NULL) != 0) {
        perror("sigaction");
        return -1;
    }

    return 0;
}

int
app_init(int argc, char *argv[])
{
    int ret;
    unsigned int nb_ports;
    uint32_t mbuf_count = getenv_u32_or_default("FLOWCORE_NUM_MBUFS",
            NUM_MBUFS);
    uint32_t mbuf_data_size = getenv_u32_or_default("FLOWCORE_MBUF_DATA_SIZE",
            RTE_MBUF_DEFAULT_BUF_SIZE);
    const char *rules_path = getenv_str_or_default("FLOWCORE_RULES_PATH",
            SPI_RULES_PATH);

    force_quit = 0;
    memset(worker_rings, 0, sizeof(worker_rings));
    memset(worker_lcore_ids, 0, sizeof(worker_lcore_ids));
    memset(g_worker_args, 0, sizeof(g_worker_args));
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot init EAL\n");

    ret = stats_init();
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init stats\n");

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 2)
        rte_exit(EXIT_FAILURE, "Need at least 2 ports (PORT_IN=0, PORT_OUT=1)\n");

    if (rte_lcore_count() < NUM_WORKERS + 2)
        rte_exit(EXIT_FAILURE, "Need at least %d cores "
                 "(1 dispatcher + %d workers + 1 stats)\n",
                 NUM_WORKERS + 2, NUM_WORKERS);

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
            mbuf_count * nb_ports, MBUF_CACHE_SIZE, 0,
            mbuf_data_size, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    ret = ports_init();
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init ports\n");

    ret = flow_table_init(rte_socket_id());
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init flow table\n");

    ret = spi_rule_engine_init(rules_path);
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init SPI rule engine\n");

    ret = rings_init();
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot init worker rings\n");

    ret = launch_threads();
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot launch worker/stats threads\n");

    return 0;
}

int
app_run(void)
{
    return dispatcher_thread(NULL);
}

void
app_cleanup(void)
{
    rte_eal_mp_wait_lcore();

    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_rings[i] != NULL) {
            rte_ring_free(worker_rings[i]);
            worker_rings[i] = NULL;
        }
    }

    flow_table_destroy();
    spi_rule_engine_destroy();
}
