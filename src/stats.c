#include "flow_table.h"
#include "spi_engine.h"
#include "stats.h"
#include "common.h"

#include <rte_hash.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

enum stats_tui_view {
    STATS_TUI_STATISTICS = 0,
    STATS_TUI_WORKERS,
};

struct stats_tui_state {
    enum stats_tui_view view;
    struct termios original_termios;
    char message[128];
    char input[128];
    size_t input_len;
    int stdin_closed;
    int raw_mode_enabled;
};

static char *
trim_command(char *line)
{
    char *cmd = line;
    char *end;

    line[strcspn(line, "\r\n")] = '\0';
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;

    end = cmd + strlen(cmd);
    while (end > cmd && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
        *end = '\0';
    }

    return cmd;
}

static void
set_tui_message(struct stats_tui_state *tui, const char *text)
{
    snprintf(tui->message, sizeof(tui->message), "%s", text);
}

static void
print_tui_prompt(const struct stats_tui_state *tui)
{
    printf("\r\033[Kflowcore> %s", tui->input);
    fflush(stdout);
}

static void
stats_tui_init(struct stats_tui_state *tui)
{
    struct termios raw_termios;

    memset(tui, 0, sizeof(*tui));
    tui->view = STATS_TUI_STATISTICS;
    set_tui_message(tui,
            "commands: show statistics | show worker | reload rule");

    if (!isatty(STDIN_FILENO))
        return;

    if (tcgetattr(STDIN_FILENO, &tui->original_termios) != 0)
        return;

    raw_termios = tui->original_termios;
    raw_termios.c_lflag &= ~(ICANON | ECHO);
    raw_termios.c_cc[VMIN] = 0;
    raw_termios.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios) == 0)
        tui->raw_mode_enabled = 1;
}

static void
stats_tui_destroy(struct stats_tui_state *tui)
{
    if (tui->raw_mode_enabled)
        tcsetattr(STDIN_FILENO, TCSANOW, &tui->original_termios);
}

static void
execute_tui_command(struct stats_tui_state *tui)
{
    char line[sizeof(tui->input)];
    char *cmd;

    snprintf(line, sizeof(line), "%s", tui->input);
    tui->input_len = 0;
    tui->input[0] = '\0';

    cmd = trim_command(line);
    if (*cmd == '\0')
        return;

    if (strcasecmp(cmd, "show statistics") == 0 ||
        strcasecmp(cmd, "stats") == 0) {
        tui->view = STATS_TUI_STATISTICS;
        set_tui_message(tui, "showing statistics");
        return;
    }

    if (strcasecmp(cmd, "show worker") == 0 ||
        strcasecmp(cmd, "show workers") == 0 ||
        strcasecmp(cmd, "workers") == 0) {
        tui->view = STATS_TUI_WORKERS;
        set_tui_message(tui, "showing worker details");
        return;
    }

    if (strcasecmp(cmd, "reload rule") == 0 ||
        strcasecmp(cmd, "reload rules") == 0 ||
        strcasecmp(cmd, "reload") == 0) {
        spi_rule_engine_request_reload();
        set_tui_message(tui, "rule reload requested");
        return;
    }

    if (strcasecmp(cmd, "help") == 0) {
        set_tui_message(tui,
                "commands: show statistics | show worker | reload rule | quit");
        return;
    }

    if (strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0) {
        force_quit = 1;
        set_tui_message(tui, "quit requested");
        return;
    }

    set_tui_message(tui,
            "unknown command; try: show statistics | show worker | reload rule");
}

static void
poll_tui_command(struct stats_tui_state *tui)
{
    struct timeval timeout = { 0, 0 };
    fd_set readfds;
    int ready;

    if (tui->stdin_closed)
        return;

    for (;;) {
        char ch;
        ssize_t nread;

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &readfds))
            return;

        nread = read(STDIN_FILENO, &ch, sizeof(ch));
        if (nread == 0) {
            tui->stdin_closed = 1;
            return;
        }
        if (nread < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            tui->stdin_closed = 1;
            return;
        }

        if (ch == '\n' || ch == '\r') {
            execute_tui_command(tui);
            print_tui_prompt(tui);
            continue;
        }

        if (ch == 4) {
            force_quit = 1;
            set_tui_message(tui, "quit requested");
            return;
        }

        if (ch == 21) {
            tui->input_len = 0;
            tui->input[0] = '\0';
            print_tui_prompt(tui);
            continue;
        }

        if (ch == 8 || ch == 127) {
            if (tui->input_len > 0)
                tui->input[--tui->input_len] = '\0';
            print_tui_prompt(tui);
            continue;
        }

        if (ch == 27)
            continue;

        if (ch < 32 || ch >= 127)
            continue;

        if (tui->input_len + 1 >= sizeof(tui->input)) {
            set_tui_message(tui, "command too long");
            continue;
        }

        tui->input[tui->input_len++] = ch;
        tui->input[tui->input_len] = '\0';
        print_tui_prompt(tui);
    }
}

