# Flowcore Defense Manual

This document is the mentor-facing explanation of the current `flowcore`
implementation in this repository.

It is written from the real source code, not from an idealized architecture.
Use this document for:

- final report writing
- slide preparation
- project defense
- answering questions about why the code is structured this way

Related source files:

- `include/common.h`
- `include/app_init.h`
- `include/app_threads.h`
- `include/flow_table.h`
- `include/spi_engine.h`
- `include/stats.h`
- `src/main.c`
- `src/app_init.c`
- `src/dispatcher.c`
- `src/worker.c`
- `src/flow_table.c`
- `src/spi_engine.c`
- `src/stats.c`

## 1. Executive Summary

`flowcore` is a DPDK-based flow-aware packet processing application.

Its job is:

1. receive packets in bursts
2. parse IPv4 TCP/UDP headers
3. identify the packet's flow by 5-tuple
4. reuse an existing worker assignment or create a new one
5. send the packet to the correct worker through a lock-free ring
6. let the worker apply SPI rules and protocol accounting
7. transmit the packet or drop it
8. age out inactive flows and print runtime statistics

The current implementation is already a valid performance-oriented design:

- lock-free DPDK hash table
- RCU QSBR for safe deletion
- bulk flow lookup
- hot/cold flow state split
- one ring per worker
- one TX queue per worker
- cached SPI decision per flow
- double-buffered rule-table hot reload

It is not a perfect final product. It is a strong mini-project with clear room
for further improvement.

## 2. High-Level Architecture

### 2.1 Runtime threads

At runtime, the application uses:

- 1 dispatcher on the main lcore
- 4 workers
- 1 stats/aging thread

So the pipeline is:

`RX port -> dispatcher -> worker ring -> worker -> TX/free`

Important implementation truth:

- there is no dedicated TX thread
- workers transmit directly with `rte_eth_tx_burst()`

### 2.2 File/module structure

The current structure is:

- `src/main.c`
  - very small entry point
  - installs signal handlers
  - calls init, run, cleanup
- `src/app_init.c`
  - bootstrap logic
  - EAL init
  - port setup
  - mempool creation
  - ring creation
  - worker/stats thread launch
- `src/dispatcher.c`
  - RX burst handling
  - packet parsing
  - flow lookup/creation
  - worker dispatch
- `src/worker.c`
  - SPI rule enforcement
  - protocol counters
  - TX burst
- `src/flow_table.c`
  - hash table creation
  - side-array allocation
  - RCU registration helpers
  - aging
- `src/spi_engine.c`
  - rule loading
  - rule matching
  - hot reload
- `src/stats.c`
  - periodic stats
  - periodic aging trigger
  - RCU deferred reclaim

This is a reasonable architecture for a mini-project because:

- each module has one main responsibility
- the hot path is not spread across too many layers
- `main.c` is no longer overloaded with initialization details

## 3. Important Structs and Why They Exist

## 3.1 `struct ipv4_5tuple_key`

Defined in `include/flow_table.h`.

Fields:

- `ip_src`
- `ip_dst`
- `port_src`
- `port_dst`
- `proto`
- `pad[3]`

Use:

- this is the flow key inserted into `rte_hash`
- it identifies a flow uniquely in the current app

Why it is needed:

- same-flow packets must map to the same hash entry
- the dispatcher needs a compact lookup key for very fast repeated queries

Optimization used:

- the key is padded to 16 bytes
- that is better for DPDK CRC-based hashing than a 13-byte irregular layout

## 3.2 `struct flow_hot_data`

Defined in `include/flow_table.h`.

Fields:

- `last_seen`
- `flow_gen`
- `action_version`
- `worker_id`
- `spi_action`

Use:

- this is the per-flow state needed most often during packet processing

Why it is needed:

- `worker_id` tells the dispatcher where to send the packet
- `last_seen` supports aging
- `flow_gen` detects stale metadata reuse
- `action_version` and `spi_action` cache the SPI result for the current rule version

Why hot/cold split exists:

- the dispatcher does not need full flow metadata on every packet
- separating hot state reduces unnecessary memory traffic

## 3.3 `struct flow_cold_data`

Defined in `include/flow_table.h`.

Fields:

- `src_ip`
- `dst_ip`
- `src_port`
- `dst_port`
- `protocol`
- `create_time`

Use:

- stores flow information used less frequently than hot state

Why it is needed:

- SPI matching needs these fields
- fallback worker re-parse logic can rebuild equivalent data
- `create_time` is useful for lifecycle tracking and debugging

## 3.4 `struct flow_table_ctx`

Defined in `include/flow_table.h`.

Fields:

- `hash`
- `qsv`
- `storage_entries`
- `hot`
- `cold`
- `current_chunk`

Use:

- this is the global flow-table state container

Why it is needed:

- keeps flow-table ownership in one place
- avoids many unrelated globals
- makes initialization and cleanup simpler

## 3.5 `struct spi_rule`

Defined in `include/spi_engine.h`.

Fields:

- rule name
- match values for protocol/IP/ports
- match flags for wildcard handling
- action

Use:

- this is one policy rule for shallow packet inspection

Why it is needed:

- policy is data-driven from `rules.cfg`
- wildcard matching must be represented explicitly

## 3.6 `struct lcore_stats`

Defined in `include/stats.h`.

This stores:

- RX/TX packet counts
- RX/TX bytes
- TX drop count
- flow create/delete counts
- SPI forwarded/drop counts
- SPI revalidation count
- protocol counters

Why it is needed:

- each lcore updates its own counters
- the stats thread aggregates them later

Optimization used:

- `__rte_cache_aligned`
- this reduces false sharing between lcores updating different stats blocks

## 3.7 `struct worker_args`

Defined in `include/app_threads.h`.

Use:

- passes `worker_id` into each worker thread

Why it is needed:

- each worker must know which ring it consumes and which TX queue it uses

## 4. Important Functions and What They Do

## 4.1 Entry and bootstrap

### `main()`

File: `src/main.c`

Use:

- minimal program entry point

What it does:

1. install signal handlers
2. initialize app
3. run dispatcher loop
4. cleanup

Why this refactor is better:

- `main.c` now describes the app lifecycle clearly
- bootstrap details moved out into `app_init.c`

### `app_install_signal_handlers()`

File: `src/app_init.c`

Use:

- installs handlers for `SIGINT`, `SIGTERM`, `SIGUSR1`

Why it is needed:

- `SIGINT` and `SIGTERM` stop the app cleanly
- `SIGUSR1` triggers SPI hot reload

### `app_init()`

File: `src/app_init.c`

Use:

- performs application setup

What it does:

1. `rte_eal_init()`
2. validate ports and lcores
3. create mbuf pool
4. initialize ports
5. initialize flow table
6. initialize SPI engine
7. create rings
8. launch stats and workers

### `app_run()`

File: `src/app_init.c`

Use:

- runs the dispatcher on the main lcore

### `app_cleanup()`

File: `src/app_init.c`

Use:

- waits for remote threads
- frees rings
- destroys flow table and SPI engine

## 4.2 Dispatcher

### `dispatcher_thread()`

File: `src/dispatcher.c`

This is the RX-side hot path.

Main jobs:

1. receive a burst from `PORT_IN`
2. parse only supported packets
3. build 5-tuple keys
4. bulk lookup flows
5. create new flows if needed
6. update `last_seen`
7. attach flow metadata to mbuf
8. enqueue mbufs to worker rings
9. report RCU quiescent state

Why it is needed:

- it centralizes flow ownership decisions
- it enforces flow affinity

## 4.3 Worker

### `worker_thread()`

File: `src/worker.c`

Main jobs:

1. dequeue packets from its ring
2. recover flow metadata from mbuf
3. validate `flow_gen`
4. evaluate SPI action
5. update protocol counters
6. drop or transmit

Why it is needed:

- each worker processes only its assigned flows
- this reduces inter-core contention

## 4.4 Flow table

### `flow_table_init()`

File: `src/flow_table.c`

Use:

- create the hash table
- allocate hot/cold arrays
- create and attach RCU QSBR state

### `flow_table_rcu_register()`

Use:

- register a thread as an RCU reader

Needed because:

- dispatcher and stats/aging touch the hash table concurrently

### `flow_table_rcu_quiescent()`

Use:

- report that the calling thread is not holding old references anymore

### `flow_table_aging_tick()`

Use:

- run one periodic aging step

What it does:

1. iterate the hash
2. decide which entries are expired
3. batch their keys
4. delete them from the hash
5. move to the next logical chunk

## 4.5 SPI

### `spi_rule_engine_init()`

Use:

- load initial rule file

### `spi_rule_engine_eval()`

Use:

- fast-path evaluation for an existing flow entry

What it does:

- if cached rule version is still valid, reuse cached `spi_action`
- otherwise re-match against current rule table and refresh cache

### `spi_rule_engine_match_cold()`

Use:

- evaluate a temporary or reparsed flow description

### `spi_rule_engine_reload_if_needed()`

Use:

- called by the stats thread
- replaces the active rule table when a reload was requested

## 4.6 Stats

### `stats_thread()`

File: `src/stats.c`

Use:

- periodic observability and housekeeping

