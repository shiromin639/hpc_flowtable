#!/usr/bin/env python3
from __future__ import annotations

import argparse
import struct
import time
from pathlib import Path


PCAP_DIR = Path(__file__).resolve().parent
DEFAULT_TOTAL_PACKETS = 10_000_000
DEFAULT_FLOW_COUNTS = [10_000, 100_000, 1_000_000, 10_000_000]
DEFAULT_CACHE_FLOWS = 1_000_000

PCAP_GLOBAL_HEADER = struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)
PCAP_RECORD_HEADER = struct.Struct("<IIII")
ETH_HEADER = bytes.fromhex("001122334455aabbccddeeff0800")
IPV4_HEADER = struct.Struct("!BBHHHBBHII")
UDP_HEADER = struct.Struct("!HHHH")

IPV4_MIN = (10 << 24) | 1
IPV4_MAX = (10 << 24) | (255 << 16) | (255 << 8) | 254
DST_IP = (192 << 24) | (168 << 16) | (0 << 8) | 1
PROTO_UDP = 17
PACKET_LEN = 14 + 20 + 8


def checksum16(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"

    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]

    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def pcap_name(total_packets: int, flow_count: int) -> str:
    return f"fixed_packets_{total_packets}_pkts_{flow_count}_flows.pcap"


def udp_packet(flow_id: int) -> bytes:
    src_ip = IPV4_MIN + flow_id
    if src_ip > IPV4_MAX:
        raise ValueError("flow count exceeds address space in 10.0.0.0/8")

    sport = 1024 + (flow_id % 64512)
    dport = 20000 + ((flow_id // 64512) % 40000)
    udp = UDP_HEADER.pack(sport, dport, 8, 0)

    ip_without_csum = IPV4_HEADER.pack(
        0x45, 0, 20 + len(udp), flow_id & 0xFFFF, 0, 64, PROTO_UDP, 0,
        src_ip, DST_IP)
    ip_csum = checksum16(ip_without_csum)
    ip = IPV4_HEADER.pack(
        0x45, 0, 20 + len(udp), flow_id & 0xFFFF, 0, 64, PROTO_UDP,
        ip_csum, src_ip, DST_IP)

    return ETH_HEADER + ip + udp


def write_pcap(path: Path, total_packets: int, flow_count: int,
        cache_flows: int, overwrite: bool) -> None:
    if path.exists() and not overwrite:
        print(f"skip existing {path.name}")
        return

    if flow_count > total_packets:
        raise ValueError("flow_count cannot exceed total_packets")

    print(
        f"generating {path.name}: {total_packets:,} packets, "
        f"{flow_count:,} unique flows")

    cached_packets: list[bytes] | None = None
    if flow_count <= cache_flows:
        print(f"  caching {flow_count:,} packet templates")
        cached_packets = [udp_packet(flow_id) for flow_id in range(flow_count)]

    tmp_path = path.with_suffix(path.suffix + ".tmp")
    started = time.monotonic()
    next_report = 1_000_000
    buffer = bytearray()
    buffer_limit = 8 * 1024 * 1024

    with tmp_path.open("wb", buffering=1024 * 1024) as fp:
        fp.write(PCAP_GLOBAL_HEADER)

        for pkt_id in range(total_packets):
            ts_sec, ts_usec = divmod(pkt_id, 1_000_000)
            flow_id = pkt_id % flow_count
            pkt = cached_packets[flow_id] if cached_packets is not None else udp_packet(flow_id)

            buffer += PCAP_RECORD_HEADER.pack(ts_sec, ts_usec, PACKET_LEN, PACKET_LEN)
            buffer += pkt

            if len(buffer) >= buffer_limit:
                fp.write(buffer)
                buffer.clear()

            done = pkt_id + 1
            if done >= next_report:
                elapsed = max(time.monotonic() - started, 0.001)
                print(f"  {done:,}/{total_packets:,} packets ({done / elapsed:,.0f} pkt/s)")
                next_report += 1_000_000

        if buffer:
            fp.write(buffer)

    tmp_path.replace(path)
    size_mb = path.stat().st_size / (1024 * 1024)
    elapsed = max(time.monotonic() - started, 0.001)
    print(f"  wrote {size_mb:.1f} MiB in {elapsed:.1f}s")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate fixed-packet-count PCAPs with different unique-flow counts.")
    parser.add_argument("--packets", type=int, default=DEFAULT_TOTAL_PACKETS)
    parser.add_argument("--flows", type=int, nargs="+", default=DEFAULT_FLOW_COUNTS)
    parser.add_argument("--out-dir", type=Path, default=PCAP_DIR)
    parser.add_argument("--cache-flows", type=int, default=DEFAULT_CACHE_FLOWS)
    parser.add_argument("--overwrite", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    estimated_bytes = 24 + len(args.flows) * args.packets * (16 + PACKET_LEN)
    print(f"estimated total size: {estimated_bytes / (1024 ** 3):.2f} GiB")

    for flow_count in args.flows:
        path = args.out_dir / pcap_name(args.packets, flow_count)
        write_pcap(path, args.packets, flow_count, args.cache_flows,
                args.overwrite)


if __name__ == "__main__":
    main()
