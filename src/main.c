#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <string.h>



#define RX_DESC_PER_QUEUE 1024
#define TX_DESC_PER_QUEUE 1024

#define MAX_PKTS_BURST 32
#define NUM_MBUFS ((64*1024)-1)
#define MBUF_CACHE_SIZE 128
#define BURST_SIZE 64
#define SCHED_RX_RING_SZ 8192
#define SCHED_TX_RING_SZ 65536
#define BURST_SIZE_TX 32

#define RING_SIZE 16384
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_RESET   "\x1b[0m"
unsigned int portmask;
struct output_buffer {
    unsigned count;
    struct rte_mbuf *mbufs[BURST_SIZE];
};

struct worker_thread_args {
    struct rte_ring *ring_in;
};

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_DESC_PER_QUEUE;
    uint16_t nb_txd = TX_DESC_PER_QUEUE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    memset(&port_conf, 0, sizeof(struct rte_eth_conf));

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u) info: %s\n",
               port, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd, 
                                        rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;

    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd, 
                                        rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);

    if (retval != 0) {
        return retval;
    }

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port, RTE_ETHER_ADDR_BYTES(&addr));
    
    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}
static inline void 
pktmbuf_free_bulk(struct rte_mbuf *mbuf_table[], unsigned n)
{
    unsigned int i;
    for (i = 0; i < n; i++)
        rte_pktmbuf_free(mbuf_table[i]);
}

static __rte_always_inline int
dispatcher_thread(struct rte_ring *ring_out)
{
    uint16_t ret = 0;
    uint16_t nb_rx_pkts;
    uint16_t port_id;
    struct rte_mbuf *pkts[MAX_PKTS_BURST];
    for (;;) {
        RTE_ETH_FOREACH_DEV(port_id) {
            nb_rx_pkts = rte_eth_rx_burst(port_id, 0, pkts, MAX_PKTS_BURST);
            if (likely(nb_rx_pkts > 0)) {
                printf("Dispatcher received %u packets\n", nb_rx_pkts);
                ret = rte_ring_enqueue_burst(ring_out, (void *)pkts, nb_rx_pkts, NULL);
                printf("Dispatcher enqueue %u packets to worker's ring\n", ret);
                if (unlikely(ret < nb_rx_pkts)) {
                    pktmbuf_free_bulk(&pkts[ret], nb_rx_pkts -ret);
                }
            }
        }
    }
    return 0;
}

static int 
worker_thread(void *args_ptr)
{
    uint16_t burst_size = 0;
    uint16_t nb_tx;
    uint16_t tx_port = 1;
    struct worker_thread_args *args;
    struct rte_mbuf *burst_buffer[MAX_PKTS_BURST] = { NULL };
    struct rte_ring *ring_in;

    printf("Worker thread started on lcore %u\n", rte_lcore_id());
    args = (struct worker_thread_args *) args_ptr;
    ring_in = args->ring_in;

    for (;;) {
        burst_size = rte_ring_dequeue_burst(ring_in, (void *)burst_buffer, 
                                            MAX_PKTS_BURST, NULL);
        if (unlikely(burst_size == 0))
            continue;
        printf("Worker dequeued %u packets from ring\n", burst_size);
        nb_tx = rte_eth_tx_burst(tx_port, 0, burst_buffer, burst_size);
        printf("Worker sent out %u packets\n", nb_tx);
        if (unlikely(nb_tx < burst_size)) {
            pktmbuf_free_bulk(&burst_buffer[nb_tx], burst_size - nb_tx);
        }
    }
    return 0;
}
int
main(int argc, char *argv[])
{
    struct rte_mempool *mbuf_pool;
    int ret;
    uint16_t nb_ports;
    uint16_t portid;
    unsigned int worker_lcore_id, main_lcore_id;
    struct worker_thread_args worker_args = { NULL };
    struct rte_ring *dispatcher_to_workers;
    ret = rte_eal_init(argc, argv);
    if (ret < 0) 
        rte_exit(EXIT_FAILURE, "Inivalid EAL arguments\n");

    argc -= ret;
    argv += ret;

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        rte_exit(EXIT_FAILURE, "No ethernet ports -bye\n");
    }
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    RTE_ETH_FOREACH_DEV(portid) {
        if (port_init(portid, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", portid);
    }

    dispatcher_to_workers = rte_ring_create("dispatcher_to_worker", RING_SIZE,
                                            rte_socket_id(), RING_F_SC_DEQ);

    main_lcore_id = rte_get_main_lcore();
    worker_args.ring_in = dispatcher_to_workers; 
    
    worker_lcore_id = rte_get_next_lcore(main_lcore_id, 1, 0); 
    if (worker_lcore_id == RTE_MAX_LCORE) {
        rte_exit(EXIT_FAILURE, "Not enough core available to run worker!\n");
    }

    rte_eal_remote_launch(worker_thread, &worker_args, worker_lcore_id);
    dispatcher_thread(dispatcher_to_workers);
    rte_eal_mp_wait_lcore();

    rte_eal_cleanup();
    return 0;
}
