# Flowcore Source Code Defense Guide

This document explains the current implementation of `flowcore` from the source code itself. It is meant for project defense, code review, and further optimization work.

It focuses on:

- the real runtime architecture
- how flows are stored and managed
- how packets move through the system
- how RCU, aging, SPI, stats, and hot reload work
- what optimizations are already implemented
- what limitations still exist
- what optimizations should be considered next

Relevant source files:

- `include/common.h`
- `include/flow_table.h`
- `include/spi_engine.h`
- `include/stats.h`
- `src/main.c`
- `src/flow_table.c`
- `src/dispatcher.c`
- `src/worker.c`
- `src/spi_engine.c`
- `src/stats.c`

## 1. What the system is, concretely

At runtime, the application is a DPDK packet pipeline with:

- 1 main lcore running the dispatcher
- 4 worker lcores processing packets
- 1 stats/aging lcore
- 1 shared flow table
- 4 single-producer/single-consumer rings, one per worker

The actual packet path is:

`RX port -> dispatcher -> worker ring -> worker -> SPI decision -> TX port or free`

Important implementation detail:

- there is no dedicated TX thread in the current code
- workers transmit packets directly with `rte_eth_tx_burst()`

That differs from some higher-level architecture notes that describe a separate TX thread.

## 2. Runtime topology and thread roles

### 2.1 Lcore usage

From `src/main.c`:

- the stats/aging thread is launched on the first worker lcore returned by `rte_get_next_lcore(-1, 1, 0)`
- the remaining worker lcores run `worker_thread()`
- the main lcore runs `dispatcher_thread()`

So with `-l 0-5`, the effective mapping is typically:

- lcore 0: dispatcher
- lcore 1: stats + aging
- lcore 2-5: workers 0-3

This mapping depends on the EAL lcore mask, but the role assignment logic is fixed.

### 2.2 Ports and queues

From `src/main.c`:

- `PORT_IN = 0`
- `PORT_OUT = 1`
- input port gets `1` RX queue
- output port gets `NUM_WORKERS` TX queues

The design is:

- dispatcher receives from `PORT_IN` queue `0`
- each worker transmits on `PORT_OUT` queue equal to its worker ID

That avoids TX queue contention between workers.

### 2.3 Rings

Per-worker rings are created in `main.c` with:

- size `4096`
- flags `RING_F_SP_ENQ | RING_F_SC_DEQ`

That means:

- single producer: only the dispatcher enqueues
- single consumer: only the matching worker dequeues

This is an important optimization because the ring can use a cheaper synchronization path than a generic MP/MC ring.

## 3. Global configuration

From `include/common.h`:

- `NUM_WORKERS = 4`
- `HASH_ENTRIES = 1024 * 1024`
- `BURST_SIZE = 32`
- `RING_SIZE = 4096`
- `FLOW_TIMEOUT_SEC = 1`
- `AGING_NUM_CHUNKS = 8`
- `AGING_BATCH_SIZE = 1024`
- `SPI_MAX_RULES = 256`

Two points matter for defense:

1. The current timeout is `1` second, not `5` seconds.
2. The current implementation processes only IPv4 + TCP/UDP packets.

## 4. Actual initialization sequence

Initialization order in `src/main.c`:

1. install signal handlers for `SIGINT`, `SIGTERM`, `SIGUSR1`
2. call `rte_eal_init()`
3. verify port count and lcore count
4. create mbuf pool
5. initialize ports
6. initialize flow table
7. initialize SPI engine
8. create worker rings
9. launch stats/aging thread
10. launch worker threads
11. run dispatcher on the main lcore

Cleanup order:

1. wait for remote lcores to exit
2. free worker rings
3. destroy flow table
4. destroy SPI engine

## 5. Main data structures

## 5.1 `struct ipv4_5tuple_key`

Defined in `include/flow_table.h`:

```c
struct ipv4_5tuple_key {
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t port_src;
    uint16_t port_dst;
    uint8_t  proto;
    uint8_t  pad[3];
} __attribute__((__packed__));
```

Purpose:

- this is the key stored in `rte_hash`
- it uniquely identifies a flow by 5-tuple

Why the padding exists:

- the struct is forced to 16 bytes
- a 16-byte key is convenient for DPDK hash operations and CRC-based hashing

Actual size verified from the compiled layout:

- `sizeof(struct ipv4_5tuple_key) = 16`

## 5.2 `struct flow_hot_data`

Defined in `include/flow_table.h`:

```c
struct flow_hot_data {
    uint64_t last_seen;
    uint32_t flow_gen;
    uint32_t action_version;
    uint8_t  worker_id;
    uint8_t  spi_action;
    uint8_t  pad[2];
};
```

Purpose:

- stores fields that are touched on the steady-state fast path

Why these fields are here:

- `last_seen`: updated on every hit in the dispatcher
- `flow_gen`: detects stale packet metadata when a hash slot is reused
- `action_version`: tells whether cached SPI action matches the current rule table version
- `worker_id`: the assigned worker for this flow
- `spi_action`: cached SPI decision for this flow

Actual size and offsets:

- `sizeof(struct flow_hot_data) = 24`
- `last_seen` at offset `0`
- `flow_gen` at offset `8`
- `action_version` at offset `12`
- `worker_id` at offset `16`
- `spi_action` at offset `17`

Why this split exists:

- the dispatcher hot path needs only `worker_id` and `last_seen`
- the worker fast path needs cached `spi_action` and `action_version`
- placing these fields in a compact array improves density and reduces needless cold-field traffic

## 5.3 `struct flow_cold_data`

Defined in `include/flow_table.h`:

```c
struct flow_cold_data {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  pad[3];
    uint64_t create_time;
};
```

Purpose:

- stores flow attributes that are not needed on every packet in the dispatcher

Why these fields are here:

- SPI matching needs source/destination IPs, ports, and protocol
- `create_time` records when the flow was created

Actual size and offsets:

- `sizeof(struct flow_cold_data) = 24`
- `src_ip` at offset `0`
- `dst_ip` at offset `4`
- `src_port` at offset `8`
- `dst_port` at offset `10`
- `protocol` at offset `12`
- `create_time` at offset `16`

Important design point:

- the cold array duplicates information that also exists in the hash key
- this duplication is intentional
- it avoids reparsing the packet or asking the hash for key bytes during worker SPI slow path

## 5.4 `struct flow_table_ctx`

Defined in `include/flow_table.h`:

```c
struct flow_table_ctx {
    struct rte_hash      *hash;
    struct rte_rcu_qsbr  *qsv;
    uint32_t              storage_entries;
    struct flow_hot_data  *hot;
    struct flow_cold_data *cold;
    uint32_t              current_chunk;
};
```

This is the central flow-table object.

It owns:

- the DPDK hash table
- the QSBR RCU state
- the hot array
- the cold array
- the aging cursor

Most important field:

- `storage_entries` is the real capacity of `hot[]` and `cold[]`

This must be used instead of assuming `HASH_ENTRIES` because DPDK hash key IDs are not guaranteed to fit a naive `0..HASH_ENTRIES-1` model in all configurations.

## 5.5 `struct spi_rule`

Defined in `include/spi_engine.h`:

```c
struct spi_rule {
    char name[32];
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t protocol;
    uint8_t action;
    uint8_t match_src_ip;
    uint8_t match_dst_ip;
    uint8_t match_src_port;
    uint8_t match_dst_port;
    uint8_t match_protocol;
};
```

Purpose:

- one parsed rule from `rules.cfg`

Matching model:

- wildcard matching is implemented with boolean `match_*` flags
- if a flag is `0`, that field is treated as `ANY`

Actual size:

- `sizeof(struct spi_rule) = 52`

## 5.6 `struct lcore_stats`

Defined in `include/stats.h`.

Fields include:

- RX/TX packets and bytes
- TX drops
- created/deleted flows
- SPI forwarded/dropped packets
- SPI revalidation counter
- protocol counters: HTTP, HTTPS, DNS, TCP, UDP, OTHER

