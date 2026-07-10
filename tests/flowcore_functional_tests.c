#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PROJECT_ROOT
#define PROJECT_ROOT "."
#endif

#ifndef FLOWCORE_BIN
#define FLOWCORE_BIN "./build/flowcore"
#endif

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))
#define MAX_PATH_LEN 512
#define MAX_OUTPUT_SIZE (1024 * 1024)

struct metrics {
    unsigned long long created_flows;
    unsigned long long deleted_flows;
    unsigned long long active_flows;
    unsigned long long spi_drops;
    unsigned long long tx_drops;
    unsigned long long active_rules;
    unsigned long long rule_version;
    unsigned long long spi_forwarded;
    unsigned long long rechecks;
    unsigned long long http;
    unsigned long long https;
    unsigned long long dns;
    unsigned long long tcp;
    unsigned long long udp;
    unsigned long long other;
    int reload_seen;
};

struct run_result {
    int exit_code;
    char *raw_output;
    char *clean_output;
    struct metrics metrics;
};

struct child_process {
    pid_t pid;
    char output_path[MAX_PATH_LEN];
};

struct pcap_writer {
    FILE *fp;
    uint32_t ts_usec;
};

struct packet_desc {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t proto;
};

static void
fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void
join_path(char *dst, size_t dst_len, const char *a, const char *b)
{
    if (snprintf(dst, dst_len, "%s/%s", a, b) >= (int)dst_len)
        fatal("Path too long for %s/%s", a, b);
}

static void
sleep_ms(unsigned int ms)
{
    struct timespec req;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

static uint16_t
checksum16(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2)
        sum += (uint16_t)((data[i] << 8) | data[i + 1]);

    if (len & 1)
        sum += (uint16_t)(data[len - 1] << 8);

    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);

    return (uint16_t)(~sum);
}

static void
write_be16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)(value & 0xff);
}

static void
write_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)((value >> 16) & 0xff);
    dst[2] = (uint8_t)((value >> 8) & 0xff);
    dst[3] = (uint8_t)(value & 0xff);
}

static size_t
build_tcp_packet(uint8_t *buf, uint32_t src_ip, uint32_t dst_ip,
        uint16_t src_port, uint16_t dst_port)
{
    static const uint8_t dst_mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    static const uint8_t src_mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    uint8_t pseudo[12 + 20];
    size_t off = 0;
    uint16_t ip_csum;
    uint16_t tcp_csum;

    memcpy(buf + off, dst_mac, sizeof(dst_mac));
    off += sizeof(dst_mac);
    memcpy(buf + off, src_mac, sizeof(src_mac));
    off += sizeof(src_mac);
    write_be16(buf + off, 0x0800);
    off += 2;

    buf[off + 0] = 0x45;
    buf[off + 1] = 0;
    write_be16(buf + off + 2, 40);
    write_be16(buf + off + 4, 0);
    write_be16(buf + off + 6, 0);
    buf[off + 8] = 64;
    buf[off + 9] = 6;
    write_be16(buf + off + 10, 0);
    write_be32(buf + off + 12, src_ip);
    write_be32(buf + off + 16, dst_ip);
    ip_csum = checksum16(buf + off, 20);
    write_be16(buf + off + 10, ip_csum);
    off += 20;

    write_be16(buf + off + 0, src_port);
    write_be16(buf + off + 2, dst_port);
    write_be32(buf + off + 4, 0);
    write_be32(buf + off + 8, 0);
    write_be16(buf + off + 12, (uint16_t)((5U << 12) | 0x0002U));
    write_be16(buf + off + 14, 8192);
    write_be16(buf + off + 16, 0);
    write_be16(buf + off + 18, 0);

    write_be32(pseudo + 0, src_ip);
    write_be32(pseudo + 4, dst_ip);
    pseudo[8] = 0;
    pseudo[9] = 6;
    write_be16(pseudo + 10, 20);
    memcpy(pseudo + 12, buf + off, 20);
    tcp_csum = checksum16(pseudo, sizeof(pseudo));
    write_be16(buf + off + 16, tcp_csum);
    off += 20;

    return off;
}