Main jobs:

1. sleep for `AGING_INTERVAL_US`
2. process pending SPI reload
3. run one aging tick
4. reclaim deferred RCU entries
5. aggregate per-lcore counters
6. print stats every full cycle

## 5. Packet Data Path

This is the exact packet journey.

## 5.1 RX

The dispatcher calls `rte_eth_rx_burst(PORT_IN, 0, ...)`.

Why burst RX is used:

- fewer function calls
- lower per-packet overhead
- better cache locality for loop-local arrays

## 5.2 Parse

For each packet, dispatcher:

1. checks Ethernet type is IPv4
2. checks L4 protocol is TCP or UDP
3. validates enough bytes exist for headers
4. extracts source/destination IP and ports

What gets dropped here:

- non-IPv4 packets
- non-TCP/UDP IPv4 packets
- malformed or truncated packets

This is why ICMP and ARP never reach workers in the current code.

## 5.3 Flow lookup

Valid packets are turned into `struct ipv4_5tuple_key`.

Then dispatcher calls:

- `rte_hash_lookup_bulk()`

This is important:

- bulk lookup is faster than repeated single-key lookup
- it keeps the hot path vector-friendly

## 5.4 Existing-flow hit

If the flow exists:

1. read `worker_id`
2. update `last_seen`
3. write `flow_idx` and `flow_gen` into mbuf metadata
4. stage packet into that worker's burst buffer

## 5.5 New-flow miss

If the flow does not exist:

1. add key into hash with `rte_hash_add_key()`
2. initialize `cold[flow_idx]`
3. choose worker by hash value modulo `NUM_WORKERS`
4. initialize hot state
5. store metadata in mbuf
6. enqueue to worker

## 5.6 Ring handoff

The dispatcher groups packets by worker and uses:

- `rte_ring_enqueue_burst()`

Only mbuf pointers move between threads.

This is important for defense:

- dispatcher does not copy packet payload
- worker receives the same mbuf pointer
- this is a zero-copy handoff between dispatcher and worker

More precise wording:

- it is zero-copy at the software handoff layer inside the app
- it is not claiming magical zero-copy all the way through hardware

## 5.7 Worker-side SPI and TX

Worker dequeues mbufs, checks flow metadata, applies SPI, updates stats, and:

- forwards via `rte_eth_tx_burst()`
- or frees the mbuf

## 6. What the Flow Table Is

The flow table is not just one data structure.

It is a combined design:

1. a DPDK hash table for key lookup
2. a hot side-array for fast-path flow state
3. a cold side-array for less frequently used flow state

So the logical model is:

`5-tuple key -> hash position/key id -> hot[position] + cold[position]`

### Why this design is good

- hash gives fast key lookup
- side arrays give compact indexed access
- hot/cold split avoids one giant per-flow struct
- RCU allows concurrent lookup and delete

### What algorithm is used for flow assignment

The worker assignment is:

- `worker_id = rte_hash_hash(key) % NUM_WORKERS`

Why:

- deterministic assignment
- same flow maps to same worker
- keeps packet order inside a flow

This is flow affinity.

## 7. Concurrency Model and RCU

## 7.1 What is concurrent

These components run at the same time:

- dispatcher
- four workers
- stats/aging thread

The app is concurrent by design.

## 7.2 Where synchronization comes from

Synchronization is not done with one big lock.

Instead, the design uses:

- DPDK lock-free hash internals
- RCU QSBR
- SP/SC rings
- per-worker ownership
- per-lcore stats ownership

## 7.3 How RCU works here

RCU in this app protects the hash-table deletion lifecycle.

Basic idea:

1. dispatcher and stats thread are registered as readers
2. they periodically report quiescent state
3. aging deletes a key from the hash
4. actual internal reclamation is deferred
5. reclaim happens only after readers are known to be safe

Why this matters:

- without it, a reader could still observe a hash entry while another thread is deleting it

## 7.4 What RCU does not protect

RCU does not automatically make all side-array fields safe forever.

That is why this app also uses:

- `flow_gen`
- stale-metadata fallback in the worker

Meaning:

- if a flow slot is deleted and later reused
- an old packet can be detected because the generation number changed

This is a very important defense point:

- RCU protects the hash entry lifetime
- `flow_gen` protects slot reuse correctness
- they solve different problems

## 7.5 Why many fields do not use atomics

### Cases where atomics are not needed

- per-worker stats: each lcore mostly writes only its own struct
- ring operations: DPDK ring handles synchronization internally
- hash operations: DPDK hash lock-free mode handles synchronization internally
- worker-owned SPI cache after flow assignment: one worker owns that flow's SPI cache behavior

