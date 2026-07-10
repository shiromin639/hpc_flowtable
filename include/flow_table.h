#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#include <rte_common.h>
#include <rte_hash.h>
#include <rte_rcu_qsbr.h>
#include <rte_hash_crc.h>
#include <rte_cycles.h>

struct ipv4_5tuple_key {
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t port_src;
    uint16_t port_dst;
    uint8_t  proto;
    uint8_t  pad[3];    /* Pad to 16 bytes for fast CRC32 */
} __attribute__((__packed__));

struct flow_hot_data {
    uint64_t last_seen;     /* TSC timestamp - updated every packet */
    uint32_t flow_gen;      /* Detect stale flow_idx metadata in worker */
    uint32_t action_version;/* Cached SPI rule-table version */
    uint8_t  worker_id;     /* Worker core assignment */
    uint8_t  spi_action;    /* Cached SPI decision */
    uint8_t  pad[2];
};

struct flow_cold_data {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  pad[3];
    uint64_t create_time;   /* TSC at flow creation */
};

struct flow_table_ctx {
    struct rte_hash      *hash;         
    struct rte_rcu_qsbr  *qsv;         
    uint32_t              storage_entries; 
    struct flow_hot_data  *hot;          
    struct flow_cold_data *cold;        
    uint32_t              current_chunk; 
};

int flow_table_init(int socket_id);
void flow_table_destroy(void);
void flow_table_rcu_register(unsigned int thread_id);
void flow_table_rcu_quiescent(unsigned int thread_id);
uint32_t flow_table_aging_tick(void);
struct flow_table_ctx *flow_table_get_ctx(void);

#endif /* FLOW_TABLE_H */
