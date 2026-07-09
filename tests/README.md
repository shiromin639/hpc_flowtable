# Test Harness

Main entry points:

- `generate_test_assets.py`: creates deterministic PCAP and rule fixtures under `tests/generated/`.
- `run_functional_tests.py`: runs the software-only regression suite.
- `run_performance_tests.py`: runs baseline performance scenarios and writes `perf_results.json`.
- `flowcore_test_lib.py`: shared launcher, shutdown, and stats parsing helpers.
- `test_strategy.md`: scope, coverage, and limitations of the software-only approach.

Typical workflow:

```bash
python3 tests/generate_test_assets.py
python3 tests/run_functional_tests.py
python3 tests/run_performance_tests.py
```

The generated directory is intentionally git-ignored. The scripts recreate any missing fixtures on demand.