Actual layout:

- `sizeof(struct lcore_stats) = 128`
- alignment = `64`

Why alignment matters:

- each lcore mostly updates only its own stats entry
- `__rte_cache_aligned` keeps different lcores from fighting over one cache line
- this avoids false sharing

## 6. How the flow table is managed

## 6.1 Storage model

The flow table is not a single array of full entries.

It is a combination of:

- a DPDK `rte_hash` used for key lookup and key-ID allocation
- a `hot[]` array indexed by hash key ID
- a `cold[]` array indexed by the same hash key ID

So the logical model is:

`5-tuple key -> rte_hash lookup -> key ID -> hot[key_id] + cold[key_id]`

This is a hash + side-array design.

It is not:

- a linked list
- a tree
- a per-worker table
- an array scanned linearly for lookup

## 6.2 Allocation

In `src/flow_table.c`:

1. create `rte_hash` with:
   - `entries = HASH_ENTRIES`
   - `key_len = sizeof(struct ipv4_5tuple_key)`
   - `hash_func = rte_hash_crc`
   - `extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`
2. query `rte_hash_max_key_id()`
3. allocate `hot[]` and `cold[]` using that returned range
4. allocate and initialize QSBR state
5. attach QSBR to the hash with `rte_hash_rcu_qsbr_add()`

Important:

- `hot[]` and `cold[]` are allocated with `rte_zmalloc_socket()`
- allocation is cache-line aligned
- allocation is socket-aware

Approximate memory footprint of flow side arrays:

- `hot[]`: `24 * storage_entries`
- `cold[]`: `24 * storage_entries`
- if `storage_entries` is around `1,048,576`, those two arrays alone are about `48 MiB`

That is much smaller than the earlier over-padded layout that burned a full cache line per entry.

## 6.3 Why `rte_hash` is used

`rte_hash` provides:

- fast key lookup
- key insertion
- key deletion
- bulk lookup API
- lock-free read/write concurrency mode
- integration with DPDK QSBR RCU

This is the core enabling structure for high flow counts.

## 6.4 How lookup works

In `src/dispatcher.c`:

1. extract valid IPv4/TCP/UDP packets from the burst
2. build a temporary `struct ipv4_5tuple_key` for each valid packet
3. call `rte_hash_lookup_bulk()`
4. for each result:
   - if hit: use returned key ID to access `hot[]`
   - if miss: add a new key with `rte_hash_add_key()`

Hit path:

- read `hot[flow_idx].worker_id`
- update `hot[flow_idx].last_seen`
- write `flow_idx` and `flow_gen` into packet metadata
- enqueue to the selected worker ring

## 6.5 How flow add works

When `positions[i] < 0`, the dispatcher treats the packet as a new flow.

It does:

1. `flow_idx = rte_hash_add_key(ft->hash, key_ptrs[i])`
2. bounds-check against `ft->storage_entries`
3. fill `cold[flow_idx]` with 5-tuple and `create_time`
4. compute `hash_val = rte_hash_hash(ft->hash, key_ptrs[i])`
5. assign `target_worker = hash_val % NUM_WORKERS`
6. increment `hot[flow_idx].flow_gen`
7. initialize:
   - `worker_id`
   - `spi_action = SPI_ACTION_UNKNOWN`
   - `action_version = 0`
   - `last_seen = current_tsc`
8. increment `flows_created`
9. attach metadata to the mbuf and enqueue it

Important defense point:

- the current worker assignment algorithm is not `src_ip % N`
- it is `hash(5-tuple) % NUM_WORKERS`

This is a meaningful difference from the early design notes.

## 6.6 How flow delete works

Deletion is done by the aging thread, not by workers.

Delete path:

1. iterate a subset of the hash
2. detect expired flows from `last_seen`
3. collect their keys in a temporary batch
4. call `rte_hash_del_key()` for each expired key

What deletion does not do:

- it does not free `hot[]` or `cold[]` memory
- it does not `free()` a per-flow object
- it does not immediately zero the slot

Why:

