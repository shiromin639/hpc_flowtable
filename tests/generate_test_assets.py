#!/usr/bin/env python3
from __future__ import annotations

import socket
import struct
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
GENERATED_DIR = REPO_ROOT / "tests" / "generated"


def checksum(data: bytes) -> int:
    if len(data) % 2 == 1:
        data += b"\x00"

    words = struct.unpack(f"!{len(data) // 2}H", data)
    total = sum(words)
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ethernet_header() -> bytes:
    dst = bytes.fromhex("001122334455")
    src = bytes.fromhex("aabbccddeeff")
    ether_type = struct.pack("!H", 0x0800)
    return dst + src + ether_type


def ipv4_header(src_ip: str, dst_ip: str, proto: int, payload_len: int,
        ident: int = 0) -> bytes:
    version_ihl = 0x45
    tos = 0
    total_length = 20 + payload_len
    flags_fragment = 0
    ttl = 64
    hdr_checksum = 0
    src = socket.inet_aton(src_ip)
    dst = socket.inet_aton(dst_ip)

    header = struct.pack(
        "!BBHHHBBH4s4s",
        version_ihl,
        tos,
        total_length,
        ident & 0xFFFF,
        flags_fragment,
        ttl,
        proto,
        hdr_checksum,
        src,
        dst,
    )
    hdr_checksum = checksum(header)
    return struct.pack(
        "!BBHHHBBH4s4s",
        version_ihl,
        tos,
        total_length,
        ident & 0xFFFF,
        flags_fragment,
        ttl,
        proto,
        hdr_checksum,
        src,
        dst,
    )


def tcp_segment(src_ip: str, dst_ip: str, sport: int, dport: int) -> bytes:
    seq = 0
    ack = 0
    data_offset_flags = (5 << 12) | 0x002
    window = 8192
    urg_ptr = 0
    header = struct.pack(
        "!HHIIHHHH",
        sport,
        dport,
        seq,
        ack,
        data_offset_flags,
        window,
        0,
        urg_ptr,
    )
    pseudo = socket.inet_aton(src_ip) + socket.inet_aton(dst_ip) + \
        struct.pack("!BBH", 0, 6, len(header))
    tcp_checksum = checksum(pseudo + header)
    return struct.pack(
        "!HHIIHHHH",
        sport,
        dport,
        seq,
        ack,
        data_offset_flags,
        window,
        tcp_checksum,
        urg_ptr,
    )


def udp_segment(src_ip: str, dst_ip: str, sport: int, dport: int) -> bytes:
    length = 8
    header = struct.pack("!HHHH", sport, dport, length, 0)
    pseudo = socket.inet_aton(src_ip) + socket.inet_aton(dst_ip) + \
        struct.pack("!BBH", 0, 17, length)
    udp_checksum = checksum(pseudo + header)
    if udp_checksum == 0:
        udp_checksum = 0xFFFF
    return struct.pack("!HHHH", sport, dport, length, udp_checksum)


def tcp_pkt(src_ip: str, dst_ip: str, sport: int, dport: int):
    l4 = tcp_segment(src_ip, dst_ip, sport, dport)
    return ethernet_header() + ipv4_header(src_ip, dst_ip, 6, len(l4)) + l4


def udp_pkt(src_ip: str, dst_ip: str, sport: int, dport: int):
    l4 = udp_segment(src_ip, dst_ip, sport, dport)
    return ethernet_header() + ipv4_header(src_ip, dst_ip, 17, len(l4)) + l4