int
stats_thread(__rte_unused void *arg)
{
    struct lcore_stats prev_totals = { 0 };
    uint64_t prev_worker_tx_pkts[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_tx_bytes[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_rx_pkts[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_spi_drops[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_tx_drops[NUM_WORKERS] = { 0 };
    uint64_t prev_worker_spi_checks[NUM_WORKERS] = { 0 };

    struct flow_table_ctx *ft = flow_table_get_ctx();
    unsigned int lcore_id = rte_lcore_id();
    struct lcore_stats *local_stats = stats_get_current();
    uint64_t prev_tsc = rte_rdtsc();
    uint64_t tsc_hz = rte_get_tsc_hz();
    struct stats_tui_state tui;

    stats_tui_init(&tui);
    flow_table_rcu_register(lcore_id);

    printf("Stats/Aging thread on lcore %u (RCU registered)\n", lcore_id);
    printf("\033[2J");

    uint32_t tick_counter = 0;

    while (!force_quit) {
        flow_table_rcu_offline(lcore_id);
        rte_delay_us_sleep(AGING_INTERVAL_US);
        flow_table_rcu_online(lcore_id);

        if (force_quit)
            break;

        poll_tui_command(&tui);
        if (force_quit)
            break;

        spi_rule_engine_reload_if_needed();

        struct flow_aging_result aged = flow_table_aging_tick();
        local_stats->aging_scanned += aged.scanned;
        local_stats->aging_expired += aged.expired;
        local_stats->aging_deleted += aged.deleted;
        local_stats->flows_deleted += aged.deleted;

        int32_t pressure_active_flows = rte_hash_count(ft->hash);
        if (pressure_active_flows > 0) {
            enum flow_pressure_mode pressure_mode =
                flow_table_pressure_mode((uint32_t)pressure_active_flows);

            if (pressure_mode != FLOW_PRESSURE_NORMAL) {
                struct flow_pressure_result pressure =
                    flow_table_pressure_maintenance(pressure_mode,
                            (uint32_t)pressure_active_flows);

                local_stats->aging_scanned += pressure.scanned;
                local_stats->flows_deleted += pressure.evicted;
                local_stats->victim_evicted_flows += pressure.evicted;
            }
        }

        flow_table_rcu_quiescent(lcore_id);

        uint64_t reclaimed_this_tick = flow_table_reclaim(1024);
        local_stats->aging_reclaimed += reclaimed_this_tick;

        tick_counter++;

        if (tick_counter < AGING_NUM_CHUNKS)
            continue;

        tick_counter = 0;

        struct lcore_stats totals;
        uint64_t now_tsc = rte_rdtsc();
        double elapsed_sec = stats_elapsed_seconds(now_tsc, prev_tsc, tsc_hz);

        stats_collect_totals(&totals);

        uint64_t pps_rx = stats_rate_per_sec(totals.rx_pkts,
                prev_totals.rx_pkts, elapsed_sec);
        uint64_t pps_tx = stats_rate_per_sec(totals.tx_pkts,
                prev_totals.tx_pkts, elapsed_sec);
        uint64_t mbps_rx = stats_mbps(totals.rx_bytes,
                prev_totals.rx_bytes, elapsed_sec);
        uint64_t mbps_tx = stats_mbps(totals.tx_bytes,
                prev_totals.tx_bytes, elapsed_sec);
        uint64_t worker_pps = stats_rate_per_sec(totals.worker_rx_pkts,
                prev_totals.worker_rx_pkts, elapsed_sec);
        uint64_t filtered_ps = stats_rate_per_sec(totals.rx_filtered_pkts,
                prev_totals.rx_filtered_pkts, elapsed_sec);
        uint64_t ring_drop_ps = stats_rate_per_sec(totals.ring_drop_pkts,
                prev_totals.ring_drop_pkts, elapsed_sec);
        uint64_t hash_fail_ps = stats_rate_per_sec(totals.hash_add_failures,
                prev_totals.hash_add_failures, elapsed_sec);
        uint64_t cps = stats_rate_per_sec(totals.flows_created,
                prev_totals.flows_created, elapsed_sec);
        uint64_t dps = stats_rate_per_sec(totals.flows_deleted,
                prev_totals.flows_deleted, elapsed_sec);
        uint64_t timeout_delete_ps = stats_rate_per_sec(totals.aging_deleted,
                prev_totals.aging_deleted, elapsed_sec);
        uint64_t pressure_evict_ps = stats_rate_per_sec(
                totals.victim_evicted_flows,
                prev_totals.victim_evicted_flows, elapsed_sec);
        uint64_t spi_drop_ps = stats_rate_per_sec(totals.spi_pkts_dropped,
                prev_totals.spi_pkts_dropped, elapsed_sec);
        uint64_t tx_drop_ps = stats_rate_per_sec(totals.tx_drop_pkts,
                prev_totals.tx_drop_pkts, elapsed_sec);
        uint64_t aging_scan_ps = stats_rate_per_sec(totals.aging_scanned,
                prev_totals.aging_scanned, elapsed_sec);
        uint64_t aging_expire_ps = stats_rate_per_sec(totals.aging_expired,
                prev_totals.aging_expired, elapsed_sec);
        uint64_t aging_delete_ps = stats_rate_per_sec(totals.aging_deleted,
                prev_totals.aging_deleted, elapsed_sec);
        uint64_t aging_reclaim_ps = stats_rate_per_sec(totals.aging_reclaimed,
                prev_totals.aging_reclaimed, elapsed_sec);
        int32_t active_flows = rte_hash_count(ft->hash);
        enum flow_pressure_mode pressure_mode = flow_table_pressure_mode(
                active_flows > 0 ? (uint32_t)active_flows : 0);
        uint32_t victim_cache_count = flow_table_victim_cache_count();

        printf("\033[1;1H\033[J");
        if (tui.view == STATS_TUI_STATISTICS) {
            printf("================ PERFORMANCE STATS ================\n");
            printf("RX Throughput : %10"PRIu64" PPS | %10"PRIu64" Mbps\n",
                   pps_rx, mbps_rx);
            printf("TX Throughput : %10"PRIu64" PPS | %10"PRIu64" Mbps\n",
                   pps_tx, mbps_tx);
            printf("Worker Input  : %10"PRIu64" PPS | %10"PRIu64" Pkts\n",
                   worker_pps, totals.worker_rx_pkts);
            printf("RX Filtered   : %10"PRIu64" Pkts | %10"PRIu64" Pkts/s\n",
                   totals.rx_filtered_pkts, filtered_ps);
            printf("Active Flows  : %10d Flows\n", active_flows);
            printf("Created Flows : %10"PRIu64" Flows\n", totals.flows_created);
            printf("Deleted Flows : %10"PRIu64" Flows\n", totals.flows_deleted);
            printf("Timeout Delete: %10"PRIu64" Flows\n", totals.aging_deleted);
            printf("Pressure Evict: %10"PRIu64" Flows\n",
                   totals.victim_evicted_flows);
            printf("Flow Rate     : %10"PRIu64" C/s | %10"PRIu64" D/s\n",
                   cps, dps);
            printf("Delete Split  : timeout=%"PRIu64"/s evict=%"PRIu64"/s\n",
                   timeout_delete_ps, pressure_evict_ps);
            printf("SPI Drops     : %10"PRIu64" Pkts | %10"PRIu64" Pkts/s\n",
                   totals.spi_pkts_dropped, spi_drop_ps);
            printf("TX Drops      : %10"PRIu64" Pkts | %10"PRIu64" Pkts/s\n",
                   totals.tx_drop_pkts, tx_drop_ps);
            printf("Ring Drops    : %10"PRIu64" Pkts | %10"PRIu64" Pkts/s\n",
                   totals.ring_drop_pkts, ring_drop_ps);
            printf("Hash Add Fail : %10"PRIu64" Fails | %10"PRIu64" Fails/s\n",
                   totals.hash_add_failures, hash_fail_ps);
            printf("Flow Pressure : %10s | active=%10d | victim_cache=%6"PRIu32"\n",
                   flow_table_pressure_mode_name(pressure_mode),
                   active_flows, victim_cache_count);
            printf("Replacement   : attempts=%"PRIu64" success=%"PRIu64
                   " fail=%"PRIu64" evicted=%"PRIu64"\n",
                   totals.replacement_attempts, totals.replacement_success,
                   totals.replacement_failures, totals.victim_evicted_flows);
            printf("Retry Add     : success=%"PRIu64" fail=%"PRIu64
                   " cache_empty=%"PRIu64"\n",
                   totals.flow_add_retry_success,
                   totals.flow_add_retry_failures, totals.victim_cache_empty);
            printf("Active Rules  : %10"PRIu32" Rules | %10"PRIu32" Version\n",
                   spi_rule_engine_rule_count(), spi_rule_engine_version());
            printf("SPI Forwarded : %10"PRIu64" Pkts | %10"PRIu64" Rule Checks\n",
                   totals.spi_pkts_forwarded, totals.spi_rule_checks);
            printf("Aging         : scan=%"PRIu64"/s timeout_seen=%"PRIu64"/s "
                   "timeout_del=%"PRIu64"/s reclaim=%"PRIu64"/s\n",
                   aging_scan_ps, aging_expire_ps, aging_delete_ps,
                   aging_reclaim_ps);
            printf("Protocols     : HTTP=%"PRIu64" HTTPS=%"PRIu64" DNS=%"PRIu64
                   " TCP=%"PRIu64" UDP=%"PRIu64" OTHER=%"PRIu64"\n",
                   totals.http_pkts, totals.https_pkts, totals.dns_pkts,
                   totals.tcp_pkts, totals.udp_pkts, totals.other_pkts);
            printf("===================================================\n");
        } else {
            printf("================== WORKERS DETAILS ==================\n");

            for (int w = 0; w < NUM_WORKERS; w++) {
                unsigned int wl = worker_lcore_ids[w];
                const struct lcore_stats *worker_stats = stats_get_lcore(wl);
                uint64_t w_rx_pkts = worker_stats->worker_rx_pkts;
                uint64_t w_tx_pkts = worker_stats->tx_pkts;
                uint64_t w_tx_bytes = worker_stats->tx_bytes;
                uint64_t w_spi_drops = worker_stats->spi_pkts_dropped;
                uint64_t w_tx_drops = worker_stats->tx_drop_pkts;
                uint64_t w_spi_checks = worker_stats->spi_rule_checks;
                uint64_t w_in_pps = stats_rate_per_sec(w_rx_pkts,
                        prev_worker_rx_pkts[w], elapsed_sec);
                uint64_t w_pps = stats_rate_per_sec(w_tx_pkts,
                        prev_worker_tx_pkts[w], elapsed_sec);
                uint64_t w_mbps = stats_mbps(w_tx_bytes,
                        prev_worker_tx_bytes[w], elapsed_sec);
                uint64_t w_spi_drop_ps = stats_rate_per_sec(w_spi_drops,
                        prev_worker_spi_drops[w], elapsed_sec);
                uint64_t w_tx_drop_ps = stats_rate_per_sec(w_tx_drops,
                        prev_worker_tx_drops[w], elapsed_sec);
                uint64_t w_spi_check_ps = stats_rate_per_sec(w_spi_checks,
                        prev_worker_spi_checks[w], elapsed_sec);

                printf("Worker %-3d lcore %-3u | IN %10"PRIu64" PPS | TX %10"PRIu64" PPS %10"PRIu64" Mbps "
                       "| SPI_DROP %8"PRIu64"/s | TX_DROP %8"PRIu64"/s | SPI_CHECK %10"PRIu64"/s\n",
                       w, wl, w_in_pps, w_pps, w_mbps, w_spi_drop_ps,
                       w_tx_drop_ps, w_spi_check_ps);
                printf("  HTTP=%"PRIu64" HTTPS=%"PRIu64" DNS=%"PRIu64
                       " TCP=%"PRIu64" UDP=%"PRIu64" OTHER=%"PRIu64"\n",
                       worker_stats->http_pkts,
                       worker_stats->https_pkts,
                       worker_stats->dns_pkts,
                       worker_stats->tcp_pkts,
                       worker_stats->udp_pkts,
                       worker_stats->other_pkts);

                prev_worker_rx_pkts[w] = w_rx_pkts;
                prev_worker_tx_pkts[w]  = w_tx_pkts;
                prev_worker_tx_bytes[w] = w_tx_bytes;
                prev_worker_spi_drops[w] = w_spi_drops;
                prev_worker_tx_drops[w] = w_tx_drops;
                prev_worker_spi_checks[w] = w_spi_checks;
            }
            printf("=====================================================\n");
        }

        if (tui.view != STATS_TUI_WORKERS) {
            for (int w = 0; w < NUM_WORKERS; w++) {
                unsigned int wl = worker_lcore_ids[w];
                const struct lcore_stats *worker_stats = stats_get_lcore(wl);

                prev_worker_rx_pkts[w] = worker_stats->worker_rx_pkts;
                prev_worker_tx_pkts[w] = worker_stats->tx_pkts;
                prev_worker_tx_bytes[w] = worker_stats->tx_bytes;
                prev_worker_spi_drops[w] = worker_stats->spi_pkts_dropped;
                prev_worker_tx_drops[w] = worker_stats->tx_drop_pkts;
                prev_worker_spi_checks[w] = worker_stats->spi_rule_checks;
            }
        }

        printf("\nTUI: %s\n", tui.message);
        printf("Commands: show statistics | show worker | reload rule | help | quit\n");
        print_tui_prompt(&tui);

        prev_totals = totals;
        prev_tsc = now_tsc;
    }
    stats_tui_destroy(&tui);
    flow_table_rcu_unregister(lcore_id);
    return 0;
}
