#!/usr/bin/env python3
from __future__ import annotations

import shutil
import signal
import sys
import tempfile
import time
from pathlib import Path

from flowcore_test_lib import launch_flowcore, parse_metrics, stop_flowcore
from generate_test_assets import generate_assets


class FunctionalTestFailure(RuntimeError):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise FunctionalTestFailure(message)


def run_basic_case(name: str, rx_name: str, rules_name: str,
        runtime_sec: float, assertions) -> tuple[bool, str]:
    assets = generate_assets()
    with tempfile.TemporaryDirectory(prefix=f"flowcore_{name}_") as tmpdir:
        from flowcore_test_lib import run_flowcore
        result = run_flowcore(
            assets / rx_name,
            None,
            assets / rules_name,
            runtime_sec=runtime_sec,
            output_mode="null",
        )
        assertions(result)
        return True, (
            f"created={result.metrics['created_flows']} "
            f"deleted={result.metrics['deleted_flows']} "
            f"spi_forwarded={result.metrics['spi_forwarded']} "
            f"spi_drops={result.metrics['spi_drops']}"
        )


def test_same_flow_reuse():
    def assertions(result):
        expect(result.returncode in (0, 130), f"unexpected return code {result.returncode}")
        expect(result.metrics["created_flows"] == 1,
               f"expected 1 created flow, got {result.metrics['created_flows']}")
        expect(result.metrics["spi_forwarded"] == 10,
               f"expected 10 forwarded packets, got {result.metrics['spi_forwarded']}")
        expect(result.metrics["spi_drops"] == 0,
               f"expected 0 SPI drops, got {result.metrics['spi_drops']}")
    return run_basic_case("same_flow_reuse", "same_flow_10.pcap",
            "rules_default.cfg", 2.5, assertions)


def test_unique_flow_creation():
    def assertions(result):
        expect(result.metrics["created_flows"] == 32,
               f"expected 32 created flows, got {result.metrics['created_flows']}")
        expect(result.metrics["spi_forwarded"] == 32,
               f"expected 32 forwarded packets, got {result.metrics['spi_forwarded']}")
    return run_basic_case("unique_flow_creation", "unique_flow_32.pcap",
            "rules_default.cfg", 2.5, assertions)


def test_rule_drop_ssh():
    def assertions(result):
        expect(result.metrics["spi_forwarded"] == 5,
               f"expected 5 forwarded packets, got {result.metrics['spi_forwarded']}")
        expect(result.metrics["spi_drops"] >= 1,
               f"expected at least 1 SPI drop, got {result.metrics['spi_drops']}")
    return run_basic_case("rule_drop_ssh", "mixed_rules_6.pcap",
            "rules_default.cfg", 2.5, assertions)


def test_protocol_accounting():
    def assertions(result):
        proto = result.metrics["protocols"]
        expect(proto["http"] == 1, f"expected HTTP=1, got {proto['http']}")
        expect(proto["https"] == 1, f"expected HTTPS=1, got {proto['https']}")
        expect(proto["dns"] == 1, f"expected DNS=1, got {proto['dns']}")
        expect(proto["tcp"] == 2, f"expected TCP=2, got {proto['tcp']}")
        expect(proto["udp"] == 1, f"expected UDP=1, got {proto['udp']}")
        expect(proto["other"] == 0, f"expected OTHER=0, got {proto['other']}")
    return run_basic_case("protocol_accounting", "mixed_rules_6.pcap",
            "rules_default.cfg", 2.5, assertions)


def test_aging_cleanup():
    def assertions(result):
        expect(result.metrics["created_flows"] == 1,
               f"expected 1 created flow, got {result.metrics['created_flows']}")
        expect(result.metrics["deleted_flows"] >= 1,
               f"expected at least 1 deleted flow, got {result.metrics['deleted_flows']}")
    return run_basic_case("aging_cleanup", "aging_single.pcap",
            "rules_default.cfg", 3.5, assertions)


def test_hot_reload():
    assets = generate_assets()
    with tempfile.TemporaryDirectory(prefix="flowcore_hot_reload_") as tmpdir:
        tmpdir_path = Path(tmpdir)
        active_rules = tmpdir_path / "rules.cfg"
        shutil.copyfile(assets / "rules_reload_phase1.cfg", active_rules)

        proc, cmd = launch_flowcore(
            assets / "reload_8443_loop.pcap",
            None,
            active_rules,
            infinite_rx=True,
            output_mode="null",
        )

        time.sleep(1.5)
        shutil.copyfile(assets / "rules_reload_phase2.cfg", active_rules)
        proc.send_signal(signal.SIGUSR1)
        time.sleep(2.0)

        output = stop_flowcore(proc)
        metrics = parse_metrics(output)

        expect(proc.returncode in (0, 130), f"unexpected return code {proc.returncode}")
        expect(metrics["reload_seen"], "expected reload message in output")
        expect(metrics["rule_version"] >= 2,
               f"expected rule version >= 2, got {metrics['rule_version']}")
        expect(metrics["spi_drops"] > 0,
               f"expected SPI drops after reload, got {metrics['spi_drops']}")
        expect(metrics["spi_forwarded"] > 0,
               f"expected some forwarded packets before reload, got {metrics['spi_forwarded']}")

        return True, (
            f"rule_version={metrics['rule_version']} "
            f"spi_forwarded={metrics['spi_forwarded']} "
            f"spi_drops={metrics['spi_drops']}"
        )


def main() -> int:
    tests = [
        ("same_flow_reuse", test_same_flow_reuse),
        ("unique_flow_creation", test_unique_flow_creation),
        ("rule_drop_ssh", test_rule_drop_ssh),
        ("protocol_accounting", test_protocol_accounting),
        ("aging_cleanup", test_aging_cleanup),
        ("hot_reload", test_hot_reload),
    ]

    failures = 0
    for name, fn in tests:
        try:
            ok, detail = fn()
            print(f"[PASS] {name}: {detail}")
        except Exception as exc:
            failures += 1
            print(f"[FAIL] {name}: {exc}")

    if failures:
        print(f"\nFunctional tests failed: {failures}/{len(tests)}")
        return 1

    print(f"\nFunctional tests passed: {len(tests)}/{len(tests)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
