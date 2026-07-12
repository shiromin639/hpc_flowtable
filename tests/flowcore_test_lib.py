#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import signal
import struct
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO


REPO_ROOT = Path(__file__).resolve().parents[1]
FLOWCORE_BIN = REPO_ROOT / "build" / "flowcore"
DEFAULT_LCORES = "0-5"
DEFAULT_MBUF_COUNT = "32768"
DEFAULT_MEM_MB = "512"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


@dataclass
class FlowcoreRunResult:
    command: list[str]
    returncode: int
    raw_output: str
    clean_output: str
    metrics: dict
    tx_packet_count: int
    tx_pcap: Path | None


@dataclass
class FlowcoreHandle:
    proc: subprocess.Popen[None]
    command: list[str]
    output_file: TextIO


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def count_pcap_packets(path: Path) -> int:
    if not path.exists():
        return 0

    with path.open("rb") as fp:
        global_hdr = fp.read(24)
        if len(global_hdr) != 24:
            return 0

        magic = global_hdr[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            endian = "<"
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian = ">"
        else:
            return 0

        rec_hdr = struct.Struct(f"{endian}IIII")
        count = 0
        while True:
            raw_hdr = fp.read(rec_hdr.size)
            if not raw_hdr:
                break
            if len(raw_hdr) != rec_hdr.size:
                break
            _, _, incl_len, _ = rec_hdr.unpack(raw_hdr)
            fp.seek(incl_len, os.SEEK_CUR)
            count += 1

    return count


def parse_metrics(output: str) -> dict:
    text = strip_ansi(output)

    def last_int(pattern: str, default: int = 0) -> int:
        matches = re.findall(pattern, text, flags=re.MULTILINE)
        return int(matches[-1]) if matches else default

    proto_match = re.findall(
        r"Protocols\s*:\s*HTTP=(\d+)\s+HTTPS=(\d+)\s+DNS=(\d+)\s+TCP=(\d+)\s+UDP=(\d+)\s+OTHER=(\d+)",
        text,
        flags=re.MULTILINE,
    )
    if proto_match:
        http, https, dns, tcp, udp, other = map(int, proto_match[-1])
    else:
        http = https = dns = tcp = udp = other = 0

    return {
        "created_flows": last_int(r"Created Flows\s*:\s*(\d+)"),
        "deleted_flows": last_int(r"Deleted/Timeout:\s*(\d+)"),
        "active_flows": last_int(r"Active Flows\s*:\s*(\d+)"),
        "rx_filtered": last_int(r"RX Filtered\s*:\s*(\d+)"),
        "spi_drops": last_int(r"SPI Drops\s*:\s*(\d+)"),
        "tx_drops": last_int(r"TX Drops\s*:\s*(\d+)"),
        "active_rules": last_int(r"Active Rules\s*:\s*(\d+)"),
        "rule_version": last_int(r"Active Rules\s*:\s*\d+\s+Rules\s+\|\s*(\d+)"),
        "spi_forwarded": last_int(r"SPI Forwarded\s*:\s*(\d+)"),
        "rechecks": last_int(r"SPI Forwarded\s*:\s*\d+\s+Pkts\s+\|\s*(\d+)"),
        "protocols": {
            "http": http,
            "https": https,
            "dns": dns,
            "tcp": tcp,
            "udp": udp,
            "other": other,
        },
        "reload_seen": "[SPI] Reloaded" in text,
        "raw_text": text,
    }


def build_flowcore_cmd(rx_pcap: Path, tx_pcap: Path | None,
        infinite_rx: bool, output_mode: str) -> list[str]:
    rx_vdev = f"net_pcap0,rx_pcap={rx_pcap},infinite_rx={1 if infinite_rx else 0}"

    cmd = [
        str(FLOWCORE_BIN),
        "--no-huge",
        "--no-pci",
        "--in-memory",
        "--no-shconf",
        "--no-telemetry",
        "-l",
        DEFAULT_LCORES,
        "-n",
        "4",
        "-m",
        DEFAULT_MEM_MB,
        "--vdev",
        rx_vdev,
    ]

    if output_mode == "pcap":
        if tx_pcap is None:
            raise ValueError("tx_pcap is required for pcap output mode")
        cmd.extend(["--vdev", f"net_pcap1,tx_pcap={tx_pcap}"])
    elif output_mode == "null":
        cmd.extend(["--vdev", "net_null0"])
    else:
        raise ValueError(f"unsupported output mode: {output_mode}")

    return cmd


def launch_flowcore(rx_pcap: Path, tx_pcap: Path | None, rules_path: Path,
        infinite_rx: bool = False, output_mode: str = "null"
        ) -> FlowcoreHandle:
    env = os.environ.copy()
    env["XDG_RUNTIME_DIR"] = "/tmp"
    env["FLOWCORE_NUM_MBUFS"] = env.get("FLOWCORE_NUM_MBUFS", DEFAULT_MBUF_COUNT)
    env["FLOWCORE_RULES_PATH"] = str(rules_path)

    cmd = build_flowcore_cmd(rx_pcap, tx_pcap, infinite_rx, output_mode)
    output_file = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    proc = subprocess.Popen(
        cmd,
        cwd=REPO_ROOT,
        env=env,
        stdout=output_file,
        stderr=subprocess.STDOUT,
    )
    return FlowcoreHandle(proc=proc, command=cmd, output_file=output_file)


def stop_flowcore(handle: FlowcoreHandle, timeout_sec: float = 5.0) -> str:
    proc = handle.proc

    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)

    try:
        proc.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout_sec)

    handle.output_file.flush()
    handle.output_file.seek(0)
    stdout = handle.output_file.read()
    handle.output_file.close()
    return stdout


def run_flowcore(rx_pcap: Path, tx_pcap: Path | None, rules_path: Path,
        runtime_sec: float = 3.0, infinite_rx: bool = False,
        output_mode: str = "null") -> FlowcoreRunResult:
    handle = launch_flowcore(rx_pcap, tx_pcap, rules_path,
            infinite_rx=infinite_rx, output_mode=output_mode)
    time.sleep(runtime_sec)
    output = stop_flowcore(handle)
    metrics = parse_metrics(output)
    tx_count = count_pcap_packets(tx_pcap) if tx_pcap is not None else 0

    return FlowcoreRunResult(
        command=handle.command,
        returncode=handle.proc.returncode,
        raw_output=output,
        clean_output=metrics["raw_text"],
        metrics=metrics,
        tx_packet_count=tx_count,
        tx_pcap=tx_pcap,
    )
