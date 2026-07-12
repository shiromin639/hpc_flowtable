#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
import tempfile
import time
from pathlib import Path

from flowcore_test_lib import run_flowcore
from generate_test_assets import generate_assets


def run_scenario(name: str, rx_name: str, rules_name: str,
        runtime_sec: float = 4.0) -> dict:
    assets = generate_assets()
    with tempfile.TemporaryDirectory(prefix=f"flowcore_perf_{name}_") as tmpdir:
        started = time.time()
        result = run_flowcore(
            assets / rx_name,
            None,
            assets / rules_name,
            runtime_sec=runtime_sec,
            output_mode="null",
        )
        elapsed = time.time() - started

        return {
            "name": name,
            "elapsed_sec": round(elapsed, 3),
            "created_flows": result.metrics["created_flows"],
            "deleted_flows": result.metrics["deleted_flows"],
            "active_flows": result.metrics["active_flows"],
            "spi_forwarded": result.metrics["spi_forwarded"],
            "spi_drops": result.metrics["spi_drops"],
            "tx_drops": result.metrics["tx_drops"],
            "rule_checks": result.metrics["rule_checks"],
            "protocols": result.metrics["protocols"],
        }


def main() -> int:
    scenarios = [
        ("baseline_fixed_256", "perf_fixed_256.pcap", "rules_default.cfg"),
        ("flow_creation_20k", "perf_unique_20k.pcap", "rules_default.cfg"),
        ("rule_mix_20k", "perf_rule_mix_20k.pcap", "rules_default.cfg"),
        ("aging_smoke", "aging_single.pcap", "rules_default.cfg"),
    ]

    results = [run_scenario(*scenario) for scenario in scenarios]

    out_dir = generate_assets()
    result_path = out_dir / "perf_results.json"
    result_path.write_text(json.dumps(results, indent=2), encoding="utf-8")

    print("Performance scenarios completed:")
    for result in results:
        print(
            f"- {result['name']}: forwarded={result['spi_forwarded']} "
            f"created={result['created_flows']} dropped={result['spi_drops']} "
            f"rule_checks={result['rule_checks']} elapsed={result['elapsed_sec']}s"
        )
    print(f"\nWrote results to {result_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