### Cases where atomics are used

In the SPI engine:

- active rule-table pointer uses atomic load/store
- reload flag uses atomic exchange

Why:

- one thread can publish a new active rule table
- workers can read it without a heavy lock

### Important honesty point

Some shared scalar fields are currently plain loads/stores, for example:

- `last_seen`
- cached SPI action/version fields

On x86-64 DPDK environments, this is commonly treated as acceptable for this
kind of performance-oriented code when access patterns are controlled.

Strict language-lawyer answer:

- this is not the same as a fully portable C11-atomic proof

Practical defense answer:

- the design reduces contention by ownership and by one-writer patterns
- the correctness-critical shared structures are protected by DPDK primitives
- if stricter cross-platform memory semantics were required, selected fields could be promoted to atomics

## 7.6 Is the app concurrent-safe

Short answer:

- for the intended DPDK/x86-64 environment, yes, broadly
- for strict formal portability, there are still a few fields that could be tightened

What is solid:

- hash lifetime management
- ring handoff
- one TX queue per worker
- worker-local counters
- hot reload publication model

What is "good enough" rather than perfect:

- plain shared-field accesses such as `last_seen`
- stats aggregation reading counters while writers are updating them

For a mini-project, this is acceptable and understandable.

## 8. How Flow Aging Works

Flow aging is driven by `stats_thread()`.

Every `AGING_INTERVAL_US`:

1. stats thread wakes up
2. calls `flow_table_aging_tick()`
3. expired keys are deleted
4. RCU deferred reclaim is drained

Expiration condition:

- if `now - last_seen > FLOW_TIMEOUT_SEC * rte_get_tsc_hz()`
- then the flow is expired

Why `last_seen` is enough:

- dispatcher updates it on every flow hit and on creation
- if packets stop arriving, the value stops moving

### Chunking

The code uses a logical chunk index:

- `position % AGING_NUM_CHUNKS == current_chunk`

Intended benefit:

- distribute work over time instead of deleting everything in one large pass

Important truth about the current implementation:

- it still iterates the full hash each tick
- then filters entries by chunk

So the current design is:

- partially optimized
- better than one giant delete batch
- but not a full "scan only one physical chunk" implementation

This is one of the clearest remaining optimization opportunities.

## 9. How SPI Rule Processing Works

SPI here means shallow packet inspection, not payload DPI.

Matching fields:

- protocol
- source IP
- destination IP
- source port
- destination port

Actions supported by parser:

- `FORWARD`
- `DROP`
- `LOG`
- `COUNT`

Current implementation behavior:

- `LOG` and `COUNT` are normalized to `FORWARD`

Why:

- synchronous per-packet logging on the worker hot path would be too expensive

### Match algorithm

Current rule matching is:

- linear scan
- first match wins

Why it is acceptable now:

- the project has a small rule count
- simplicity is good for correctness and defense

Limitation:

- this will not scale well to very large rule sets

## 10. How Hot Reload Works

Hot reload uses double-buffered rule tables.

Data model:

- `g_rule_tables[2]`
- one active
- one inactive

Reload sequence:

1. `SIGUSR1` arrives
2. signal handler marks reload requested
3. stats thread notices reload request
4. inactive table is loaded from file
5. inactive version becomes old version + 1
6. active pointer is atomically swapped

Why this is a good optimization:

- workers do not need a big reload lock
- current readers keep using a stable rule table pointer
- publication is cheap

Why flow-level SPI cache still works:

- each flow stores `action_version`
- if worker sees cache version mismatch, it recomputes action lazily

This is another good defense point:

- reload cost is amortized over later packets
- not forced immediately on every flow in the system

## 11. Monitoring and Statistics

Monitoring is done by `stats_thread()`.

It prints:

- RX throughput
- TX throughput
- active flows
- created/deleted flows
- flow create/delete rate
- SPI drops
- TX drops
- rule count and version
- protocol counters
- per-worker breakdown

Why stats are useful:

- prove the system is alive
- prove rule behavior
- prove flow creation/reuse/aging behavior
- support testing without a hardware sniffer

Why console stats are enough for this project:

- simple
- visible during demo
- easy to parse in functional tests

## 12. Optimization Techniques Used

This section is intentionally detailed.

## 12.1 Major optimizations

### DPDK EAL and poll-mode design

- avoids kernel network stack overhead
- avoids interrupt-driven packet processing

### Burst RX/TX

- `rte_eth_rx_burst()`
- `rte_eth_tx_burst()`

Benefit:

- lower per-packet API overhead

### Lock-free hash

- `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`

Benefit:

- readers and writers avoid coarse locking

### RCU QSBR

