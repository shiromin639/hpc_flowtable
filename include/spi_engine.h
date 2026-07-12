#ifndef SPI_ENGINE_H
#define SPI_ENGINE_H

#include "common.h"
#include "flow_table.h"

#include <stdint.h>

#define SPI_RULE_NAME_LEN 32

enum spi_action {
    SPI_ACTION_UNKNOWN = 0,
    SPI_ACTION_FORWARD = 1,
    SPI_ACTION_DROP = 2,
};

struct spi_rule {
    char name[SPI_RULE_NAME_LEN];
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t protocol;
    uint8_t action;
    uint8_t match_src_ip;
    uint8_t match_dst_ip;
    uint8_t match_src_port;
    uint8_t match_dst_port;
    uint8_t match_protocol;
};

int spi_rule_engine_init(const char *path);
void spi_rule_engine_destroy(void);
void spi_rule_engine_request_reload(void);
int spi_rule_engine_reload_if_needed(void);
uint32_t spi_rule_engine_version(void);
uint32_t spi_rule_engine_rule_count(void);
uint8_t spi_rule_engine_match_cold(const struct flow_cold_data *cold);

#endif /* SPI_ENGINE_H */
