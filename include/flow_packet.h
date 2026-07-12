#ifndef FLOW_PACKET_H
#define FLOW_PACKET_H

#include "flow_table.h"
#include "stats.h"

#include <stdint.h>

enum flow_packet_parse_status {
    FLOW_PACKET_PARSE_OK = 0,
    FLOW_PACKET_PARSE_UNSUPPORTED = -1,
    FLOW_PACKET_PARSE_TRUNCATED = -2,
};

int flow_packet_extract_key(const void *data, uint32_t data_len,
        struct ipv4_5tuple_key *key);
int flow_packet_extract_cold(const void *data, uint32_t data_len,
        struct flow_cold_data *cold);
void flow_cold_from_key(struct flow_cold_data *cold,
        const struct ipv4_5tuple_key *key, uint64_t create_time);
void flow_stats_account_protocol(struct lcore_stats *stats,
        const struct flow_cold_data *cold);

#endif /* FLOW_PACKET_H */
