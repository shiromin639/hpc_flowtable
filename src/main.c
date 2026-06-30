#include <netinet/in.h>
#include <rte_jhash.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_hash.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#define NUM_MBUFS 8191
#define NUM_WORKERS 4
#define MBUF_CACHE_SIZE 250
#define RX_DESC_PER_QUEUE 1024
#define TX_DESC_PER_QUEUE 1024
#define RING_SIZE 4096
#define HASH_ENTRIES (1024*1024)
#define BURST_SIZE 32

#define PORT_IN 0 
#define PORT_OUT 1
static struct rte_mempool *mbuf_pool;
static struct rte_hash *flow_table;
static struct rte_ring *worker_rings[NUM_WORKERS];
volatile uint8_t force_quit;


struct ipv4_5tuple_key {
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t port_src;
    uint16_t port_dst;
    uint8_t  proto;
} __attribute__((__packed__));

typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;

    uint8_t worker_id;
    uint64_t create_time;
    uint64_t last_seen;
    uint8_t protocol;
} flow_entry_t;

static flow_entry_t flow_entries[HASH_ENTRIES];
struct worker_args {
    uint32_t worker_id;
};
static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_NONE,
    },
    .txmode = {
        .mq_mode = RTE_ETH_MQ_TX_NONE,
    }
};

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
    
    // set up port in
    ret = rte_eth_dev_configure(PORT_IN, rx_rings, tx_rings, &port_conf); 
    if (ret != 0)
        return ret;
    for (q = 0; q < rx_rings; q++) {
        ret = rte_eth_rx_queue_setup(PORT_IN, q, nb_rxd, 
                                     socket_id_in, NULL, mbuf_pool);
        if (ret < 0) {
            printf("Cannot set up rx queue for port in\n");
            return ret;
        }
    }
    rte_eth_tx_queue_setup(PORT_IN, 0, nb_txd, socket_id_in, NULL);

    // set up port out
    tx_rings = 4;
    ret = rte_eth_dev_configure(PORT_OUT, rx_rings, tx_rings, &port_conf);
    if (ret != 0) {
        printf("Cannot configure port out\n");
        return ret;
    }
    rte_eth_rx_queue_setup(PORT_OUT, 0, nb_rxd, socket_id_out, 
                           NULL, mbuf_pool);
    for (q = 0; q < tx_rings; q++) {
        ret = rte_eth_tx_queue_setup(PORT_OUT, q, nb_txd, socket_id_out, NULL);  
        if (ret != 0) {
            printf("Cannot set up tx queue %d for port out\n", q);
        }
    }

    // start the ports
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
dispatcher_thread(__rte_unused void *arg)
{
    struct rte_mbuf *pkts_burst[BURST_SIZE];
    struct rte_mbuf *worker_buffers[NUM_WORKERS][BURST_SIZE];
    uint16_t worker_counts[NUM_WORKERS];
    int i, w;

    printf("Dispatcher is running on lcore %u\n", rte_lcore_id());

    while (!force_quit) {
        memset(worker_counts, 0, sizeof(worker_counts));
        
        const uint16_t nb_rx = rte_eth_rx_burst(PORT_IN, 0, pkts_burst, BURST_SIZE);
        if (unlikely(nb_rx == 0))
            continue;
        
        /* Lấy thời gian hiện tại một lần cho cả batch (tiết kiệm cycle) */
        uint64_t current_tsc = rte_rdtsc();

        for (i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = pkts_burst[i];
            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

            if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                rte_pktmbuf_free(m);
                continue;
            }

            struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
            
            if (ipv4_hdr->next_proto_id != IPPROTO_TCP && ipv4_hdr->next_proto_id != IPPROTO_UDP) {
                rte_pktmbuf_free(m);
                continue;
            }

            /* Tạo Key */
            struct ipv4_5tuple_key key;
            key.ip_src = ipv4_hdr->src_addr;
            key.ip_dst = ipv4_hdr->dst_addr;
            key.proto  = ipv4_hdr->next_proto_id;

            uint16_t *ports = (uint16_t *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));
            key.port_src = ports[0];
            key.port_dst = ports[1];

            /* ====================================================
             * CORE LOGIC: QUẢN LÝ FLOW TABLE & ROUTING
             * ==================================================== */
            uint32_t target_worker_id;
            
            /* Tìm flow trong bảng băm */
            int flow_index = rte_hash_lookup(flow_table, &key);
            
            if (flow_index < 0) {
                /* KHÔNG TÌM THẤY -> ĐÂY LÀ FLOW MỚI */
                flow_index = rte_hash_add_key(flow_table, &key);
                
                if (likely(flow_index >= 0)) {
                    /* Hash table trả về vị trí index, ta ghi đè vào mảng flow_entries */
                    flow_entries[flow_index].src_ip = key.ip_src;
                    flow_entries[flow_index].dst_ip = key.ip_dst;
                    flow_entries[flow_index].src_port = key.port_src;
                    flow_entries[flow_index].dst_port = key.port_dst;
                    flow_entries[flow_index].protocol = key.proto;
                    
                    /* Phân công Worker (Round-robin hoặc Hash). Ở đây dùng Hash modulo */
                    hash_sig_t hash_val = rte_hash_hash(flow_table, &key);
                    target_worker_id = hash_val % NUM_WORKERS;
                    
                    flow_entries[flow_index].worker_id = target_worker_id;
                    flow_entries[flow_index].create_time = current_tsc;
                    flow_entries[flow_index].last_seen = current_tsc;
                } else {
                    /* LỖI: Bảng băm đã đầy (Hash table full) -> Drop packet */
                    rte_pktmbuf_free(m);
                    continue; 
                }
            } else {
                /* ĐÃ TÌM THẤY -> FLOW ĐANG TỒN TẠI */
                /* flow_index lúc này là index của flow cũ */
                target_worker_id = flow_entries[flow_index].worker_id;
                
                /* Cập nhật timestamp để sau này làm cơ sở xóa flow (timeout) */
                flow_entries[flow_index].last_seen = current_tsc;
            }

            /* ==================================================== */

            /* Đẩy gói tin vào buffer của worker tương ứng */
            worker_buffers[target_worker_id][worker_counts[target_worker_id]++] = m;
        }

        /* Đẩy từ buffer tạm vào Ring (Tương tự như cũ) */
        for (w = 0; w < NUM_WORKERS; w++) {
            if (worker_counts[w] > 0) {
                uint16_t sent = rte_ring_enqueue_burst(worker_rings[w], 
                                                      (void **)worker_buffers[w], 
                                                      worker_counts[w], NULL);
                if (unlikely(sent < worker_counts[w])) {
                    for (i = sent; i < worker_counts[w]; i++) {
                        rte_pktmbuf_free(worker_buffers[w][i]);
                    }
                }
            }
        }
    }
    return 0;
}
static int 
worker_thread(void *arg)
{
    struct worker_args *wargs = (struct worker_args *)arg;
    uint32_t worker_id = wargs->worker_id;
    struct rte_mbuf *pkts[BURST_SIZE];
    uint16_t nb_rx, nb_tx;
    uint16_t i;

    printf("Worker %u is running on lcore %u\n", worker_id, rte_lcore_id());

    while (!force_quit) {
        nb_rx = rte_ring_dequeue_burst(worker_rings[worker_id], (void **)pkts,
                                       BURST_SIZE, NULL);
        if (unlikely(nb_rx == 0)) 
            continue;

        nb_tx = rte_eth_tx_burst(PORT_OUT, worker_id, pkts, nb_rx);

        if (unlikely(nb_tx < nb_rx)) {
            for (i = nb_tx; i < nb_rx; i++) {
                rte_pktmbuf_free(pkts[i]);
            }
        }

    }
    return 0;
}

