#include "spi_engine.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct spi_rule_table {
    uint32_t version;
    uint32_t count;
    struct spi_rule rules[SPI_MAX_RULES];
};

static struct spi_rule_table g_rule_tables[2];
static struct spi_rule_table *g_active_rules;
static volatile sig_atomic_t g_reload_requested;
static char g_rules_path[256];

static char *
trim_token(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static int
parse_protocol(const char *token, struct spi_rule *rule)
{
    if (strcmp(token, "*") == 0) {
        rule->match_protocol = 0;
        rule->protocol = 0;
        return 0;
    }

    rule->match_protocol = 1;
    if (strcasecmp(token, "TCP") == 0) {
        rule->protocol = IPPROTO_TCP;
        return 0;
    }
    if (strcasecmp(token, "UDP") == 0) {
        rule->protocol = IPPROTO_UDP;
        return 0;
    }

    return -1;
}

static int
parse_ip(const char *token, uint32_t *ip, uint8_t *match)
{
    if (strcmp(token, "*") == 0) {
        *match = 0;
        *ip = 0;
        return 0;
    }

    *match = 1;
    if (inet_pton(AF_INET, token, ip) != 1)
        return -1;

    return 0;
}

static int
parse_port(const char *token, uint16_t *port, uint8_t *match)
{
    char *end = NULL;
    unsigned long value;

    if (strcmp(token, "*") == 0) {
        *match = 0;
        *port = 0;
        return 0;
    }

    errno = 0;
    value = strtoul(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' || value > UINT16_MAX)
        return -1;

    *match = 1;
    *port = rte_cpu_to_be_16((uint16_t)value);
    return 0;
}

static int
parse_action(const char *token, uint8_t *action)
{
    /*
     * LOG/COUNT are intentionally normalized to FORWARD. Synchronous
     * per-packet logging on the worker hot path would be a regression.
     */
    if (strcasecmp(token, "FORWARD") == 0 ||
        strcasecmp(token, "LOG") == 0 ||
        strcasecmp(token, "COUNT") == 0) {
        *action = SPI_ACTION_FORWARD;
        return 0;
    }

    if (strcasecmp(token, "DROP") == 0) {
        *action = SPI_ACTION_DROP;
        return 0;
    }

    return -1;
}

static int
load_rule_file(const char *path, struct spi_rule_table *table)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    unsigned int lineno = 0;

    if (fp == NULL) {
        printf("[SPI] Cannot open rule file '%s'\n", path);
        return -1;
    }

    memset(table, 0, sizeof(*table));

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *tokens[7];
        char *cursor;
        char *saveptr = NULL;
        unsigned int idx = 0;
        struct spi_rule *rule;

        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        cursor = trim_token(line);

        if (*cursor == '\0' || *cursor == '#')
            continue;

        for (char *tok = strtok_r(cursor, ",", &saveptr);
             tok != NULL && idx < RTE_DIM(tokens);
             tok = strtok_r(NULL, ",", &saveptr)) {
            tokens[idx++] = trim_token(tok);
        }

        if (idx != RTE_DIM(tokens) || strtok_r(NULL, ",", &saveptr) != NULL) {
            printf("[SPI] Invalid rule format at line %u\n", lineno);
            fclose(fp);
            return -1;
        }

        if (table->count >= SPI_MAX_RULES) {
            printf("[SPI] Too many rules (max=%u)\n", SPI_MAX_RULES);
            fclose(fp);
            return -1;
        }

        rule = &table->rules[table->count];
        memset(rule, 0, sizeof(*rule));
        snprintf(rule->name, sizeof(rule->name), "%s", tokens[0]);

        if (parse_protocol(tokens[1], rule) != 0 ||
            parse_ip(tokens[2], &rule->src_ip, &rule->match_src_ip) != 0 ||
            parse_ip(tokens[3], &rule->dst_ip, &rule->match_dst_ip) != 0 ||
            parse_port(tokens[4], &rule->src_port, &rule->match_src_port) != 0 ||
            parse_port(tokens[5], &rule->dst_port, &rule->match_dst_port) != 0 ||
            parse_action(tokens[6], &rule->action) != 0) {
            printf("[SPI] Invalid rule values at line %u\n", lineno);
            fclose(fp);
            return -1;
        }

        table->count++;
    }

    fclose(fp);
    return 0;
}