Benefit:

- safe delete/reclaim with low reader overhead

### Bulk lookup

- `rte_hash_lookup_bulk()`

Benefit:

- faster than many small lookups

### Hot/cold split

Benefit:

- dispatcher touches less data on the steady-state path

### Per-worker SP/SC ring

Benefit:

- cheap producer/consumer synchronization

### One TX queue per worker

Benefit:

- avoids worker TX queue contention

### Cached SPI result per flow

Benefit:

- repeated packets of the same flow do not re-scan the rule list every time

### Double-buffered hot reload

Benefit:

- no global pause for rule publication

## 12.2 Smaller optimizations

### 16-byte key padding

- better fit for CRC-based hash path

### Side arrays indexed by hash position

- avoids pointer-heavy per-flow heap objects

### Per-lcore cache-aligned stats

- reduces false sharing

### Mbuf metadata reuse

- dispatcher writes `flow_idx` and `flow_gen` into mbuf metadata fields
- worker can use them directly

Benefit:

- avoids extra lookup on the worker fast path

### Worker-local protocol accounting

- no shared protocol-counter lock

### Batched ring enqueue

- packets are grouped per worker before enqueue

Benefit:

- fewer ring API calls

### Batched aging deletion

- expired keys collected first, then deleted

Benefit:

- lower control overhead than immediate delete-per-entry

### NUMA-local allocation intent

- `rte_zmalloc_socket(...)`

Benefit:

- flow-table memory is placed near the processing socket when possible

## 13. Ownership, Freeing, and Double-Free Safety

This is an important defense topic.

### Packet ownership model

At any time, an mbuf should have one logical owner:

- dispatcher while parsing/dispatching
- worker after successful enqueue
- TX path after successful transmit

### Where frees happen

Dispatcher frees:

- unsupported packets
- malformed packets
- packets that could not get a flow entry
- packets that could not be enqueued because ring space was exhausted

Worker frees:

- SPI-dropped packets
- packets that fail stale fallback parse
- packets not accepted by TX burst

### Why double free is avoided

The code avoids double free by ownership transfer:

- if enqueue succeeds, dispatcher stops touching that mbuf
- if worker decides to drop, it frees it exactly once
- if TX accepts the packet, worker does not free it
- if TX rejects the tail of the burst, worker frees only the unsent suffix

So:

- there is no intentional double-free path in the current design

## 14. Is the Architecture Good

Yes, for a mini-project, the architecture is good.

Why:

- clean flow-affinity model
- reasonable concurrency design
- useful performance-oriented choices
- testing can prove real behavior
- logic is modular enough to explain

It is not overengineered in a bad way.

The most "AI-like" part of the repo is the amount of explanation text in some
docs, not the core datapath structure.

## 15. Room for More Optimization

There is still meaningful room for improvement.

Highest-value improvements:

1. make aging truly chunk-local instead of full-hash iteration every tick
2. add a clearer port-count check for the two-port design
3. expose machine-readable stats
4. improve rule matcher if rule count grows
5. consider prefetch on dispatcher hot path after measuring
6. add stricter atomics only where they truly matter

Bigger future improvements:

- multi-RX-queue receive scaling
- RSS-aware ingress
- IPv6 support
- fragmented packet handling
- per-rule hit counters
- CLI or control socket
- JSON metrics export

## 16. What To Do With 2 Days Left

My recommendation:

- do not add more major features
- do not redesign the datapath
- do not chase a large benchmark campaign unless you already have a stable script

Instead, finish with:

1. the small refactor already done
2. the working functional test suite
3. one or two simple benchmark/demo runs
4. a strong understanding of the code path and design choices
5. clean report and slides

If you only have 2 days, defense quality matters more than one more feature.

Best use of time now:

- learn this document well
- run the app yourself a few times
- understand each printed counter
- be ready to explain tradeoffs honestly

## 17. Short Mentor-Defense Version

If your mentor asks for a short summary, you can say:

`flowcore` is a DPDK flow-aware dispatcher. It uses a lock-free `rte_hash`
with RCU QSBR to map packet 5-tuples to worker cores. The dispatcher receives
bursts, parses IPv4 TCP/UDP packets, does bulk flow lookup, preserves flow
affinity, and hands mbuf pointers to worker-specific SP/SC rings without copying
packet payload. Workers apply cached SPI rules, update per-core statistics, and
transmit on dedicated TX queues. A stats thread performs periodic flow aging,
drains deferred RCU reclamation, processes rule hot reload, and prints
throughput and flow statistics. The design is performance-oriented, modular,
and good for a mini-project, with the main remaining optimization opportunity
being the aging scan.`