def write_pcap(path: Path, packets: list[bytes]) -> None:
    with path.open("wb") as fp:
        fp.write(struct.pack("<IHHIIII",
            0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        ts_sec = 0
        ts_usec = 0
        for packet in packets:
            fp.write(struct.pack("<IIII",
                ts_sec, ts_usec, len(packet), len(packet)))
            fp.write(packet)
            ts_usec += 1000


def write_rules(path: Path, lines: list[str]) -> None:
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def generate_pcaps() -> None:
    GENERATED_DIR.mkdir(parents=True, exist_ok=True)

    write_pcap(
        GENERATED_DIR / "same_flow_10.pcap",
        [tcp_pkt("10.0.0.1", "192.168.0.1", 12345, 80) for _ in range(10)],
    )

    write_pcap(
        GENERATED_DIR / "unique_flow_32.pcap",
        [
            udp_pkt(f"10.0.0.{(i % 250) + 1}", "192.168.0.1", 20000 + i, 8080)
            for i in range(32)
        ],
    )

    write_pcap(
        GENERATED_DIR / "mixed_rules_6.pcap",
        [
            tcp_pkt("10.0.0.1", "192.168.0.1", 1111, 80),
            tcp_pkt("10.0.0.2", "192.168.0.1", 1112, 443),
            udp_pkt("10.0.0.3", "192.168.0.1", 1113, 53),
            tcp_pkt("10.0.0.4", "192.168.0.1", 1114, 22),
            tcp_pkt("10.0.0.5", "192.168.0.1", 1115, 8080),
            udp_pkt("10.0.0.6", "192.168.0.1", 1116, 12345),
        ],
    )

    write_pcap(
        GENERATED_DIR / "aging_single.pcap",
        [tcp_pkt("10.1.1.1", "192.168.1.1", 25000, 80)],
    )

    write_pcap(
        GENERATED_DIR / "reload_8443_loop.pcap",
        [tcp_pkt("172.16.0.1", "192.168.10.1", 32000 + i, 8443) for i in range(8)],
    )

    write_pcap(
        GENERATED_DIR / "perf_fixed_256.pcap",
        [
            udp_pkt(f"10.10.{((i % 256) // 64) + 1}.{((i % 256) % 64) + 1}",
                    "192.168.100.1", 10000 + (i % 256), 80)
            for i in range(20000)
        ],
    )

    write_pcap(
        GENERATED_DIR / "perf_unique_20k.pcap",
        [
            udp_pkt(f"10.{(i // 65536) % 256}.{(i // 256) % 256}.{(i % 254) + 1}",
                    "192.168.101.1", 1024 + (i % 60000), 8080 + (i % 32))
            for i in range(20000)
        ],
    )

    mixed_perf = []
    for i in range(20000):
        src = f"10.20.{(i // 256) % 256}.{(i % 254) + 1}"
        if i % 5 == 0:
            mixed_perf.append(tcp_pkt(src, "192.168.102.1", 20000 + i, 22))
        elif i % 5 == 1:
            mixed_perf.append(tcp_pkt(src, "192.168.102.1", 20000 + i, 80))
        elif i % 5 == 2:
            mixed_perf.append(tcp_pkt(src, "192.168.102.1", 20000 + i, 443))
        elif i % 5 == 3:
            mixed_perf.append(udp_pkt(src, "192.168.102.1", 20000 + i, 53))
        else:
            mixed_perf.append(udp_pkt(src, "192.168.102.1", 20000 + i, 12345))
    write_pcap(GENERATED_DIR / "perf_rule_mix_20k.pcap", mixed_perf)


def generate_rules() -> None:
    write_rules(
        GENERATED_DIR / "rules_default.cfg",
        [
            "# Format: Rule_Name,Protocol,Src_IP,Dst_IP,Src_Port,Dst_Port,Action",
            "HTTP_ALLOW,TCP,*,*,*,80,FORWARD",
            "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD",
            "DNS_ALLOW,UDP,*,*,*,53,FORWARD",
            "SSH_BLOCK,TCP,*,*,*,22,DROP",
            "DEFAULT,*,*,*,*,*,FORWARD",
        ],
    )

    write_rules(
        GENERATED_DIR / "rules_reload_phase1.cfg",
        [
            "HTTP_ALLOW,TCP,*,*,*,80,FORWARD",
            "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD",
            "PORT8443_ALLOW,TCP,*,*,*,8443,FORWARD",
            "SSH_BLOCK,TCP,*,*,*,22,DROP",
            "DEFAULT,*,*,*,*,*,FORWARD",
        ],
    )

    write_rules(
        GENERATED_DIR / "rules_reload_phase2.cfg",
        [
            "HTTP_ALLOW,TCP,*,*,*,80,FORWARD",
            "HTTPS_ALLOW,TCP,*,*,*,443,FORWARD",
            "PORT8443_DROP,TCP,*,*,*,8443,DROP",
            "SSH_BLOCK,TCP,*,*,*,22,DROP",
            "DEFAULT,*,*,*,*,*,FORWARD",
        ],
    )


def generate_assets() -> Path:
    generate_pcaps()
    generate_rules()
    return GENERATED_DIR


if __name__ == "__main__":
    path = generate_assets()
    print(f"Generated test assets in {path}")