static inline int
rule_matches(const struct spi_rule *rule, const struct flow_cold_data *cold)
{
    return (!rule->match_protocol || rule->protocol == cold->protocol) &&
           (!rule->match_src_ip || rule->src_ip == cold->src_ip) &&
           (!rule->match_dst_ip || rule->dst_ip == cold->dst_ip) &&
           (!rule->match_src_port || rule->src_port == cold->src_port) &&
           (!rule->match_dst_port || rule->dst_port == cold->dst_port);
}

static inline uint8_t
match_cold_against_table(const struct spi_rule_table *table,
        const struct flow_cold_data *cold)
{
    if (table == NULL)
        return SPI_ACTION_FORWARD;

    for (uint32_t i = 0; i < table->count; i++) {
        if (rule_matches(&table->rules[i], cold))
            return table->rules[i].action;
    }

    return SPI_ACTION_FORWARD;
}

uint8_t
spi_rule_engine_match_cold(const struct flow_cold_data *cold)
{
    const struct spi_rule_table *active =
        __atomic_load_n(&g_active_rules, __ATOMIC_ACQUIRE);

    return match_cold_against_table(active, cold);
}

int
spi_rule_engine_init(const char *path)
{
    memset(g_rule_tables, 0, sizeof(g_rule_tables));
    memset(g_rules_path, 0, sizeof(g_rules_path));
    snprintf(g_rules_path, sizeof(g_rules_path), "%s", path);

    if (load_rule_file(g_rules_path, &g_rule_tables[0]) != 0)
        return -1;

    g_rule_tables[0].version = 1;
    g_active_rules = &g_rule_tables[0];
    g_reload_requested = 0;

    printf("[SPI] Loaded %u rules from %s (version %u)\n",
           g_rule_tables[0].count, g_rules_path, g_rule_tables[0].version);
    return 0;
}

void
spi_rule_engine_destroy(void)
{
    memset(g_rule_tables, 0, sizeof(g_rule_tables));
    g_active_rules = NULL;
    g_reload_requested = 0;
    memset(g_rules_path, 0, sizeof(g_rules_path));
}

void
spi_rule_engine_request_reload(void)
{
    g_reload_requested = 1;
}

int
spi_rule_engine_reload_if_needed(void)
{
    struct spi_rule_table *active;
    struct spi_rule_table *inactive;

    if (!__atomic_exchange_n(&g_reload_requested, 0, __ATOMIC_ACQ_REL))
        return 0;

    active = __atomic_load_n(&g_active_rules, __ATOMIC_ACQUIRE);
    inactive = (active == &g_rule_tables[0]) ?
        &g_rule_tables[1] : &g_rule_tables[0];

    if (load_rule_file(g_rules_path, inactive) != 0) {
        printf("[SPI] Reload failed, keeping version %u\n",
               active != NULL ? active->version : 0);
        return -1;
    }

    inactive->version = (active != NULL ? active->version : 0) + 1;
    __atomic_store_n(&g_active_rules, inactive, __ATOMIC_RELEASE);

    printf("[SPI] Reloaded %u rules from %s (version %u)\n",
           inactive->count, g_rules_path, inactive->version);
    return 1;
}

uint32_t
spi_rule_engine_version(void)
{
    const struct spi_rule_table *active =
        __atomic_load_n(&g_active_rules, __ATOMIC_ACQUIRE);

    return active != NULL ? active->version : 0;
}

uint32_t
spi_rule_engine_rule_count(void)
{
    const struct spi_rule_table *active =
        __atomic_load_n(&g_active_rules, __ATOMIC_ACQUIRE);

    return active != NULL ? active->count : 0;
}

uint8_t
spi_rule_engine_eval(struct flow_table_ctx *ft, uint32_t flow_idx)
{
    const struct spi_rule_table *active =
        __atomic_load_n(&g_active_rules, __ATOMIC_ACQUIRE);
    struct flow_hot_data *hot = &ft->hot[flow_idx];

    if (active == NULL)
        return SPI_ACTION_FORWARD;

    if (likely(hot->action_version == active->version) &&
        likely(hot->spi_action != SPI_ACTION_UNKNOWN))
        return hot->spi_action;

    hot->spi_action = match_cold_against_table(active, &ft->cold[flow_idx]);
    hot->action_version = active->version;
    return hot->spi_action;
}
