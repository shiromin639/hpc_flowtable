#!/usr/bin/env python3
from __future__ import annotations

import socket
import struct
from pathlib import Path


PCAP_DIR = Path(__file__).resolve().parent
FLOW_COUNTS = [100, 1_000, 10_000, 100_000, 1_000_000]


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
    return dst + src + struct.pack("!H", 0x0800)


def ipv4_header(src_ip: str, dst_ip: str, proto: int, payload_len: int,
        ident: int) -> bytes:
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


def udp_segment(src_ip: str, dst_ip: str, sport: int, dport: int) -> bytes:
    length = 8
    header = struct.pack("!HHHH", sport, dport, length, 0)
    pseudo = socket.inet_aton(src_ip) + socket.inet_aton(dst_ip) + \
        struct.pack("!BBH", 0, 17, length)
    udp_checksum = checksum(pseudo + header)
    if udp_checksum == 0:
        udp_checksum = 0xFFFF
    return struct.pack("!HHHH", sport, dport, length, udp_checksum)


def udp_pkt(src_ip: str, dst_ip: str, sport: int, dport: int,
        ident: int) -> bytes:
    l4 = udp_segment(src_ip, dst_ip, sport, dport)
    return ethernet_header() + ipv4_header(src_ip, dst_ip, 17, len(l4), ident) + l4


def flow_tuple(flow_id: int) -> tuple[str, str, int, int]:
    src_a = 10 + ((flow_id >> 24) & 0xFF)
    src_b = (flow_id >> 16) & 0xFF
    src_c = (flow_id >> 8) & 0xFF
    src_d = (flow_id & 0xFF) or 1

    dst_a = 192
    dst_b = 168
    dst_c = (flow_id >> 12) & 0xFF
    dst_d = ((flow_id >> 4) & 0xFE) + 1

    src_ip = f"{src_a}.{src_b}.{src_c}.{src_d}"
    dst_ip = f"{dst_a}.{dst_b}.{dst_c}.{dst_d}"
    sport = 1024 + (flow_id % 60_000)
    dport = 20000 + (flow_id % 128)
    return src_ip, dst_ip, sport, dport


def write_flow_pcap(path: Path, flow_count: int) -> None:
    with path.open("wb") as fp:
        fp.write(struct.pack("<IHHIIII",
            0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))

        ts_sec = 0
        ts_usec = 0
        for flow_id in range(flow_count):
            src_ip, dst_ip, sport, dport = flow_tuple(flow_id)
            pkt = udp_pkt(src_ip, dst_ip, sport, dport, flow_id)
            fp.write(struct.pack("<IIII", ts_sec, ts_usec, len(pkt), len(pkt)))
            fp.write(pkt)
            ts_usec += 1
            if ts_usec >= 1_000_000:
                ts_sec += 1
                ts_usec = 0


def main() -> None:
    for flow_count in FLOW_COUNTS:
        path = PCAP_DIR / f"flow_sweep_{flow_count}.pcap"
        print(f"Generating {path.name} with {flow_count} unique flows")
        write_flow_pcap(path, flow_count)


if __name__ == "__main__":
    main()