- the arrays are persistent side storage
- DPDK hash manages key removal and later key-ID reuse
- immediate zeroing of deleted slots is unsafe when readers may still carry old references or metadata

## 6.7 How slot reuse works

When a deleted hash key ID is later reused for a new flow:

- `cold[flow_idx]` is overwritten with the new flow information
- `hot[flow_idx].flow_gen` is incremented
- cached SPI fields are reset

This generation number is critical because packets already queued to workers may still contain the old `flow_idx`.

## 7. Packet metadata carried from dispatcher to worker

The dispatcher writes:

- `m->hash.fdir.lo = flow_idx`
- `m->hash.fdir.hi = flow_gen`

This is a lightweight way to carry per-packet flow metadata without reparsing in the worker.

Why both values are needed:

- `flow_idx` tells the worker which hot/cold slot to use
- `flow_gen` tells the worker whether that slot still corresponds to the same flow

If the generation matches:

- worker can safely use cached `cold[]` and `hot[]`

If the generation mismatches:

- worker must treat the metadata as stale
- it reparses the packet and performs SPI directly from the packet fields

This is the correctness guard against stale queued packets after flow deletion and slot reuse.

## 8. Full packet data flow

## 8.1 Dispatcher path

For each RX burst:

1. call `rte_eth_rx_burst(PORT_IN, 0, ...)`
2. if no packets:
   - call `flow_table_rcu_quiescent()`
   - continue
3. for each packet:
   - count RX stats
   - check EtherType is IPv4
   - parse IPv4 header
   - compute true IPv4 header length with `rte_ipv4_hdr_len()`
   - accept only TCP or UDP
   - validate that enough bytes are present for ports
   - extract 5-tuple
4. run bulk hash lookup
5. for each valid packet:
   - deduplicate same-burst identical keys
   - hit or add the flow
   - update `last_seen`
   - attach `flow_idx` and `flow_gen`
   - stage packet in `worker_buffers[target_worker]`
6. enqueue staged packets to worker rings with `rte_ring_enqueue_burst()`
7. free unsent packets if a ring is full
8. report RCU quiescent state

### Same-burst duplicate handling

The dispatcher keeps:

- `resolved_indices[]`
- `resolved_positions[]`
- `resolved_workers[]`

It linearly scans previously resolved packets in the same burst and reuses the first result for duplicate keys.

Purpose:

- prevent duplicate flow creation when multiple packets of the same new flow appear in one RX burst

This fixed a real correctness issue.

## 8.2 Worker path

For each dequeue burst:

1. dequeue from the worker ring
2. for each packet:
   - read `flow_idx` and `flow_gen` from mbuf metadata
   - read current SPI rule-table version
   - if `flow_idx` is in range and generation matches:
     - use `ft->cold[flow_idx]`
     - call `spi_rule_engine_eval()`
   - else:
     - reparse packet into a temporary `flow_cold_data`
     - call `spi_rule_engine_match_cold()`
   - account protocol counters
   - if SPI says drop:
     - increment SPI drop stats
     - free packet
   - else:
     - increment SPI forwarded stats
     - stage packet for TX
3. transmit staged packets via `rte_eth_tx_burst(PORT_OUT, worker_id, ...)`
4. count TX stats
5. free unsent packets and count TX drops

Important:

- workers never modify the flow table mapping
- flow ownership stays with dispatcher + aging logic

## 9. Why the hot/cold split is cache-friendly

Without splitting, every flow entry would contain:

- lookup-routing data
- timestamps
- SPI cache state
- full 5-tuple
- creation metadata

The dispatcher touches only a small subset of that state per packet.

By splitting:

- `hot[]` contains fast-path state
- `cold[]` contains infrequently used state

Benefits:

- higher flow-entry density in cache
- fewer useless cold-field fetches
- lower LLC and TLB pressure

Actual result:

- both hot and cold entries are `24` bytes each
- the arrays are aligned, but entries themselves are compact

This is a better tradeoff than forcing each flow to consume a full cache line.

## 10. How fields are accessed on the hot path

### Dispatcher hit path