static size_t
build_udp_packet(uint8_t *buf, uint32_t src_ip, uint32_t dst_ip,
        uint16_t src_port, uint16_t dst_port)
{
    static const uint8_t dst_mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    static const uint8_t src_mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    uint8_t pseudo[12 + 8];
    size_t off = 0;
    uint16_t ip_csum;
    uint16_t udp_csum;

    memcpy(buf + off, dst_mac, sizeof(dst_mac));
    off += sizeof(dst_mac);
    memcpy(buf + off, src_mac, sizeof(src_mac));
    off += sizeof(src_mac);
    write_be16(buf + off, 0x0800);
    off += 2;

    buf[off + 0] = 0x45;
    buf[off + 1] = 0;
    write_be16(buf + off + 2, 28);
    write_be16(buf + off + 4, 0);
    write_be16(buf + off + 6, 0);
    buf[off + 8] = 64;
    buf[off + 9] = 17;
    write_be16(buf + off + 10, 0);
    write_be32(buf + off + 12, src_ip);
    write_be32(buf + off + 16, dst_ip);
    ip_csum = checksum16(buf + off, 20);
    write_be16(buf + off + 10, ip_csum);
    off += 20;

    write_be16(buf + off + 0, src_port);
    write_be16(buf + off + 2, dst_port);
    write_be16(buf + off + 4, 8);
    write_be16(buf + off + 6, 0);

    write_be32(pseudo + 0, src_ip);
    write_be32(pseudo + 4, dst_ip);
    pseudo[8] = 0;
    pseudo[9] = 17;
    write_be16(pseudo + 10, 8);
    memcpy(pseudo + 12, buf + off, 8);
    udp_csum = checksum16(pseudo, sizeof(pseudo));
    if (udp_csum == 0)
        udp_csum = 0xffff;
    write_be16(buf + off + 6, udp_csum);
    off += 8;

    return off;
}

static size_t
build_icmp_echo_packet(uint8_t *buf, uint32_t src_ip, uint32_t dst_ip)
{
    static const uint8_t dst_mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    static const uint8_t src_mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t payload[] = "flowcore-icmp";
    size_t off = 0;
    uint16_t ip_csum;
    uint16_t icmp_csum;

    memcpy(buf + off, dst_mac, sizeof(dst_mac));
    off += sizeof(dst_mac);
    memcpy(buf + off, src_mac, sizeof(src_mac));
    off += sizeof(src_mac);
    write_be16(buf + off, 0x0800);
    off += 2;

    buf[off + 0] = 0x45;
    buf[off + 1] = 0;
    write_be16(buf + off + 2, (uint16_t)(20 + 8 + sizeof(payload) - 1));
    write_be16(buf + off + 4, 0);
    write_be16(buf + off + 6, 0);
    buf[off + 8] = 64;
    buf[off + 9] = 1;
    write_be16(buf + off + 10, 0);
    write_be32(buf + off + 12, src_ip);
    write_be32(buf + off + 16, dst_ip);
    ip_csum = checksum16(buf + off, 20);
    write_be16(buf + off + 10, ip_csum);
    off += 20;

    buf[off + 0] = 8;
    buf[off + 1] = 0;
    write_be16(buf + off + 2, 0);
    write_be16(buf + off + 4, 1);
    write_be16(buf + off + 6, 1);
    memcpy(buf + off + 8, payload, sizeof(payload) - 1);
    icmp_csum = checksum16(buf + off, 8 + sizeof(payload) - 1);
    write_be16(buf + off + 2, icmp_csum);
    off += 8 + sizeof(payload) - 1;

    return off;
}

static size_t
build_arp_request(uint8_t *buf, uint32_t src_ip, uint32_t target_ip)
{
    static const uint8_t dst_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t src_mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    size_t off = 0;

    memcpy(buf + off, dst_mac, sizeof(dst_mac));
    off += sizeof(dst_mac);
    memcpy(buf + off, src_mac, sizeof(src_mac));
    off += sizeof(src_mac);
    write_be16(buf + off, 0x0806);
    off += 2;

    write_be16(buf + off + 0, 1);
    write_be16(buf + off + 2, 0x0800);
    buf[off + 4] = 6;
    buf[off + 5] = 4;
    write_be16(buf + off + 6, 1);
    memcpy(buf + off + 8, src_mac, sizeof(src_mac));
    write_be32(buf + off + 14, src_ip);
    memset(buf + off + 18, 0, 6);
    write_be32(buf + off + 24, target_ip);
    off += 28;

    return off;
}

