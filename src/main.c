#include <rte_branch_prediction.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf; // hold the config of port
    const uint16_t rx_rings = 1, tx_rings = 1; // number of rx and tx rings of a port
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;
    int retval;
    uint16_t q;
    
    // check if the port is valid
    if (!rte_eth_dev_is_valid_port(port))
        return -1;
    // refresh the port_conf
    memset(&port_conf, 0, sizeof(struct rte_eth_conf));
    
    // get the device info
    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u) info: %s\n",
               port, strerror(-retval));
        return retval;
    }
    
    // eth dev offload 
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    
    // configure the port
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;
    
    // spcecify the number of ring descriptor (mbuf) in the rx, tx queue for the port
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;
    // allocate and setup n rx queue for the port, for this example, n = 1 
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd, 
                                        rte_eth_dev_socket_id(port), 
                                        NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }
    // [UNKOWN], review later 
    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;

    // allocate tx queue
    // a note for this is that tx queue dont need mempool parameter, but need config
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd, 
                                        rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    // after configure, allocate, then we start the port
    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;
    
    // display some information
    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0) {
        return retval;          
    }
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port, RTE_ETHER_ADDR_BYTES(&addr));

    // enable promiscuous mode (allow all packets to pass)
    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}

// processing function
static __rte_noreturn void
lcore_main(void)
{
    uint16_t port;
    
    // check whether the port is belong to this numa node
    RTE_ETH_FOREACH_DEV(port)
        if (rte_eth_dev_socket_id(port) >= 0 &&
            rte_eth_dev_socket_id(port) != (int)rte_socket_id())
            printf("warning, port %u is on remote numa mode to "
                   "polling thread.\n\tperformance will "
                   "not be optimal.\n", port);
    printf("\nCore %u forwarding packets, [Ctrl+C to quit]\n",
           rte_lcore_id());

    // main loop: receive and send packets
    for (;;) {
        // receive step
        RTE_ETH_FOREACH_DEV(port) {
            // declare an array to hold all the pointers to the burst of mbuf received in mempool
            struct rte_mbuf *bufs[BURST_SIZE];
            const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);

            // branch prediction
            if (unlikely(nb_rx ==0))
                continue;

            const uint16_t nb_tx = rte_eth_tx_burst(port ^ 1, 0, bufs, nb_rx);

            // free unsent packtes
            if (unlikely(nb_tx < nb_rx)) {
                uint16_t buf;
                // e.g bufs have 8 entries, nb_tx = 3 => unsent from packet index 3 til 7
                for (buf = nb_tx; buf < nb_rx; buf++)
                    rte_pktmbuf_free(bufs[buf]);
            }


        }
    }
}
int
main(int argc, char *argv[])
{
    // always declare variable at the very first of a function
    
    struct rte_mempool *mbuf_pool;
    unsigned nb_ports;
    uint16_t portid;

    // 1, init the environment
    int rte = rte_eal_init(argc, argv);
    if (rte < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    // check for availability and validity
    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 2 || (nb_ports & 1))
        rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");
    
    // create the mempool 
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports, 
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    
    // init all the port (inform it to dma into mempool)
    RTE_ETH_FOREACH_DEV(portid)
        if (port_init(portid, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", portid);

    // utility, just check if we are wasting resources 
    if (rte_lcore_count() > 1)
        printf("\nWARNING: Too many lcores enabled. Only 1 use.\n");

    // the processing step: receive and send packet via ports
    lcore_main();

    rte_eal_cleanup();
    return 0;
}
