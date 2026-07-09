# Flowcore Software-Only Test Strategy

## Scope

This test plan targets the current `flowcore` design using only software devices. It is built for machines without a NIC and avoids any dependency on `testpmd`, `pktgen`, or privileged veth setup for the core regression suite.

Topology used by the automated tests:

`PCAP file -> net_pcap RX -> dispatcher -> workers -> SPI -> net_null TX`

The suite validates:

- flow creation and reuse
- SPI allow/drop behavior
- protocol counters
- rule hot reload
- flow aging
- relative performance baselines on software replay

## Why This Strategy

The current repo already supports a software path through DPDK virtual devices. That path is the most repeatable option for CI and for local development on non-NIC machines.

The key design choice is to assert on:

- `flowcore` stdout statistics
- SPI reload log messages

This gives deterministic signals without requiring packet sniffing on host interfaces or a multi-queue egress device.

## Functional Tests

### 1. Same Flow Reuse

Input:
- 10 packets with the same 5-tuple

Checks:
- `Created Flows == 1`
- `SPI Forwarded == 10`
- no SPI drops

Purpose:
- verifies steady-state hit path and flow affinity cache reuse

### 2. Unique Flow Creation

Input:
- 32 packets with unique 5-tuples

Checks:
- `Created Flows == 32`
- `SPI Forwarded == 32`

Purpose:
- verifies cold-path flow creation behavior

### 3. Rule Drop

Input:
- HTTP, HTTPS, DNS, SSH, generic TCP, generic UDP

Checks:
- exactly the SSH packet is dropped
- SPI drop counter increments

Purpose:
- verifies rule matching and action enforcement

### 4. Protocol Accounting

Input:
- same mixed packet set as the rule-drop case

Checks:
- global protocol counters match:
  - `HTTP=1`
  - `HTTPS=1`
  - `DNS=1`
  - `TCP=2`
  - `UDP=1`
  - `OTHER=0`

Purpose:
- verifies worker-side traffic classification

Note:
- `OTHER` is expected to stay `0` in the current architecture because dispatcher drops non-TCP/UDP traffic before packets ever reach workers.
- Protocol counters are updated before SPI action enforcement, so a dropped SSH packet still contributes to the generic TCP count.

### 5. Aging Cleanup

Input:
- one single flow, then idle time

Checks:
- one flow created
- flow later deleted by timeout
- final active flow count returns to zero

Purpose:
- verifies aging loop behavior without requiring timestamp-aware replay

### 6. Hot Reload

Input:
- looping PCAP with TCP destination port `8443`
- initial rules allow `8443`
- reloaded rules drop `8443`

Checks:
- reload log is printed
- rule version increments
- some packets are forwarded before reload
- drops occur after reload

Purpose:
- verifies runtime rule replacement plus lazy per-flow action invalidation

## Performance Tests

These are software baseline tests. They are not line-rate NIC benchmarks, but they are useful for regression tracking and comparing code changes.

### 1. Baseline Fixed-Flow Replay

Traffic:
- many packets over a limited flow set

Measure:
- throughput stability
- SPI forwarded count
- TX drop count

Purpose:
- approximate best-case steady-state behavior

### 2. Flow Creation Pressure

Traffic:
- mostly unique 5-tuples

Measure:
- created flow count
- throughput under cold-path pressure

Purpose:
- stress hash insert and per-flow initialization cost

### 3. Rule Mix Replay

Traffic:
- mix of allow/drop ports

Measure:
- SPI drop rate
- revalidation cost
- throughput impact versus baseline

Purpose:
- observe SPI overhead under a realistic mixed policy set

### 4. Aging Smoke

Traffic:
- short input followed by idle time

Measure:
- deletion count
- active flow convergence to zero

Purpose:
- ensures aging still behaves under benchmark harness execution

## How To Run

Generate assets:

```bash
python3 tests/generate_test_assets.py
```

Run the functional suite:

```bash
python3 tests/run_functional_tests.py
```

Run the software performance suite:

```bash
python3 tests/run_performance_tests.py
```

## Current Limitations

- This suite depends on DPDK `net_pcap` support being present in the local build.
- It measures software replay behavior, not hardware NIC behavior.
- It does not validate actual worker assignment directly because worker ID is not exported as an external observable in the current runtime path.
- It does not cover non-TCP/UDP worker classification because dispatcher filters those packets out earlier in the pipeline.

## Recommended Next Extensions

- Add a machine-readable stats export mode such as JSON lines.
- Add an optional test mode that exits automatically after RX EOF plus a quiet grace period.
- Add a small control socket or CLI command to expose per-worker counters in a parseable format.
- Add memif-loop tests later if you want a closer approximation to a live traffic source without requiring physical NICs.
