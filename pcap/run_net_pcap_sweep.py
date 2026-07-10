#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import time
from dataclasses import dataclass, asdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PCAP_DIR = REPO_ROOT / "pcap"
FLOWCORE_BIN = REPO_ROOT / "build" / "flowcore"
RULES_PATH = REPO_ROOT / "rules.cfg"
RESULTS_PATH = PCAP_DIR / "net_pcap_sweep_results.json"
FLOW_COUNTS = [1_000, 10_000, 100_000, 1_000_000]
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


@dataclass
class SweepResult:
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
    mbufs: int
    mem_mb: int
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
        for value in re.findall(rf"{label}\s*:\s*(\d+)\s+PPS", text, flags=re.MULTILINE)
    ]


def build_cmd(pcap_path: Path, mem_mb: int) -> list[str]:
    return [
        str(FLOWCORE_BIN),
        "--no-huge",
        "--no-pci",
        "--in-memory",
        "--no-shconf",
        "--no-telemetry",
        "-l", "0-5",
        "-n", "4",
        "-m", str(mem_mb),
        "--vdev", f"net_pcap0,rx_pcap={pcap_path},infinite_rx=1",
        "--vdev", "net_null0",
    ]


def run_one(flow_count: int) -> SweepResult:
    pcap_path = PCAP_DIR / f"flow_sweep_{flow_count}.pcap"
    if not pcap_path.exists():
        raise FileNotFoundError(pcap_path)

    if flow_count >= 1_000_000:
        mbufs = 600_000
        mem_mb = 2048
    else:
        mbufs = max(flow_count, 32_768)
        mem_mb = 512 if flow_count <= 10_000 else 1024

    env = os.environ.copy()
    env["XDG_RUNTIME_DIR"] = "/tmp"
    env["FLOWCORE_NUM_MBUFS"] = str(mbufs)
    env["FLOWCORE_MBUF_DATA_SIZE"] = "512"
    env["FLOWCORE_RULES_PATH"] = str(RULES_PATH)

    cmd = build_cmd(pcap_path, mem_mb)
    proc = subprocess.Popen(
        cmd,
        cwd=REPO_ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    runtime_sec = 4.0 if flow_count < 1_000_000 else 6.0
    time.sleep(runtime_sec)
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)

    try:
        output, _ = proc.communicate(timeout=10.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        output, _ = proc.communicate(timeout=10.0)

    clean = strip_ansi(output)
    rx_series = [value for value in parse_pps_series(clean, "RX Throughput") if value > 0]
    tx_series = [value for value in parse_pps_series(clean, "TX Throughput") if value > 0]

    return SweepResult(
        flow_count=flow_count,
        best_rx_pps=max(rx_series, default=0),
        best_tx_pps=max(tx_series, default=0),
        last_nonzero_rx_pps=rx_series[-1] if rx_series else 0,
        last_nonzero_tx_pps=tx_series[-1] if tx_series else 0,
        active_flows=last_int(r"Active Flows\s*:\s*(\d+)", clean),
        created_flows=last_int(r"Created Flows\s*:\s*(\d+)", clean),
        deleted_flows=last_int(r"Deleted/Timeout:\s*(\d+)", clean),
        spi_drops=last_int(r"SPI Drops\s*:\s*(\d+)", clean),
        tx_drops=last_int(r"TX Drops\s*:\s*(\d+)", clean),
        runtime_sec=runtime_sec,
        mbufs=mbufs,
        mem_mb=mem_mb,
        returncode=proc.returncode,
        command=cmd,
    )


def main() -> None:
    results: list[SweepResult] = []
    for flow_count in FLOW_COUNTS:
        print(f"Running net_pcap sweep for {flow_count} flows")
        result = run_one(flow_count)
        results.append(result)
        print(
            f"  best_RX={result.best_rx_pps} PPS best_TX={result.best_tx_pps} PPS "
            f"last_TX={result.last_nonzero_tx_pps} active={result.active_flows} "
            f"created={result.created_flows} deleted={result.deleted_flows}"
        )

    RESULTS_PATH.write_text(
        json.dumps([asdict(result) for result in results], indent=2) + "\n",
        encoding="ascii",
    )
    print(f"Saved results to {RESULTS_PATH}")


if __name__ == "__main__":
    main()