static void
pcap_writer_open(struct pcap_writer *writer, const char *path)
{
    uint8_t global_hdr[24] = {
        0xd4, 0xc3, 0xb2, 0xa1,
        0x02, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00
    };

    writer->fp = fopen(path, "wb");
    if (writer->fp == NULL)
        fatal("Cannot open %s for write: %s", path, strerror(errno));

    writer->ts_usec = 0;
    if (fwrite(global_hdr, 1, sizeof(global_hdr), writer->fp) != sizeof(global_hdr))
        fatal("Cannot write PCAP global header to %s", path);
}

static void
pcap_writer_write(struct pcap_writer *writer, const uint8_t *data, uint32_t len)
{
    uint8_t rec_hdr[16];

    memset(rec_hdr, 0, sizeof(rec_hdr));
    rec_hdr[4] = (uint8_t)(writer->ts_usec & 0xff);
    rec_hdr[5] = (uint8_t)((writer->ts_usec >> 8) & 0xff);
    rec_hdr[8] = (uint8_t)(len & 0xff);
    rec_hdr[9] = (uint8_t)((len >> 8) & 0xff);
    rec_hdr[10] = (uint8_t)((len >> 16) & 0xff);
    rec_hdr[11] = (uint8_t)((len >> 24) & 0xff);
    rec_hdr[12] = rec_hdr[8];
    rec_hdr[13] = rec_hdr[9];
    rec_hdr[14] = rec_hdr[10];
    rec_hdr[15] = rec_hdr[11];

    if (fwrite(rec_hdr, 1, sizeof(rec_hdr), writer->fp) != sizeof(rec_hdr))
        fatal("Cannot write PCAP record header");
    if (fwrite(data, 1, len, writer->fp) != len)
        fatal("Cannot write PCAP packet payload");

    writer->ts_usec += 1000;
}

static void
pcap_writer_close(struct pcap_writer *writer)
{
    fclose(writer->fp);
    writer->fp = NULL;
}

static uint32_t
ipv4_addr(const char *text)
{
    struct in_addr addr;

    if (inet_pton(AF_INET, text, &addr) != 1)
        fatal("Invalid IPv4 address: %s", text);

    return ntohl(addr.s_addr);
}

static void
write_packet_descs_to_pcap(const char *path, const struct packet_desc *packets,
        size_t count)
{
    struct pcap_writer writer;
    uint8_t buf[128];

    pcap_writer_open(&writer, path);

    for (size_t i = 0; i < count; i++) {
        size_t len;

        if (packets[i].proto == 6) {
            len = build_tcp_packet(buf, packets[i].src_ip, packets[i].dst_ip,
                    packets[i].src_port, packets[i].dst_port);
        } else if (packets[i].proto == 17) {
            len = build_udp_packet(buf, packets[i].src_ip, packets[i].dst_ip,
                    packets[i].src_port, packets[i].dst_port);
        } else {
            fatal("Unsupported packet proto %u", packets[i].proto);
        }

        pcap_writer_write(&writer, buf, (uint32_t)len);
    }

    pcap_writer_close(&writer);
}

static void
write_same_flow_pcap(const char *path)
{
    struct packet_desc packets[10];

    for (size_t i = 0; i < ARRAY_LEN(packets); i++) {
        packets[i].src_ip = ipv4_addr("10.0.0.1");
        packets[i].dst_ip = ipv4_addr("192.168.0.1");
        packets[i].src_port = 12345;
        packets[i].dst_port = 80;
        packets[i].proto = 6;
    }

    write_packet_descs_to_pcap(path, packets, ARRAY_LEN(packets));
}

static void
write_unique_flow_pcap(const char *path)
{
    struct packet_desc packets[32];
    char src_ip[32];

    for (size_t i = 0; i < ARRAY_LEN(packets); i++) {
        snprintf(src_ip, sizeof(src_ip), "10.0.0.%zu", (i % 250) + 1);
        packets[i].src_ip = ipv4_addr(src_ip);
        packets[i].dst_ip = ipv4_addr("192.168.0.1");
        packets[i].src_port = (uint16_t)(20000 + i);
        packets[i].dst_port = 8080;
        packets[i].proto = 17;
    }

    write_packet_descs_to_pcap(path, packets, ARRAY_LEN(packets));
}