static void
int_handler(int sig_num)
{
    printf("Exiting on signal %d\n", sig_num);
    force_quit = 1;
}
int 
main(int argc, char *argv[])
{
    int ret, i;
    unsigned nb_ports; 
    unsigned lcore_id; 

    signal(SIGINT, int_handler);
    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot init eal\n");
    }
    
    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 1) {
        rte_exit(EXIT_FAILURE, "Not enough ports\n");
    }
    
    if (rte_lcore_count() < NUM_WORKERS + 1) {
        rte_exit(EXIT_FAILURE, "Need at least %d cores\n", NUM_WORKERS + 1);
    }

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports, 
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, 
                                        rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf pool\n");
    }

    ret = ports_init();
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Cannit init ports\n");
    }

    struct rte_hash_parameters hash_params = {
        .name = "flow_table",
        .entries = HASH_ENTRIES,
        .key_len = sizeof(struct ipv4_5tuple_key),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };
    
    flow_table = rte_hash_create(&hash_params);
    if (flow_table == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create hash_table\n");
    }

    for (i = 0; i < NUM_WORKERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "worker_ring_%d", i);

        worker_rings[i] = rte_ring_create(name, RING_SIZE, rte_socket_id(), 
                                          RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (worker_rings[i] == NULL) {
            rte_exit(EXIT_FAILURE, "Cannot create ring %d\n", i);
        }
    }

    force_quit = 0;
    struct worker_args wargs[NUM_WORKERS];
    uint32_t w_id = 0;
    
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (w_id >= NUM_WORKERS) break;
        wargs[w_id].worker_id = w_id;
        rte_eal_remote_launch(worker_thread, &wargs[w_id], lcore_id);
        w_id++;
    }

    dispatcher_thread(NULL);

    for (i = 0; i < NUM_WORKERS; i++) {
        rte_ring_free(worker_rings[i]);
    }

    rte_hash_free(flow_table);

    return 0;
}
