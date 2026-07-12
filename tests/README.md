# Test Harness

Main entry points:

- `flowcore_functional_tests.c`: mentor-facing C functional suite that generates test PCAPs, runs the real `flowcore` binary, and checks stdout counters.
- `flowcore_unit_tests.c`: fast C unit tests for packet parsing, protocol accounting, and stats rate helpers.
- `generate_test_assets.py`: creates deterministic PCAP and rule fixtures under `tests/generated/`.
- `run_functional_tests.py`: runs the software-only regression suite.
- `run_performance_tests.py`: runs baseline performance scenarios and writes `perf_results.json`.
- `flowcore_test_lib.py`: shared launcher, shutdown, and stats parsing helpers.
- `test_strategy.md`: scope, coverage, and limitations of the software-only approach.

The functional suite uses `net_pcap` RX in every case and `net_null` for TX.
The current application configures four TX queues on `PORT_OUT`, which makes
`net_pcap` TX incompatible with the default four-worker topology in this repo.

Typical workflow:

```bash
meson compile -C build flowcore flowcore_functional_tests
meson test -C build flowcore-unit
./build/flowcore_functional_tests

python3 tests/generate_test_assets.py
python3 tests/run_functional_tests.py
python3 tests/run_performance_tests.py
```

The generated directory is intentionally git-ignored. The scripts recreate any missing fixtures on demand.
