#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#include "common.h"
#include <rte_hash.h>
#include <rte_rcu_qsbr.h>
#include <rte_hash_crc.h>
#include <rte_cycles.h>

/* ============================================================
 * 5-tuple key cho hash lookup
 * ============================================================
 * 
 * Packed để rte_hash so sánh key byte-by-byte chính xác.
 * Pad 3 bytes để tổng = 16 bytes → CRC32 hash nhanh hơn
 * vì 16 bytes = 2 lần CRC32 8-byte (hardware instruction).
 */
struct ipv4_5tuple_key {
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t port_src;
    uint16_t port_dst;
    uint8_t  proto;
    uint8_t  pad[3];    /* Pad to 16 bytes for fast CRC32 */
} __attribute__((__packed__));

/* ============================================================
 * Hot/Cold Data Splitting
 * ============================================================
 *
 * Tại sao tách hot/cold?
 * 
 * Trên hot path (xử lý mỗi packet), dispatcher chỉ cần:
 *   - Đọc worker_id → để biết gửi packet vào ring nào
 *   - Ghi last_seen → cập nhật timestamp
 * 
 * Nếu worker_id và last_seen nằm cùng struct với src_ip, dst_ip,
 * create_time (cold data, chỉ dùng khi tạo flow/aging), thì mỗi
 * lần update last_seen, CPU phải fetch toàn bộ cache line chứa
 * cả cold data → lãng phí bandwidth memory bus.
 * 
 * Bằng cách tách ra:
 *   - flow_hot_data[position] giữ fields của steady-state path
 *   - flow_cold_data[position] giữ fields chỉ dùng khi create/SPI/aging
 *
 * → Dispatcher chỉ touch hot_data trên steady-state path.
 * → Worker SPI slow-path và aging mới cần cold_data.
 * → Vẫn giữ locality tốt nhưng tránh padding quá mức cho 1M flow entries.
 */

/*
 * Hot data: kept compact to improve table density.
 *
 * The previous "one cache line per flow entry" layout was too expensive:
 * at 1M flows it burns tens of MB just for padding and increases TLB/LLC
 * pressure. Only the arrays themselves need aligned allocation here.
 */
struct flow_hot_data {
    uint64_t last_seen;     /* TSC timestamp - updated every packet */
    uint32_t flow_gen;      /* Detect stale flow_idx metadata in worker */
    uint32_t action_version;/* Cached SPI rule-table version */
    uint8_t  worker_id;     /* Worker core assignment */
    uint8_t  spi_action;    /* Cached SPI decision */
    uint8_t  pad[2];
};

/* Cold data: accessed on flow creation, worker SPI slow path, and aging. */
struct flow_cold_data {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  pad[3];
    uint64_t create_time;   /* TSC at flow creation */
};

/* ============================================================
 * Flow table context - tất cả state liên quan đến flow table
 * ============================================================
 * 
 * Gom lại 1 struct để:
 * 1. Dễ init/cleanup (chỉ cần pass 1 pointer)
 * 2. Tránh global variable rải rác
 * 3. Dễ unit test (inject mock flow table)
 */
struct flow_table_ctx {
    struct rte_hash      *hash;         /* DPDK hash table (lock-free mode) */
    struct rte_rcu_qsbr  *qsv;          /* RCU QSBR variable */
    uint32_t              storage_entries; /* hot/cold array capacity */

    /*
     * Data arrays: indexed by key IDs returned from rte_hash.
     * The valid index range is derived from rte_hash_max_key_id(),
     * so callers must treat storage_entries as the authoritative
     * capacity instead of assuming it is exactly HASH_ENTRIES.
     */
    struct flow_hot_data  *hot;          /* Hot data array */
    struct flow_cold_data *cold;         /* Cold data array */

    /*
     * Aging state: chunked iteration
     * 
     * current_chunk: chunk đang scan (0..AGING_NUM_CHUNKS-1)
     * Mỗi lần aging_tick() được gọi, nó scan chunk hiện tại
     * rồi tăng current_chunk lên 1 (wrap around).
     */
    uint32_t              current_chunk; /* Current aging chunk index */
};

/* ============================================================
 * API Functions
 * ============================================================ */

/**
 * Khởi tạo flow table context.
 * 
 * Tạo rte_hash ở lock-free mode (RW_CONCURRENCY_LF),
 * cấp phát hot/cold arrays trên NUMA-local memory,
 * và setup RCU QSBR.
 * 
 * @param socket_id  NUMA socket để allocate memory
 * @return           0 nếu thành công, < 0 nếu lỗi
 */
int flow_table_init(int socket_id);

/**
 * Giải phóng tài nguyên flow table.
 */
void flow_table_destroy(void);

/**
 * Đăng ký 1 thread là RCU reader.
 * 
 * Gọi hàm này TRƯỚC khi thread bắt đầu access flow table.
 * Mỗi thread cần 1 thread_id duy nhất (thường dùng lcore_id).
 * 
 * @param thread_id  ID duy nhất cho thread này
 */
void flow_table_rcu_register(unsigned int thread_id);

/**
 * Báo RCU quiescent state.
 * 
 * Gọi sau mỗi burst xử lý xong. Ý nghĩa: "Tôi không còn
 * giữ reference nào đến data cũ, writer có thể reclaim."
 * 
 * QUAN TRỌNG: Phải gọi thường xuyên (mỗi burst), nếu không
 * writer (aging) sẽ không bao giờ reclaim được entries cũ
 * → memory leak.
 * 
 * @param thread_id  ID của thread (cùng ID khi register)
 */
void flow_table_rcu_quiescent(unsigned int thread_id);

/**
 * Thực hiện 1 tick aging: scan 1 chunk, batch delete expired flows.
 * 
 * Hàm này KHÔNG lock toàn bộ table. Nó:
 * 1. Iterate qua hash entries thuộc chunk hiện tại
 * 2. Thu thập expired keys vào buffer (tối đa AGING_BATCH_SIZE)
 * 3. Batch delete tất cả expired keys
 * 4. Chuyển sang chunk tiếp theo
 * 
 * @return  Số flow đã xóa trong tick này
 */
uint32_t flow_table_aging_tick(void);

/**
 * Lấy pointer đến flow table context (để dispatcher/worker access).
 */
struct flow_table_ctx *flow_table_get_ctx(void);

#endif /* FLOW_TABLE_H */