static void
write_mixed_rules_pcap(const char *path)
{
    const struct packet_desc packets[] = {
        { 0, 0, 1111, 80, 6 },
        { 0, 0, 1112, 443, 6 },
        { 0, 0, 1113, 53, 17 },
        { 0, 0, 1114, 22, 6 },
        { 0, 0, 1115, 8080, 6 },
        { 0, 0, 1116, 12345, 17 },
    };
    struct packet_desc filled[ARRAY_LEN(packets)];
    char src_ip[32];

    for (size_t i = 0; i < ARRAY_LEN(packets); i++) {
        snprintf(src_ip, sizeof(src_ip), "10.0.0.%zu", i + 1);
        filled[i] = packets[i];
        filled[i].src_ip = ipv4_addr(src_ip);
        filled[i].dst_ip = ipv4_addr("192.168.0.1");
    }

    write_packet_descs_to_pcap(path, filled, ARRAY_LEN(filled));
}

static void
write_aging_pcap(const char *path)
{
    const struct packet_desc packet = {
        0, 0, 25000, 80, 6
    };
    struct packet_desc filled = packet;

    filled.src_ip = ipv4_addr("10.1.1.1");
    filled.dst_ip = ipv4_addr("192.168.1.1");
    write_packet_descs_to_pcap(path, &filled, 1);
}

static void
write_reload_pcap(const char *path)
{
    struct packet_desc packets[8];

    for (size_t i = 0; i < ARRAY_LEN(packets); i++) {
        packets[i].src_ip = ipv4_addr("172.16.0.1");
        packets[i].dst_ip = ipv4_addr("192.168.10.1");
        packets[i].src_port = (uint16_t)(32000 + i);
        packets[i].dst_port = 8443;
        packets[i].proto = 6;
    }

    write_packet_descs_to_pcap(path, packets, ARRAY_LEN(packets));
}

static void
write_non_tcp_udp_filtered_pcap(const char *path)
{
    struct pcap_writer writer;
    uint8_t buf[128];
    size_t len;

    pcap_writer_open(&writer, path);

    len = build_icmp_echo_packet(buf, ipv4_addr("10.9.0.1"),
            ipv4_addr("192.168.9.1"));
    pcap_writer_write(&writer, buf, (uint32_t)len);

    len = build_arp_request(buf, ipv4_addr("10.9.0.2"),
            ipv4_addr("10.9.0.254"));
    pcap_writer_write(&writer, buf, (uint32_t)len);

    pcap_writer_close(&writer);
}

static void
write_text_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL)
        fatal("Cannot open %s for write: %s", path, strerror(errno));

    if (fputs(content, fp) == EOF)
        fatal("Cannot write %s", path);

    fclose(fp);
}

static void
generate_assets(const char *dir)
{
    char path[MAX_PATH_LEN];

    join_path(path, sizeof(path), dir, "same_flow_10.pcap");
    write_same_flow_pcap(path);

    join_path(path, sizeof(path), dir, "unique_flow_32.pcap");
    write_unique_flow_pcap(path);

    join_path(path, sizeof(path), dir, "mixed_rules_6.pcap");
    write_mixed_rules_pcap(path);

    join_path(path, sizeof(path), dir, "aging_single.pcap");
    write_aging_pcap(path);

    join_path(path, sizeof(path), dir, "reload_8443_loop.pcap");
    write_reload_pcap(path);

    join_path(path, sizeof(path), dir, "non_tcp_udp_filtered.pcap");
    write_non_tcp_udp_filtered_pcap(path);

    join_path(path, sizeof(path), dir, "rules_default.cfg");
    write_text_file(path,
        "# Format: Rule_Name,Protocol,Src_IP,Dst_IP,Src_Port,Dst_Port,Action\n"
        "HTTP_ALLOW,TCP,*,*,*,80,FORWARD\n"
        "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD\n"
        "DNS_ALLOW,UDP,*,*,*,53,FORWARD\n"
        "SSH_BLOCK,TCP,*,*,*,22,DROP\n"
        "DEFAULT,*,*,*,*,*,FORWARD\n");

    join_path(path, sizeof(path), dir, "rules_reload_phase1.cfg");
    write_text_file(path,
        "HTTP_ALLOW,TCP,*,*,*,80,FORWARD\n"
        "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD\n"
        "PORT8443_ALLOW,TCP,*,*,*,8443,FORWARD\n"
        "SSH_BLOCK,TCP,*,*,*,22,DROP\n"
        "DEFAULT,*,*,*,*,*,FORWARD\n");

    join_path(path, sizeof(path), dir, "rules_reload_phase2.cfg");
    write_text_file(path,
        "HTTP_ALLOW,TCP,*,*,*,80,FORWARD\n"
        "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD\n"
        "PORT8443_DROP,TCP,*,*,*,8443,DROP\n"
        "SSH_BLOCK,TCP,*,*,*,22,DROP\n"
        "DEFAULT,*,*,*,*,*,FORWARD\n");
}

