#include <generic/rte_prefetch.h>
#include <netinet/in.h>
#include <rte_build_config.h>
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
#include <rte_random.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <rte_hash_crc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#define NUM_MBUFS 32767 
#define NUM_WORKERS 4
#define MBUF_CACHE_SIZE 250
#define RX_DESC_PER_QUEUE 1024
#define TX_DESC_PER_QUEUE 1024
#define RING_SIZE 4096
#define HASH_ENTRIES (1024*1024)
#define BURST_SIZE 32

#define PORT_IN 0 
#define PORT_OUT 1

#define PREFETCH_OFFSET 4
static struct rte_mempool *mbuf_pool;
static struct rte_hash *flow_table;
static struct rte_ring *worker_rings[NUM_WORKERS];
static unsigned int worker_lcore_ids[NUM_WORKERS];
volatile uint8_t force_quit;



struct lcore_stats {
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t flows_created;
    uint64_t flowt_deleted;
} __rte_cache_aligned;

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
    uint8_t protocol;

    uint8_t worker_id;
    uint64_t create_time;
    uint64_t last_seen;
} flow_entry_t;

struct worker_args {
    uint32_t worker_id;
};

static flow_entry_t flow_entries[HASH_ENTRIES];
static struct lcore_stats port_stats[RTE_MAX_LCORE];
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
dispatcher_thread_bulk(__rte_unused void *arg)
{
    struct rte_mbuf *pkts_burst[BURST_SIZE];
    struct rte_mbuf *worker_buffers[NUM_WORKERS][BURST_SIZE];
    uint16_t worker_counts[NUM_WORKERS];
    int i, w;

    /* Các mảng chuyên dụng phục vụ Bulk Lookup */
    struct ipv4_5tuple_key keys[BURST_SIZE];
    const void *key_ptrs[BURST_SIZE];
    int32_t positions[BURST_SIZE];

    unsigned int lcore_id = rte_lcore_id();
    printf("Dispatcher is running on lcore %u\n", lcore_id);

    while (!force_quit) {
        memset(worker_counts, 0, sizeof(worker_counts));

        const uint16_t nb_rx = rte_eth_rx_burst(PORT_IN, 0, pkts_burst, BURST_SIZE);
        if (unlikely(nb_rx == 0))
            continue;

        port_stats[lcore_id].rx_pkts += nb_rx;
        uint64_t current_tsc = rte_rdtsc();

        /* ====================================================
         * BƯỚC 1: TIỀN XỬ LÝ (PRE-PROCESSING) & LẤY KHÓA
         * Lọc các gói tin lỗi và trích xuất Five-Tuple vào mảng liên tục
         * ==================================================== */
        uint16_t valid_pkts = 0;
        struct rte_mbuf *valid_mbufs[BURST_SIZE];

        for (i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = pkts_burst[i];
            port_stats[lcore_id].rx_bytes += m->pkt_len;

            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

            if (unlikely(eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))) {
                rte_pktmbuf_free(m);
                continue;
            }

            struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

            if (unlikely(ipv4_hdr->next_proto_id != IPPROTO_TCP && 
                         ipv4_hdr->next_proto_id != IPPROTO_UDP)) {
                rte_pktmbuf_free(m);
                continue;
            }

            /* Gói tin hợp lệ, lưu lại mbuf để xử lý sau */
            valid_mbufs[valid_pkts] = m;

            /* Trích xuất thông tin định danh vào struct liền kề trong mảng */
            keys[valid_pkts].ip_src = ipv4_hdr->src_addr;
            keys[valid_pkts].ip_dst = ipv4_hdr->dst_addr;
            keys[valid_pkts].proto  = ipv4_hdr->next_proto_id;

            uint16_t *ports = (uint16_t *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));
            keys[valid_pkts].port_src = ports[0];
            keys[valid_pkts].port_dst = ports[1];

            /* Gán địa chỉ của key vào mảng con trỏ cho hàm Bulk */
            key_ptrs[valid_pkts] = &keys[valid_pkts];
            valid_pkts++;
        }

        if (unlikely(valid_pkts == 0)) 
            continue;

        /* ====================================================
         * BƯỚC 2: TRA CỨU HÀNG LOẠT (BULK LOOKUP)
         * Hàm này giải quyết toàn bộ bài toán cache miss bằng phần cứng.
         * Kết quả trả về mảng positions: >= 0 (Index), < 0 (Không tồn tại)
         * ==================================================== */
        rte_hash_lookup_bulk(flow_table, key_ptrs, valid_pkts, positions);

        /* ====================================================
         * BƯỚC 3: XỬ LÝ TRẠNG THÁI & PHÂN TẢI
         * Dựa vào mảng positions để quyết định Update hay Create Flow
         * ==================================================== */
        for (i = 0; i < valid_pkts; i++) {
            struct rte_mbuf *m = valid_mbufs[i];
            int flow_index = positions[i];
            uint32_t target_worker_id;

            if (unlikely(flow_index < 0)) {
                /* Flow Mới -> Thực hiện add key vào bảng */
                flow_index = rte_hash_add_key(flow_table, key_ptrs[i]);

                if (likely(flow_index >= 0)) {
                    flow_entries[flow_index].src_ip = keys[i].ip_src;
                    flow_entries[flow_index].dst_ip = keys[i].ip_dst;
                    flow_entries[flow_index].src_port = keys[i].port_src;
                    flow_entries[flow_index].dst_port = keys[i].port_dst;
                    flow_entries[flow_index].protocol = keys[i].proto;

                    /* Băm IP Nguồn để chọn Worker (Logic của dự án) */
                    hash_sig_t hash_val = rte_hash_hash(flow_table, key_ptrs[i]);
                    target_worker_id = hash_val % NUM_WORKERS;

                    flow_entries[flow_index].worker_id = target_worker_id;
                    flow_entries[flow_index].create_time = current_tsc;
                    flow_entries[flow_index].last_seen = current_tsc;

                    port_stats[lcore_id].flows_created++;
                } else {
                    /* Lỗi đầy bảng băm */
                    rte_pktmbuf_free(m);
                    continue; 
                }
            } else {
                /* Flow Đã Tồn Tại -> Update Last Seen */
                target_worker_id = flow_entries[flow_index].worker_id;
                flow_entries[flow_index].last_seen = current_tsc;
            }

            /* Đưa gói tin vào Buffer đợi của Worker phụ trách */
            worker_buffers[target_worker_id][worker_counts[target_worker_id]++] = m;
        }

        /* ====================================================
         * BƯỚC 4: ENQUEUE VÀO RINGS
         * Đẩy hàng loạt gói tin từ mảng tạm vào Lockless Ring
         * ==================================================== */
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
    
    unsigned int lcore_id = rte_lcore_id();
    printf("Worker %u is running on lcore %u\n", worker_id, rte_lcore_id());

    while (!force_quit) {
        nb_rx = rte_ring_dequeue_burst(worker_rings[worker_id], (void **)pkts,
                                       BURST_SIZE, NULL);
        if (unlikely(nb_rx == 0)) 
            continue;
        nb_tx = rte_eth_tx_burst(PORT_OUT, worker_id, pkts, nb_rx);
        
        port_stats[lcore_id].tx_pkts += nb_tx;
        for (i = 0; i < nb_tx; i++) {
            port_stats[lcore_id].tx_bytes += pkts[i]->pkt_len;
        }
        if (unlikely(nb_tx < nb_rx)) {
            for (i = nb_tx; i < nb_rx; i++) {
                rte_pktmbuf_free(pkts[i]);
            }
        }

    }
    return 0;
}
static int
stats_thread(__rte_unused void *arg)
{
    uint64_t prev_rx_pkts = 0, prev_tx_pkts = 0;
    uint64_t prev_rx_bytes = 0, prev_tx_bytes = 0;
    uint64_t prev_flows_created = 0;

    uint64_t prev_worker_tx_pkts[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_tx_bytes[NUM_WORKERS] = { 0 };

    printf("Statistics Collector is running on lcore %u\n", rte_lcore_id());

    printf("\033[2J");
    while (!force_quit) {
        rte_delay_us_sleep(1000000); // Ngủ 1 giây

        uint64_t total_rx_pkts = 0, total_tx_pkts = 0;
        uint64_t total_rx_bytes = 0, total_tx_bytes = 0;
        uint64_t total_flows_created = 0;

        // Gom dữ liệu từ tất cả các Lcore một cách an toàn
        unsigned int lcore_id;
        RTE_LCORE_FOREACH(lcore_id) {
            total_rx_pkts += port_stats[lcore_id].rx_pkts;
            total_tx_pkts += port_stats[lcore_id].tx_pkts;
            total_rx_bytes += port_stats[lcore_id].rx_bytes;
            total_tx_bytes += port_stats[lcore_id].tx_bytes;
            total_flows_created += port_stats[lcore_id].flows_created;
        }

        // Tính toán thông số thời gian thực (Delta)
        uint64_t pps_rx = total_rx_pkts - prev_rx_pkts;
        uint64_t pps_tx = total_tx_pkts - prev_tx_pkts;
        
        // Công thức tính Mbps: (Bytes * 8 bit) / 1,000,000
        uint64_t mbps_rx = ((total_rx_bytes - prev_rx_bytes) * 8) / 1000000;
        uint64_t mbps_tx = ((total_tx_bytes - prev_tx_bytes) * 8) / 1000000;
        
        uint64_t cps = total_flows_created - prev_flows_created; // Connections Per Second
        
        // DPDK Hash table hỗ trợ lấy số lượng entry hiện tại trực tiếp (O(1))
        int32_t active_flows = rte_hash_count(flow_table); 

        // Xóa màn hình và in (dùng \e[1;1H\e[2J để clear console trên Linux)
        printf("\033[1;1H\033[J");
        printf("================ PERFORMANCE STATS ================\n");
        printf("RX Throughput : %10"PRIu64" PPS | %10"PRIu64" Mbps\n", pps_rx, mbps_rx);
        printf("TX Throughput : %10"PRIu64" PPS | %10"PRIu64" Mbps\n", pps_tx, mbps_tx);
        printf("Active Flows  : %10d Flows\n", active_flows);
        printf("Flow Rate     : %10"PRIu64" Created/sec\n", cps);
        printf("===================================================\n\n");

        printf("\n==================== WORKERS DETAILS =======================\n");
        printf("%-10s %-10s %-15s %-15s\n", "WorkerID", "LcoreID", "TX (PPS)", "TX (Mbps)");
        printf("------------------------------------------------------------\n");
        for (int w = 0; w < NUM_WORKERS; w++) {
            unsigned int w_lcore = worker_lcore_ids[w];
            
            // Lấy dữ liệu thô hiện tại từ stats của core tương ứng
            uint64_t w_total_tx_pkts = port_stats[w_lcore].tx_pkts;
            uint64_t w_total_tx_bytes = port_stats[w_lcore].tx_bytes;

            // Tính toán delta (tốc độ thực tế mỗi giây)
            uint64_t w_pps_tx = w_total_tx_pkts - prev_worker_tx_pkts[w];
            uint64_t w_mbps_tx = ((w_total_tx_bytes - prev_worker_tx_bytes[w]) * 8) / 1000000;

            // In dòng dữ liệu của Worker
            printf("Worker %-3d lcore %-3u %15"PRIu64" %15"PRIu64"\n", w, w_lcore, w_pps_tx, w_mbps_tx);

            // Lưu lại giá trị vòng này cho worker
            prev_worker_tx_pkts[w] = w_total_tx_pkts;
            prev_worker_tx_bytes[w] = w_total_tx_bytes;
        }
        printf("============================================================\n\n");

        fflush(stdout);
        // Lưu lại giá trị cho chu kỳ tính toán tiếp theo
        prev_rx_pkts = total_rx_pkts;
        prev_tx_pkts = total_tx_pkts;
        prev_rx_bytes = total_rx_bytes;
        prev_tx_bytes = total_tx_bytes;
        prev_flows_created = total_flows_created;
    }
    return 0;
}

static void
signal_handler(int sig_num)
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

    signal(SIGINT, signal_handler);
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
    
    unsigned int stats_lcore_id = rte_get_next_lcore(-1, 1, 0);
    rte_eal_remote_launch(stats_thread, NULL, stats_lcore_id);
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (lcore_id == stats_lcore_id) continue;
        if (w_id >= NUM_WORKERS) break;
        wargs[w_id].worker_id = w_id;
        worker_lcore_ids[w_id] = lcore_id;
        rte_eal_remote_launch(worker_thread, &wargs[w_id], lcore_id);
        w_id++;
    }

    dispatcher_thread_bulk(NULL);

    for (i = 0; i < NUM_WORKERS; i++) {
        rte_ring_free(worker_rings[i]);
    }

    rte_hash_free(flow_table);

    return 0;
}