Most frequent fields:

- `hot[flow_idx].worker_id`
- `hot[flow_idx].last_seen`

These are accessed after a hash hit and before ring enqueue.

### Worker fast path

Most frequent fields:

- `hot[flow_idx].flow_gen`
- `hot[flow_idx].action_version`
- `hot[flow_idx].spi_action`
- `cold[flow_idx]` only when metadata is still valid

The worker fast path does not need to parse packet headers again if metadata is valid.

## 11. How RCU works in this code

This code uses DPDK QSBR RCU.

QSBR means:

- quiescent-state-based reclamation

Core idea:

- readers run without heavy locks
- writers can delete from the shared structure
- actual reclamation is delayed until all readers pass through a known safe point

## 11.1 Which threads are RCU readers

Registered readers:

- dispatcher thread
- stats/aging thread

Workers are not registered as hash readers because they do not traverse the hash table directly.

They consume metadata and side arrays, not the hash structure itself.

## 11.2 Reader quiescent points

Dispatcher:

- calls `flow_table_rcu_quiescent()` after every processed burst
- also calls it when RX returns zero packets

This second case is very important:

- if traffic stops and dispatcher never reports quiescent state, deferred deletions could remain pending indefinitely

Stats/aging thread:

- calls `flow_table_rcu_quiescent()` after each aging tick

## 11.3 Why aging thread is also registered

The stats thread both:

- iterates the hash
- deletes from the hash

Because it touches the hash table, it is registered with QSBR too.

## 11.4 What RCU protects here

RCU protects the hash table’s internal key/data lifecycle after deletion.

It does not magically protect arbitrary side-array semantics.

That is why this project also needs:

- `flow_gen` in `hot[]`
- no immediate slot zeroing on delete

RCU and generation checking solve different parts of the safety problem.

## 12. How flow aging works

Aging is implemented in `src/flow_table.c` and driven by `src/stats.c`.

## 12.1 Timing

From `common.h`:

- `FLOW_TIMEOUT_SEC = 1`
- `AGING_NUM_CHUNKS = 8`
- `AGING_INTERVAL_US = 1000000 / 8 = 125000 us`

Meaning:

- every `125 ms`, the aging thread scans one chunk
- after `8` ticks, the whole hash has been covered once

## 12.2 Expiration test

For each candidate entry:

`if (t_now - last_seen) > FLOW_TIMEOUT_SEC * rte_get_tsc_hz()`

then the flow is expired.

`last_seen` is updated by the dispatcher on each packet hit or creation.

## 12.3 Chunked aging

The aging loop iterates the full hash, but only processes entries where:

`position % AGING_NUM_CHUNKS == current_chunk`

This divides the table into logical slices.

Why it exists:

- scanning a 1M-flow table all at once can cause latency spikes
- chunking distributes the work more evenly

## 12.4 Batch deletion

Expired keys are first collected into:

`const void *expired_keys[AGING_BATCH_SIZE]`

Then deleted in a small batch.

Why:

- reduces control-flow overhead
- avoids calling delete immediately for every single expired entry

## 12.5 What happens after delete

After `rte_hash_del_key()` succeeds:

- the hash key is removed
- the key ID may later be reused
- the side-array data stays in place until overwritten by a new flow

The stats thread accumulates deleted flow counts once per full aging cycle.

## 13. How SPI works

SPI here means shallow packet inspection based on 5-tuple rules.

It does not inspect payloads.

## 13.1 Rule format

Default `rules.cfg` uses:

`Rule_Name,Protocol,Src_IP,Dst_IP,Src_Port,Dst_Port,Action`

Supported protocol tokens:

- `TCP`
- `UDP`
- `*`

Supported actions:

- `FORWARD`
- `DROP`
- `LOG`
- `COUNT`

Important implementation detail:

- `LOG` and `COUNT` are normalized to `FORWARD`
- the code intentionally does not perform synchronous per-packet logging on the hot path

## 13.2 Parsing

In `src/spi_engine.c`:

- rules are loaded line-by-line with `fgets()`
- empty lines and comments are skipped
- comma-separated tokens are trimmed
- each field is parsed into a `struct spi_rule`

Wildcard handling:

- each field has a `match_*` boolean
- `*` means `match_* = 0`

## 13.3 Match algorithm

Current algorithm:

- linear scan over the active rule array
- first matching rule wins

This is simple and correct, but it is not scalable for very large rule sets.

## 13.4 Worker fast path SPI

If metadata is valid:

- worker calls `spi_rule_engine_eval(ft, flow_idx)`

That function:

1. loads the active rule table pointer atomically
2. checks whether `hot[flow_idx].action_version` matches current rule version
3. if yes and action is known:
   - return cached `spi_action`
4. else:
   - recompute by matching against `cold[flow_idx]`
   - update cached `spi_action`
   - update `action_version`

This is the lazy revalidation model.

## 13.5 Worker slow path SPI

If metadata is stale:

- worker reparses packet headers into a temporary `flow_cold_data`
- then calls `spi_rule_engine_match_cold()`

This avoids using the wrong cached slot after deletion/reuse.

## 14. How hot reload works

Hot reload uses a double-buffered rule-table design plus a signal-triggered reload request.

## 14.1 Data model

In `src/spi_engine.c`:

- `g_rule_tables[2]`
- `g_active_rules`
- `g_reload_requested`

Only one rule table is active at a time.

## 14.2 Trigger

In `src/main.c`:

- `SIGUSR1` calls `spi_rule_engine_request_reload()`
- that only sets `g_reload_requested = 1`

This is deliberate:

- signal handlers must stay minimal
- they should not parse files or allocate memory

## 14.3 Actual reload execution

In `src/stats.c`, once every aging tick:

- call `spi_rule_engine_reload_if_needed()`

If reload is requested:

1. choose the inactive rule table buffer
2. parse the rules file into that buffer
3. set `inactive->version = active->version + 1`
4. atomically store `g_active_rules = inactive`

This means:

- readers always see a complete rule table
- there is no in-place mutation of the active table

## 14.4 Why cached actions remain correct

Each flow caches:

- `spi_action`
- `action_version`

After reload:

- active rule table version increases
- workers notice cached version mismatch
- only then they recompute the action

This is efficient because:

- no global flow-table walk is needed at reload time
- only active flows touched by subsequent packets are revalidated

## 15. How core stats work

Each lcore has its own `port_stats[lcore_id]`.

Writers:

- dispatcher updates RX and flow-creation stats
- workers update TX, SPI, and protocol stats
- stats thread updates deleted-flow stats

Aggregation:

- once per full aging cycle, the stats thread scans all lcores
- it computes totals and per-second deltas

Printed outputs include:

- RX/TX throughput
- active flow count from `rte_hash_count()`
- created/deleted flow totals
- SPI drops
- TX drops
- active rule count and version
- SPI forwarded count
- SPI revalidation count
- protocol counters
- per-worker PPS, Mbps, drops, and rechecks

## 16. Protocol classification details

Worker classification rules:

- TCP dst port `80` -> HTTP
- TCP dst port `443` -> HTTPS
- UDP dst port `53` -> DNS
- other TCP -> TCP
- other UDP -> UDP
- everything else -> OTHER

Practical note:

- dispatcher drops non-TCP/UDP before workers
- so `OTHER` is usually `0` in the current architecture

Another subtle point:

- protocol counters are updated before applying DROP/FORWARD action
- so a dropped SSH packet still increments the generic TCP count

## 17. Safety and correctness details that matter in defense

## 17.1 Stale queued packets after deletion

Problem:

- dispatcher may enqueue a packet carrying `flow_idx`
- before worker processes it, aging deletes that flow
- then a new flow may reuse the same key ID

Why this does not break correctness now:

- packet also carries `flow_gen`
- slot reuse increments `flow_gen`
- worker checks generation before trusting cached arrays
- mismatch causes reparsing and slow-path SPI

## 17.2 Why deleted slots are not zeroed immediately

Immediate zeroing is unsafe because:

- readers may still rely on the slot identity check
- stale packets may still arrive at workers

Leaving the slot intact until reuse is safer when combined with generation checking.

## 17.3 Ring and TX backpressure behavior

If a ring is full:

- dispatcher frees unsent packets

If TX cannot send all packets:

- worker frees unsent packets
- increments `tx_drop_pkts`

So the system fails by dropping packets rather than blocking.

## 17.4 Packet format limits

Current dispatcher/worker assumptions:

- IPv4 only
- TCP/UDP only
- L4 ports must be present in the first data segment

This is fine for the current software tests and common DPDK traffic generation, but it is a real implementation boundary.

## 18. Implemented optimization techniques

This section names the optimizations explicitly.

### 18.1 Lock-Free Hash Table

Technique:

- `rte_hash` with `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`

How it helps:

- avoids a global lock on lookups/inserts
- allows concurrent hash access with low synchronization cost

### 18.2 Bulk Lookup

Technique:

- `rte_hash_lookup_bulk()`

How it helps:

- amortizes lookup overhead over a burst
- improves cache behavior versus one-by-one lookup

### 18.3 Hot/Cold Data Splitting

Technique:

- split flow state into `hot[]` and `cold[]`

How it helps:

- dispatcher hot path touches fewer bytes
- better cache density

### 18.4 Compact Entry Layout

Technique:

- compact `24-byte` hot and `24-byte` cold entries instead of one cache line per flow

How it helps:

- lower memory footprint
- less TLB and LLC pressure

### 18.5 Cache-Line-Aligned Arrays

Technique:

- `rte_zmalloc_socket(..., RTE_CACHE_LINE_SIZE, ...)`

How it helps:

- improves alignment for array base addresses
- reduces alignment-related penalties

### 18.6 Per-Lcore Cache-Aligned Stats

Technique:

- `struct lcore_stats __rte_cache_aligned`

How it helps:

- avoids false sharing between cores updating counters

### 18.7 Single-Producer / Single-Consumer Rings

Technique:

- `RING_F_SP_ENQ | RING_F_SC_DEQ`

How it helps:

- cheaper ring synchronization path than general MP/MC rings

### 18.8 Worker Affinity Caching

Technique:

- store `worker_id` in `hot[]`

How it helps:

- once a flow is created, no re-balancing computation is needed on future packets

### 18.9 Packet Metadata Shortcut

Technique:

- carry `flow_idx` and `flow_gen` in `mbuf->hash.fdir`

How it helps:

- worker avoids reparsing on the common path

### 18.10 Lazy SPI Action Cache

Technique:

- cache `spi_action` per flow
- invalidate lazily by comparing `action_version`

How it helps:

- avoids rematching rules on every packet
- avoids full-table invalidation on reload

### 18.11 Double-Buffered Rule Reload

Technique:

- two rule tables plus atomic active-pointer swap

How it helps:

- reload is safe
- readers never see a partially updated rule set

### 18.12 Chunked Aging

Technique:

- scan `1 / AGING_NUM_CHUNKS` per tick

How it helps:

- avoids large periodic latency spikes

### 18.13 Batch Delete During Aging

Technique:

- collect expired keys before deletion

How it helps:

- lowers per-entry control overhead

### 18.14 RCU QSBR Reclamation

Technique:

- DPDK hash + QSBR deferred reclamation

How it helps:

- safe lock-light deletion under concurrent access

### 18.15 Same-Burst Duplicate Suppression

Technique:

- burst-local duplicate tracking in dispatcher

How it helps:

- avoids duplicate creation for identical new flows in one burst

### 18.16 IPv4 Header-Length-Aware Parsing

Technique:

- use `rte_ipv4_hdr_len()` instead of assuming `20` bytes

How it helps:

- correctness for IPv4 options
- prevents malformed port extraction

### 18.17 NUMA-Aware Allocation

Technique:

- `rte_zmalloc_socket()` and ring allocation on `rte_socket_id()`

How it helps:

- attempts to keep memory local to the allocating socket

## 19. Current limitations and mismatches from the original project narrative