static char *
read_text_file(const char *path)
{
    FILE *fp;
    long size;
    char *buf;

    fp = fopen(path, "rb");
    if (fp == NULL)
        fatal("Cannot open %s for read: %s", path, strerror(errno));

    if (fseek(fp, 0, SEEK_END) != 0)
        fatal("Cannot seek %s", path);

    size = ftell(fp);
    if (size < 0)
        fatal("Cannot stat %s", path);

    if (fseek(fp, 0, SEEK_SET) != 0)
        fatal("Cannot rewind %s", path);

    if (size > MAX_OUTPUT_SIZE)
        fatal("Output too large: %s", path);

    buf = calloc((size_t)size + 1, 1);
    if (buf == NULL)
        fatal("Out of memory while reading %s", path);

    if (fread(buf, 1, (size_t)size, fp) != (size_t)size)
        fatal("Cannot read %s", path);

    fclose(fp);
    return buf;
}

static char *
strip_ansi(const char *input)
{
    size_t len = strlen(input);
    char *out = malloc(len + 1);
    size_t i = 0;
    size_t j = 0;

    if (out == NULL)
        fatal("Out of memory while stripping ANSI");

    while (i < len) {
        if ((unsigned char)input[i] == 0x1b &&
            i + 1 < len && input[i + 1] == '[') {
            i += 2;
            while (i < len) {
                char c = input[i++];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    break;
            }
            continue;
        }

        out[j++] = input[i++];
    }

    out[j] = '\0';
    return out;
}

static void
parse_metrics(const char *clean_output, struct metrics *metrics)
{
    char *copy = strdup(clean_output);
    char *line;
    char *saveptr = NULL;

    if (copy == NULL)
        fatal("Out of memory while parsing metrics");

    memset(metrics, 0, sizeof(*metrics));
    metrics->reload_seen = strstr(clean_output, "[SPI] Reloaded") != NULL;

    for (line = strtok_r(copy, "\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        unsigned long long value;

        if (sscanf(line, "Created Flows : %llu Flows", &value) == 1) {
            metrics->created_flows = value;
            continue;
        }
        if (sscanf(line, "Deleted/Timeout: %llu Flows", &value) == 1) {
            metrics->deleted_flows = value;
            continue;
        }
        if (sscanf(line, "Active Flows  : %llu Flows", &value) == 1) {
            metrics->active_flows = value;
            continue;
        }
        if (sscanf(line, "SPI Drops     : %llu Pkts", &value) == 1) {
            metrics->spi_drops = value;
            continue;
        }
        if (sscanf(line, "TX Drops      : %llu Pkts", &value) == 1) {
            metrics->tx_drops = value;
            continue;
        }
        if (sscanf(line, "Active Rules  : %llu Rules | %llu Version",
                   &metrics->active_rules, &metrics->rule_version) == 2)
            continue;
        if (sscanf(line, "SPI Forwarded : %llu Pkts | %llu Rechecks",
                   &metrics->spi_forwarded, &metrics->rechecks) == 2)
            continue;
        if (sscanf(line,
                   "Protocols     : HTTP=%llu HTTPS=%llu DNS=%llu TCP=%llu UDP=%llu OTHER=%llu",
                   &metrics->http, &metrics->https, &metrics->dns,
                   &metrics->tcp, &metrics->udp, &metrics->other) == 6)
            continue;
    }

    free(copy);
}

static struct child_process
launch_flowcore(const char *rx_pcap, const char *rules_path, int infinite_rx)
{
    struct child_process child;
    char rx_vdev[MAX_PATH_LEN];
    int out_fd;
    pid_t pid;
    char *argv[17];
    size_t argi = 0;

    if (snprintf(child.output_path, sizeof(child.output_path),
                 "/tmp/flowcore_test_output_%ld.log", (long)getpid()) >=
            (int)sizeof(child.output_path)) {
        fatal("Output path too long");
    }

    if (snprintf(rx_vdev, sizeof(rx_vdev),
                 "net_pcap0,rx_pcap=%s,infinite_rx=%d",
                 rx_pcap, infinite_rx ? 1 : 0) >= (int)sizeof(rx_vdev)) {
        fatal("RX vdev string too long");
    }

    out_fd = open(child.output_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd < 0)
        fatal("Cannot open output log: %s", strerror(errno));

    argv[argi++] = (char *)FLOWCORE_BIN;
    argv[argi++] = "--no-huge";
    argv[argi++] = "--no-pci";
    argv[argi++] = "--in-memory";
    argv[argi++] = "--no-shconf";
    argv[argi++] = "--no-telemetry";
    argv[argi++] = "-l";
    argv[argi++] = "0-5";
    argv[argi++] = "-n";
    argv[argi++] = "4";
    argv[argi++] = "-m";
    argv[argi++] = "512";
    argv[argi++] = "--vdev";
    argv[argi++] = rx_vdev;
    argv[argi++] = "--vdev";
    argv[argi++] = "net_null0";
    argv[argi] = NULL;

    pid = fork();
    if (pid < 0)
        fatal("fork failed: %s", strerror(errno));

    if (pid == 0) {
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);
        setenv("FLOWCORE_NUM_MBUFS", "32768", 1);
        setenv("FLOWCORE_RULES_PATH", rules_path, 1);

        if (dup2(out_fd, STDOUT_FILENO) < 0 || dup2(out_fd, STDERR_FILENO) < 0)
            _exit(127);
        close(out_fd);
        execv(FLOWCORE_BIN, argv);
        _exit(127);
    }

    close(out_fd);
    child.pid = pid;
    return child;
}

