#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

sys.dont_write_bytecode = True

from generate_fixed_packet_sweep import DEFAULT_FLOW_COUNTS, DEFAULT_TOTAL_PACKETS, pcap_name


REPO_ROOT = Path(__file__).resolve().parents[1]
PCAP_DIR = REPO_ROOT / "pcap"
FLOWCORE_BIN = REPO_ROOT / "build" / "flowcore"
RULES_PATH = REPO_ROOT / "rules.cfg"
RESULTS_PATH = PCAP_DIR / "fixed_packet_sweep_results.json"
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


@dataclass
class FixedPacketResult:
    total_packets: int
    flow_count: int
    best_rx_pps: int
    best_tx_pps: int
    last_nonzero_rx_pps: int
    last_nonzero_tx_pps: int
    active_flows: int
    created_flows: int
    deleted_flows: int
    spi_drops: int
    tx_drops: int
    runtime_sec: float
    returncode: int
    command: list[str]


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def last_int(pattern: str, text: str, default: int = 0) -> int:
    matches = re.findall(pattern, text, flags=re.MULTILINE)
    return int(matches[-1]) if matches else default


def parse_pps_series(text: str, label: str) -> list[int]:
    return [
        int(value)
        for value in re.findall(rf"{label}\s*:\s*(\d+)\s+PPS", text,
            flags=re.MULTILINE)
    ]


def build_cmd(pcap_path: Path) -> list[str]:
    return [
        str(FLOWCORE_BIN),
        "--no-pci",
        "--in-memory",
        "--no-shconf",
        "--no-telemetry",
        "-l", "0-5",
        "-n", "4",
        "-m", "2048",
        "--vdev", f"net_pcap0,rx_pcap={pcap_path},infinite_rx=1",
        "--vdev", "net_null0",
    ]


def run_one(total_packets: int, flow_count: int,
        runtime_sec: float) -> FixedPacketResult:
    pcap_path = PCAP_DIR / pcap_name(total_packets, flow_count)
    if not pcap_path.exists():
        raise FileNotFoundError(pcap_path)

    env = os.environ.copy()
    env["XDG_RUNTIME_DIR"] = "/tmp"
    env["FLOWCORE_NUM_MBUFS"] = "600000"
    env["FLOWCORE_MBUF_DATA_SIZE"] = "512"
    env["FLOWCORE_RULES_PATH"] = str(RULES_PATH)

    cmd = build_cmd(pcap_path)
    proc = subprocess.Popen(
        cmd,
        cwd=REPO_ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    time.sleep(runtime_sec)
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)

    try:
        output, _ = proc.communicate(timeout=10.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        output, _ = proc.communicate(timeout=10.0)

    clean = strip_ansi(output)
    rx_series = [value for value in parse_pps_series(clean, "RX Throughput")
                 if value > 0]
    tx_series = [value for value in parse_pps_series(clean, "TX Throughput")
                 if value > 0]

    return FixedPacketResult(
        total_packets=total_packets,
        flow_count=flow_count,
        best_rx_pps=max(rx_series, default=0),
        best_tx_pps=max(tx_series, default=0),
        last_nonzero_rx_pps=rx_series[-1] if rx_series else 0,
        last_nonzero_tx_pps=tx_series[-1] if tx_series else 0,
        active_flows=last_int(r"Active Flows\s*:\s*(\d+)", clean),
        created_flows=last_int(r"Created Flows\s*:\s*(\d+)", clean),
        deleted_flows=last_int(r"Deleted Flows\s*:\s*(\d+)", clean),
        spi_drops=last_int(r"SPI Drops\s*:\s*(\d+)", clean),
        tx_drops=last_int(r"TX Drops\s*:\s*(\d+)", clean),
        runtime_sec=runtime_sec,
        returncode=proc.returncode,
        command=cmd,
    )


def main() -> None:
    total_packets = int(os.environ.get("FLOWCORE_SWEEP_PACKETS",
        str(DEFAULT_TOTAL_PACKETS)))
    runtime_sec = float(os.environ.get("FLOWCORE_SWEEP_RUNTIME", "8.0"))
    flow_counts = [
        int(value)
        for value in os.environ.get(
            "FLOWCORE_SWEEP_FLOWS",
            " ".join(str(value) for value in DEFAULT_FLOW_COUNTS),
        ).split()
    ]

    results: list[FixedPacketResult] = []
    for flow_count in flow_counts:
        if flow_count > 1_048_576:
            print(
                f"warning: {flow_count:,} flows exceeds current 1M hash table; "
                "expect capacity drops unless HASH_ENTRIES is raised")
        print(f"running {total_packets:,} packets / {flow_count:,} flows")
        result = run_one(total_packets, flow_count, runtime_sec)
        results.append(result)
        print(
            f"  best_RX={result.best_rx_pps} best_TX={result.best_tx_pps} "
            f"active={result.active_flows} created={result.created_flows} "
            f"deleted={result.deleted_flows} tx_drops={result.tx_drops}")

    RESULTS_PATH.write_text(
        json.dumps([asdict(result) for result in results], indent=2) + "\n",
        encoding="ascii")
    print(f"saved {RESULTS_PATH}")


if __name__ == "__main__":
    main()