These are important to understand and explain honestly during defense.

### 19.1 Timeout is 1 second

The current code uses:

- `FLOW_TIMEOUT_SEC = 1`

If your slides or original requirement say `5 seconds`, that is not what the current source does.

### 19.2 Worker assignment uses 5-tuple hash, not source-IP modulo

Current code:

- `target_worker = rte_hash_hash(key) % NUM_WORKERS`

This is arguably better distributed than `src_ip % N`, but it is different.

### 19.3 No dedicated TX thread

Workers transmit directly.

### 19.4 SPI is linear

Rule matching is O(number of rules).

### 19.5 No IPv6 support

Current code is IPv4-only.

### 19.6 No non-TCP/UDP worker processing

Dispatcher frees such packets early.

### 19.7 `create_time` is stored but not yet used for behavior

It is currently informational or reserved for future features.

## 20. Further optimizations to consider

These are the main next steps, ordered by likely impact.

### 20.1 Replace linear SPI with bucketed lookup

Idea:

- first partition rules by protocol and destination port

Benefit:

- cut rule-match cost dramatically for common traffic

### 20.2 Move large SPI rule sets to `rte_acl`

Idea:

- use DPDK ACL for compiled rule classification

Benefit:

- much better scaling than linear scans for larger policies

### 20.3 Reuse hash signature on flow creation

Current code:

- computes `rte_hash_hash()` after inserting the key

Optimization:

- precompute signature once and reuse it for both lookup/add path and worker assignment

Benefit:

- remove redundant hash work

### 20.4 Improve same-burst duplicate detection

Current code:

- linear scan over previously resolved keys in the same burst

Optimization:

- use a tiny burst-local hash or signature table

Benefit:

- avoid O(`burst^2`) worst-case behavior

### 20.5 Add explicit per-socket placement strategy

Idea:

- pin workers by NUMA socket
- create socket-local mempools and rings accordingly

Benefit:

- reduce remote-memory traffic on multi-socket systems

### 20.6 Replace hash-iteration aging with expiry buckets or a timing wheel

Idea:

- index flows by expiry time instead of scanning the hash

Benefit:

- aging cost becomes proportional to expirations, not table size

### 20.7 Add `rte_eth_tx_buffer` or TX batching helper

Benefit:

- may improve TX efficiency and reduce drop bursts

### 20.8 Add a machine-readable stats mode

Idea:

- JSON lines or CSV snapshots instead of console formatting

Benefit:

- lower formatting overhead
- easier regression automation

### 20.9 Prefetching

You asked not to implement it now, but it is still a valid future step.

Targets for future prefetch:

- hash lookup results
- `hot[]` slots before update
- `cold[]` before worker SPI slow-path use

### 20.10 Better multi-segment packet support

Idea:

- use APIs that safely read headers across segmented mbufs

Benefit:

- more robust with real NIC traffic patterns

## 21. Defense summary

If you need to explain the design in a compact way:

1. The flow table is implemented as a DPDK lock-free hash plus two side arrays indexed by hash key ID.
2. The dispatcher owns flow lookup and creation, updates `last_seen`, and sends packets to fixed worker rings.
3. Workers process packets without touching the hash table directly; they use cached metadata and SPI action caching for the common path.
4. Aging deletes expired hash entries in chunks, and QSBR RCU ensures deletion is safe with concurrent readers.
5. Hot reload is implemented with double-buffered rule tables and lazy per-flow action invalidation.
6. The main performance ideas are bulk lookup, hot/cold split, compact entries, SP/SC rings, per-lcore stats, cached SPI decisions, and chunked aging.

## 22. Recommended next reading order

To understand the project line by line, read in this order:

1. `include/common.h`
2. `include/flow_table.h`
3. `include/spi_engine.h`
4. `include/stats.h`
5. `src/main.c`
6. `src/flow_table.c`
7. `src/dispatcher.c`
8. `src/worker.c`
9. `src/spi_engine.c`
10. `src/stats.c`

That order matches the runtime dependency chain and is the easiest way to build a correct mental model.