static int
stop_flowcore(struct child_process *child, unsigned int timeout_ms,
        char **output_text)
{
    int status = 0;
    unsigned int waited = 0;

    kill(child->pid, SIGINT);

    while (waited < timeout_ms) {
        pid_t ret = waitpid(child->pid, &status, WNOHANG);

        if (ret == child->pid)
            goto done;
        if (ret < 0)
            fatal("waitpid failed: %s", strerror(errno));

        sleep_ms(100);
        waited += 100;
    }

    kill(child->pid, SIGKILL);
    if (waitpid(child->pid, &status, 0) < 0)
        fatal("waitpid after SIGKILL failed: %s", strerror(errno));

done:
    *output_text = read_text_file(child->output_path);
    unlink(child->output_path);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return -1;
}

static struct run_result
run_flowcore_case(const char *rx_pcap, const char *rules_path,
        unsigned int runtime_ms, int infinite_rx)
{
    struct child_process child;
    struct run_result result;

    child = launch_flowcore(rx_pcap, rules_path, infinite_rx);
    sleep_ms(runtime_ms);
    result.exit_code = stop_flowcore(&child, 5000, &result.raw_output);
    result.clean_output = strip_ansi(result.raw_output);
    parse_metrics(result.clean_output, &result.metrics);

    return result;
}

static void
free_run_result(struct run_result *result)
{
    free(result->raw_output);
    free(result->clean_output);
}

static int
expect_true(bool cond, const char *fmt, ...)
{
    va_list ap;

    if (cond)
        return 1;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 0;
}

static int
test_same_flow_reuse(const char *dir)
{
    char rx_path[MAX_PATH_LEN];
    char rules_path[MAX_PATH_LEN];
    struct run_result result;
    int ok = 1;

    join_path(rx_path, sizeof(rx_path), dir, "same_flow_10.pcap");
    join_path(rules_path, sizeof(rules_path), dir, "rules_default.cfg");
    result = run_flowcore_case(rx_path, rules_path, 2500, 0);

    ok &= expect_true(result.exit_code == 0 || result.exit_code == 130,
            "same_flow_reuse: unexpected exit code %d", result.exit_code);
    ok &= expect_true(result.metrics.created_flows == 1,
            "same_flow_reuse: expected created_flows=1, got %llu",
            result.metrics.created_flows);
    ok &= expect_true(result.metrics.spi_forwarded == 10,
            "same_flow_reuse: expected spi_forwarded=10, got %llu",
            result.metrics.spi_forwarded);
    ok &= expect_true(result.metrics.spi_drops == 0,
            "same_flow_reuse: expected spi_drops=0, got %llu",
            result.metrics.spi_drops);

    free_run_result(&result);
    return ok;
}

static int
test_unique_flow_creation(const char *dir)
{
    char rx_path[MAX_PATH_LEN];
    char rules_path[MAX_PATH_LEN];
    struct run_result result;
    int ok = 1;

    join_path(rx_path, sizeof(rx_path), dir, "unique_flow_32.pcap");
    join_path(rules_path, sizeof(rules_path), dir, "rules_default.cfg");
    result = run_flowcore_case(rx_path, rules_path, 2500, 0);

    ok &= expect_true(result.metrics.created_flows == 32,
            "unique_flow_creation: expected created_flows=32, got %llu",
            result.metrics.created_flows);
    ok &= expect_true(result.metrics.spi_forwarded == 32,
            "unique_flow_creation: expected spi_forwarded=32, got %llu",
            result.metrics.spi_forwarded);

    free_run_result(&result);
    return ok;
}

static int
test_rule_drop_ssh(const char *dir)
{
    char rx_path[MAX_PATH_LEN];
    char rules_path[MAX_PATH_LEN];
    struct run_result result;
    int ok = 1;

    join_path(rx_path, sizeof(rx_path), dir, "mixed_rules_6.pcap");
    join_path(rules_path, sizeof(rules_path), dir, "rules_default.cfg");
    result = run_flowcore_case(rx_path, rules_path, 2500, 0);

    ok &= expect_true(result.metrics.spi_forwarded == 5,
            "rule_drop_ssh: expected spi_forwarded=5, got %llu",
            result.metrics.spi_forwarded);
    ok &= expect_true(result.metrics.spi_drops == 1,
            "rule_drop_ssh: expected spi_drops=1, got %llu",
            result.metrics.spi_drops);

    free_run_result(&result);
    return ok;
}

static int
test_protocol_accounting(const char *dir)
{
    char rx_path[MAX_PATH_LEN];
    char rules_path[MAX_PATH_LEN];
    struct run_result result;
    int ok = 1;

    join_path(rx_path, sizeof(rx_path), dir, "mixed_rules_6.pcap");
    join_path(rules_path, sizeof(rules_path), dir, "rules_default.cfg");
    result = run_flowcore_case(rx_path, rules_path, 2500, 0);

    ok &= expect_true(result.metrics.http == 1,
            "protocol_accounting: expected HTTP=1, got %llu",
            result.metrics.http);
    ok &= expect_true(result.metrics.https == 1,
            "protocol_accounting: expected HTTPS=1, got %llu",
            result.metrics.https);
    ok &= expect_true(result.metrics.dns == 1,
            "protocol_accounting: expected DNS=1, got %llu",
            result.metrics.dns);
    ok &= expect_true(result.metrics.tcp == 2,
            "protocol_accounting: expected TCP=2, got %llu",
            result.metrics.tcp);
    ok &= expect_true(result.metrics.udp == 1,
            "protocol_accounting: expected UDP=1, got %llu",
            result.metrics.udp);
    ok &= expect_true(result.metrics.other == 0,
            "protocol_accounting: expected OTHER=0, got %llu",
            result.metrics.other);

    free_run_result(&result);
    return ok;
}

static int
test_aging_cleanup(const char *dir)
{
    char rx_path[MAX_PATH_LEN];
    char rules_path[MAX_PATH_LEN];
    struct run_result result;
    int ok = 1;

    join_path(rx_path, sizeof(rx_path), dir, "aging_single.pcap");
    join_path(rules_path, sizeof(rules_path), dir, "rules_default.cfg");
    result = run_flowcore_case(rx_path, rules_path, 3500, 0);

    ok &= expect_true(result.metrics.created_flows == 1,
            "aging_cleanup: expected created_flows=1, got %llu",
            result.metrics.created_flows);
    ok &= expect_true(result.metrics.deleted_flows >= 1,
            "aging_cleanup: expected deleted_flows>=1, got %llu",
            result.metrics.deleted_flows);

    free_run_result(&result);
    return ok;
}

static int
test_non_tcp_udp_filtered(const char *dir)
{
    char rx_path[MAX_PATH_LEN];
    char rules_path[MAX_PATH_LEN];
    struct run_result result;
    int ok = 1;

    join_path(rx_path, sizeof(rx_path), dir, "non_tcp_udp_filtered.pcap");
    join_path(rules_path, sizeof(rules_path), dir, "rules_default.cfg");
    result = run_flowcore_case(rx_path, rules_path, 2500, 0);

    ok &= expect_true(result.metrics.created_flows == 0,
            "non_tcp_udp_filtered: expected created_flows=0, got %llu",
            result.metrics.created_flows);
    ok &= expect_true(result.metrics.spi_forwarded == 0,
            "non_tcp_udp_filtered: expected spi_forwarded=0, got %llu",
            result.metrics.spi_forwarded);
    ok &= expect_true(result.metrics.spi_drops == 0,
            "non_tcp_udp_filtered: expected spi_drops=0, got %llu",
            result.metrics.spi_drops);
    ok &= expect_true(result.metrics.http == 0 && result.metrics.https == 0 &&
            result.metrics.dns == 0 && result.metrics.tcp == 0 &&
            result.metrics.udp == 0 && result.metrics.other == 0,
            "non_tcp_udp_filtered: expected all protocol counters=0");

    free_run_result(&result);
    return ok;
}

static int
test_hot_reload(const char *dir)
{
    char rx_path[MAX_PATH_LEN];
    char active_rules[MAX_PATH_LEN];
    struct child_process child;
    struct run_result result;
    int ok = 1;

    join_path(rx_path, sizeof(rx_path), dir, "reload_8443_loop.pcap");
    join_path(active_rules, sizeof(active_rules), dir, "rules_hot_reload.cfg");

    write_text_file(active_rules,
        "HTTP_ALLOW,TCP,*,*,*,80,FORWARD\n"
        "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD\n"
        "PORT8443_ALLOW,TCP,*,*,*,8443,FORWARD\n"
        "SSH_BLOCK,TCP,*,*,*,22,DROP\n"
        "DEFAULT,*,*,*,*,*,FORWARD\n");

    child = launch_flowcore(rx_path, active_rules, 1);
    sleep_ms(1500);
    result.raw_output = NULL;
    write_text_file(active_rules,
        "HTTP_ALLOW,TCP,*,*,*,80,FORWARD\n"
        "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD\n"
        "PORT8443_DROP,TCP,*,*,*,8443,DROP\n"
        "SSH_BLOCK,TCP,*,*,*,22,DROP\n"
        "DEFAULT,*,*,*,*,*,FORWARD\n");
    kill(child.pid, SIGUSR1);
    sleep_ms(2000);

    result.exit_code = stop_flowcore(&child, 5000, &result.raw_output);
    result.clean_output = strip_ansi(result.raw_output);
    parse_metrics(result.clean_output, &result.metrics);

    ok &= expect_true(result.exit_code == 0 || result.exit_code == 130,
            "hot_reload: unexpected exit code %d", result.exit_code);
    ok &= expect_true(result.metrics.reload_seen,
            "hot_reload: expected reload log message");
    ok &= expect_true(result.metrics.rule_version >= 2,
            "hot_reload: expected rule_version>=2, got %llu",
            result.metrics.rule_version);
    ok &= expect_true(result.metrics.spi_forwarded > 0,
            "hot_reload: expected spi_forwarded>0, got %llu",
            result.metrics.spi_forwarded);
    ok &= expect_true(result.metrics.spi_drops > 0,
            "hot_reload: expected spi_drops>0, got %llu",
            result.metrics.spi_drops);

    free_run_result(&result);
    return ok;
}

struct named_test {
    const char *name;
    int (*fn)(const char *dir);
};

int
main(void)
{
    char temp_dir[] = "/tmp/flowcore_c_tests_XXXXXX";
    const struct named_test tests[] = {
        { "same_flow_reuse", test_same_flow_reuse },
        { "unique_flow_creation", test_unique_flow_creation },
        { "rule_drop_ssh", test_rule_drop_ssh },
        { "protocol_accounting", test_protocol_accounting },
        { "aging_cleanup", test_aging_cleanup },
        { "non_tcp_udp_filtered", test_non_tcp_udp_filtered },
        { "hot_reload", test_hot_reload },
    };
    int failures = 0;

    if (mkdtemp(temp_dir) == NULL)
        fatal("mkdtemp failed: %s", strerror(errno));

    generate_assets(temp_dir);

    printf("Using assets under %s\n", temp_dir);
    printf("Testing binary %s\n\n", FLOWCORE_BIN);

    for (size_t i = 0; i < ARRAY_LEN(tests); i++) {
        int ok = tests[i].fn(temp_dir);

        if (ok) {
            printf("[PASS] %s\n", tests[i].name);
        } else {
            printf("[FAIL] %s\n", tests[i].name);
            failures++;
        }
    }

    if (failures != 0) {
        printf("\nFunctional tests failed: %d/%zu\n",
               failures, ARRAY_LEN(tests));
        return 1;
    }

    printf("\nFunctional tests passed: %zu/%zu\n",
           ARRAY_LEN(tests), ARRAY_LEN(tests));
    return 0;
}
