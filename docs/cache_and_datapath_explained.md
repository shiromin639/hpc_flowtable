# Cache And Datapath Explained

This document explains the current implementation in this repository as it exists in source today. It is intentionally project-specific: it follows the real code path, names the real data structures, and explains what the CPU is likely touching at each step.

It does not claim that a developer can directly "see the cache". You cannot. What you can do is:

1. Identify which memory locations the code touches.
2. Identify which core touches them.
3. Identify whether the access is read-heavy, write-heavy, sequential, or random.
4. Infer whether the access pattern is cache-friendly.
5. Verify the inference with measurement tools such as `perf`.

## 1. What "Fast Path" Means In This Project

In this codebase, the fast path is the common case:

1. RX burst arrives on `PORT_IN`.
2. Dispatcher parses Ethernet + IPv4 + TCP/UDP.
3. Dispatcher extracts the 5-tuple and does `rte_hash_lookup_bulk()`.
4. The flow already exists.
5. Dispatcher reads cached `worker_id`, updates `last_seen`, writes `flow_idx` and `flow_gen` into the mbuf metadata, and enqueues the packet to the chosen worker ring.
6. Worker dequeues the packet.
7. Worker validates `flow_gen`, reuses the flow slot, evaluates cached SPI state, and transmits.

This is "fast" because it avoids:

- creating a new flow
- reparsing the packet in the worker
- scanning the SPI rules from scratch on every packet
- taking a global lock

The slow path is everything rarer and more expensive:

- new flow creation
- stale-slot fallback reparsing in worker
- SPI cache refresh after rule reload
- flow aging deletion
- rule reload
- stats aggregation

Code references:

- [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:4)
- [src/worker.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/worker.c:85)

## 2. Runtime Topology

The runtime topology is set in [src/main.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/main.c:77).

Threads:

- 1 dispatcher on the main lcore
- 4 workers
- 1 stats/aging thread

Ports and queues:

- `PORT_IN`: 1 RX queue
- `PORT_OUT`: 4 TX queues, one per worker

Inter-thread handoff:

- 1 ring per worker
- each ring is `RING_F_SP_ENQ | RING_F_SC_DEQ`
- that means one producer and one consumer only

That is important because it avoids the higher synchronization cost of MPMC queues.

## 3. Main Memory Objects

The main objects that matter to packet processing are:

### 3.1 `rte_mbuf`

Each packet lives in an mbuf from the mempool created in [src/main.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/main.c:193).

During processing, the app touches:

- mbuf header fields such as `pkt_len`
- packet payload header bytes through `rte_pktmbuf_mtod()`
- two metadata fields reused to pass flow information:
  - `m->hash.fdir.lo` = `flow_idx`
  - `m->hash.fdir.hi` = `flow_gen`

This is a useful optimization: worker does not need to recompute the lookup key just to find the flow slot.

### 3.2 Flow hash table

The flow table is not just a hash table and not just an array. It is a two-level design:

1. `rte_hash` maps a 5-tuple key to an integer key ID.
2. Two side arrays hold the actual per-flow state at that key ID.

The hash is created in [src/flow_table.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/flow_table.c:31).

Key points:

- `entries = HASH_ENTRIES`
- `hash_func = rte_hash_crc`
- `extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`

So lookup is hash-based, not linear scan.

### 3.3 Flow key

The lookup key is `struct ipv4_5tuple_key` in [include/flow_table.h](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/include/flow_table.h:18):

- `ip_src`
- `ip_dst`
- `port_src`
- `port_dst`
- `proto`
- `pad[3]`

The pad is intentional. The key is rounded to 16 bytes so the CRC-based hash path is efficient.

### 3.4 Hot flow array

`struct flow_hot_data` in [include/flow_table.h](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/include/flow_table.h:58) contains:

- `last_seen`
- `flow_gen`
- `action_version`
- `worker_id`
- `spi_action`

This is the data that the steady-state path needs most often.

### 3.5 Cold flow array

`struct flow_cold_data` in [include/flow_table.h](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/include/flow_table.h:68) contains:

- `src_ip`
- `dst_ip`
- `src_port`
- `dst_port`
- `protocol`
- `create_time`

This is the data used for:

- initial flow creation
- SPI matching
- stale-slot fallback reparsing

### 3.6 Per-lcore stats

`struct lcore_stats` in [include/stats.h](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/include/stats.h:14) is `__rte_cache_aligned`.

That means each lcore's stats are placed on a separate cache line boundary to reduce false sharing when different cores update their own counters.

This is one of the clearest examples of deliberate cache-aware layout in the project.

## 4. Packet Datapath, Step By Step

### 4.1 Dispatcher RX burst

Dispatcher receives up to `BURST_SIZE=32` packets in [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:51).

Why burst matters:

- fewer API calls per packet
- better amortization of loop overhead
- better temporal locality on stack arrays and packet metadata

Local arrays used by dispatcher:

- `pkts_burst[]`
- `worker_buffers[][]`
- `keys[]`
- `key_ptrs[]`
- `positions[]`
- `resolved_indices[]`
- `resolved_positions[]`
- `resolved_workers[]`

These arrays are small, contiguous, and reused every burst. They are the kind of data most likely to stay hot in L1/L2.

### 4.2 Header parse and 5-tuple extraction

For each packet, dispatcher:

1. reads `pkt_len`
2. reads Ethernet header
3. rejects non-IPv4
4. reads IPv4 header and header length
5. rejects non-TCP/UDP
6. reads ports
7. stores the key into `keys[valid_pkts]`

Code: [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:71)

Likely cache behavior:

- packet headers are likely already hot or at least recently fetched because RX just returned them
- the `keys[]` array is stack-local and written sequentially, which is cache-friendly

### 4.3 Bulk hash lookup

Dispatcher calls `rte_hash_lookup_bulk()` in [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:127).

This is one of the major optimization choices in the project:

- batch lookup instead of one lookup call per packet
- lock-free read mode
- compact 16-byte key

The result is written into `positions[]`.

From source alone, you cannot know which exact hash buckets are in L1, L2, or L3 at that moment. What you do know is:

- hash lookup touches the hash structure internally
- returned `positions[i]` lets your code jump directly into `hot[]` and `cold[]`
- those array accesses are random by `flow_idx`, so they are more cache-sensitive than the stack arrays

### 4.4 Existing flow hit

If `positions[i] >= 0`, dispatcher runs the hit path in [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:192).

It does:

- `target_worker = ft->hot[flow_idx].worker_id`
- `ft->hot[flow_idx].last_seen = current_tsc`

Then it writes:

- `m->hash.fdir.lo = flow_idx`
- `m->hash.fdir.hi = flow_gen`

and stages the packet into `worker_buffers[target_worker]`.

Why this is cache-friendly relative to a monolithic flow struct:

- hit path touches only `hot[flow_idx]`
- it does not pull `src_ip`, `dst_ip`, `ports`, `create_time` into the dispatcher path

That is the entire argument for hot/cold splitting in this project.

### 4.5 New flow miss

If `positions[i] < 0`, dispatcher executes the slower path in [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:156).

It does:

1. `rte_hash_add_key()`
2. fill `cold[flow_idx]`
3. compute `hash_val`
4. pick `worker_id = hash_val % NUM_WORKERS`
5. increment `flow_gen`
6. initialize cached SPI fields
7. set `last_seen`

This path is slower because it performs more writes and more data initialization.

### 4.6 Ring enqueue

Dispatcher batches packets by worker and calls `rte_ring_enqueue_burst()` in [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:220).

This is another optimization that is not primarily about cache, but still helps performance:

- one bulk ring operation per worker instead of one per packet
- SP/SC ring flags reduce synchronization cost

### 4.7 Worker dequeue

Worker dequeues a burst in [src/worker.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/worker.c:100).

Likely cache behavior:

- ring entries are contiguous enough to be friendly to prefetch and cache
- `pkts[]` and `tx_pkts[]` are stack arrays, again friendly to L1/L2

### 4.8 Worker validation and SPI decision

Worker reads:

- `flow_idx`
- `flow_gen`

from the mbuf in [src/worker.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/worker.c:109).

Then:

- if `flow_gen` matches `ft->hot[flow_idx].flow_gen`, worker trusts the cached slot
- otherwise it reparses packet headers into a temporary `parsed_cold`

Code: [src/worker.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/worker.c:115)

If slot is valid:

- worker uses `ft->cold[flow_idx]`
- worker may refresh SPI action if `action_version` is stale

Code: [src/spi_engine.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/spi_engine.c:303)

This means the worker touches both hot and cold flow state on the common path, not just hot state. That is an important current limitation of the hot/cold design: it benefits dispatcher more than it benefits worker.

### 4.9 TX burst

Worker appends forwardable packets into `tx_pkts[]` and transmits in [src/worker.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/worker.c:149).

Again, batching matters:

- lower call overhead
- better TX queue utilization

## 5. What Is Likely In L1, L2, L3, Or DRAM?

You cannot know exact cache contents from source code. You can only make justified guesses.

### 5.1 Strong candidates for L1/L2 residency

These are local, small, and reused quickly:

- dispatcher stack arrays
- worker stack arrays
- loop variables
- recently touched mbuf metadata
- ring producer/consumer metadata
- current burst's packet headers

### 5.2 Data that may fit in cache for small working sets but not large ones

- frequently reused `hot[]` entries for active flows
- frequently reused `cold[]` entries for active flows
- active portions of the hash structure

If the flow working set is small, many repeated packets hit recently touched flow entries and the CPU benefits.

If the flow working set is large, `flow_idx` accesses become more random and more of these touches come from L3 or DRAM.

### 5.3 Data that is more likely to miss cache under large flow counts

- randomly accessed `hot[flow_idx]` entries when the active set is large
- randomly accessed `cold[flow_idx]` entries
- wide scans over the hash during aging

That is why high-flow-cardinality workloads are usually limited by cache and memory behavior even when the algorithmic complexity is nominally O(1).

## 6. How To Reason About Cache In This Project

The correct mental model is not "where is variable X right now?"

The correct questions are:

1. Is this data contiguous or scattered?
2. Is it reused soon on the same core?
3. Is it written by one core or many cores?
4. Does one access drag in unrelated data?
5. Is the access mostly sequential or mostly random?

Examples from this project:

### 6.1 `keys[]` is friendly

- contiguous
- small
- burst-local
- written and read immediately by the same core

### 6.2 `hot[]` is partly friendly

- compact
- contains only fast-path metadata
- indexed directly by `flow_idx`

But:

- accesses are still random by flow index
- random access means cache behavior depends heavily on working set size

### 6.3 `cold[]` is more expensive

- also indexed randomly
- bigger logical working set because it holds tuple fields and create time
- touched by worker for SPI and protocol accounting

### 6.4 `port_stats[]` is intentionally isolated

- each lcore updates mostly its own slot
- `__rte_cache_aligned` reduces false sharing

This is a case where extra padding is a good tradeoff.

## 7. False Sharing In This Project

False sharing happens when two cores write different variables that happen to live in the same cache line.

That causes cache-line ping-pong even though the variables are logically independent.

Good example of avoiding it:

- `struct lcore_stats` is `__rte_cache_aligned`

Potential sharing that still exists by design:

- dispatcher writes `hot[flow_idx].last_seen`
- aging thread later reads the same array

That is real sharing, not false sharing. Both threads actually touch the same object.

The hot/cold split reduces wasted bytes per touch, but it does not eliminate true sharing on `last_seen`.

## 8. How RCU Fits The Datapath

RCU/QSBR is initialized in [src/flow_table.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/flow_table.c:75).

Readers:

- dispatcher
- stats/aging thread during iteration

They register with:

- `flow_table_rcu_register()`

and report completion with:

- `flow_table_rcu_quiescent()`

Dispatcher does this every burst, including idle bursts, in [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:53) and [src/dispatcher.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/dispatcher.c:239).

Aging deletes expired hash keys with `rte_hash_del_key()` in [src/flow_table.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/flow_table.c:192).

Because the hash is in QSBR defer mode:

- delete does not immediately reclaim all internal resources
- reclamation must happen after readers pass quiescent states

The current code drains the defer queue in [src/stats.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/stats.c:81).

RCU is therefore a concurrency optimization:

- readers avoid a global table lock
- deletion becomes safe without stopping readers

It is not primarily a cache optimization, but it changes memory lifetime and synchronization costs.

## 9. How Aging Fits The Datapath

Aging runs in the stats thread every `AGING_INTERVAL_US` in [src/stats.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/stats.c:64).

The current intended idea is:

- split the table into `AGING_NUM_CHUNKS`
- process one chunk per tick
- avoid one big once-per-second spike

Actual code behavior in [src/flow_table.c](/home/kinosaki-mei/dev/vdt_project/flowtable_dpdk/src/flow_table.c:175):

- `rte_hash_iterate()` still walks the entire hash each tick
- code filters by `position % AGING_NUM_CHUNKS`

So the logical work is chunked, but the iteration cost is still close to a full-table walk each tick.

This is one of the most important current optimization limits in the project.

## 10. Is Hot/Cold Split Redundant?

Short answer: no, not redundant, but not fully exploited yet.

Why it is useful:

- dispatcher hit path only needs `worker_id`, `flow_gen`, `spi_action`, `action_version`, `last_seen`
- keeping tuple fields out of that path improves density and reduces useless bytes fetched
- it avoids the old "one giant flow struct touched for every packet" pattern

Why it is not perfect:

- worker still touches `cold[]` on the common path
- protocol accounting uses `cold->protocol` and `cold->dst_port`
- SPI evaluation can require `cold[]`

So the split currently helps dispatcher more than worker.

If you remove hot/cold and merge everything into one struct:

Pros:

- simpler code
- fewer arrays
- fewer pointer/index jumps in the source

Cons:

- dispatcher hit path pulls in more bytes than needed
- lower table density
- larger working set
- more cache waste at large flow counts

My recommendation:

- keep hot/cold
- make it more effective instead of deleting it

The best next refinement is to move a tiny preclassified protocol category into `hot[]`, so worker protocol accounting does not need `cold[]` on every packet.

## 11. Implemented Optimization Techniques In The Current Code

These are real optimizations already present:

### 11.1 Batch RX/TX/ring operations

- `rte_eth_rx_burst()`
- `rte_hash_lookup_bulk()`
- `rte_ring_enqueue_burst()`
- `rte_eth_tx_burst()`

Effect:

- lower fixed overhead per packet

### 11.2 Lock-free hash reads

`RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`

Effect:

- dispatcher reads do not take a global lock

### 11.3 CRC hash on compact 16-byte key

Effect:

- efficient key hashing

### 11.4 Indexed side arrays

Effect:

- packet path uses integer slot IDs after lookup
- avoids storing bulky per-flow state inside the hash structure itself

### 11.5 Hot/cold flow split

Effect:

- dispatcher hit path touches less per-flow data

### 11.6 Per-worker SP/SC rings

Effect:

- cheaper queue synchronization

### 11.7 Cached worker affinity

Effect:

- worker selection is computed once on flow creation, then reused

### 11.8 Cached SPI action with version invalidation

Effect:

- avoids full rule scan on every packet when rules have not changed

### 11.9 RCU/QSBR deletion safety

Effect:

- avoids reader-side locking while keeping delete safe

### 11.10 Per-lcore cache-aligned stats

Effect:

- reduces false sharing on counters

## 12. Further Optimizations Beyond Cache Layout

Not every useful optimization is a cache optimization. The biggest next steps are:

### 12.1 Real chunked aging

Current issue:

- the code iterates the whole hash each tick and filters by modulo

Better direction:

- expiry buckets
- timing wheel
- per-second bins

Expected value:

- high

This is likely the highest-value next optimization.

### 12.2 Avoid cold-data touches on worker common path

Current issue:

- protocol accounting needs `cold->protocol` and `cold->dst_port`

Better direction:

- store a compact protocol class in `hot[]`
- example classes: HTTP, HTTPS, DNS, TCP, UDP, OTHER

Expected value:

- medium to high

### 12.3 Remove unconditional duplicate scan from dispatcher

Current issue:

- same-burst duplicate detection is an extra loop over already resolved keys

Better direction:

- only apply special handling on misses
- or use a tiny burst-local hash

Expected value:

- medium

### 12.4 Better insertion-failure visibility

Current issue:

- when table is full, new-flow add failures are silently freed

Better direction:

- dedicated counters for add failures / table full drops

Expected value:

- not a speed optimization by itself, but very important for correct analysis

### 12.5 Better worker assignment policy for skew

Current issue:

- current assignment is `hash % NUM_WORKERS`

Better direction:

- power-of-two choices on new-flow creation

Expected value:

- low in uniform workloads
- higher in skewed workloads

This is a load-balance optimization, not a cache optimization.

### 12.6 Benchmark mode

Current issue:

- stats, protocol accounting, and feature checks add real overhead

Better direction:

- build or runtime mode that disables expensive accounting when measuring raw datapath

Expected value:

- medium

### 12.7 Prefetch

You explicitly said not to implement prefetching now, but conceptually it remains a possible next step:

- prefetch packet headers ahead in dispatcher
- prefetch likely `hot[flow_idx]` entries after lookup

Expected value:

- workload-dependent
- must be measured carefully

### 12.8 NUMA tightening

Current code already allocates major structures on `rte_socket_id()`, but stronger NUMA pinning is still possible:

- explicit per-port socket matching
- socket-local rings and pools

Expected value:

- moderate if the deployment becomes multi-socket

## 13. How To Prove A Cache Optimization Actually Helped

Source-level reasoning is necessary but not sufficient.

To validate, compare before and after using:

```bash
perf stat -e cycles,instructions,cache-references,cache-misses,LLC-load-misses,branches,branch-misses <command>
```

Useful interpretations:

- lower `cache-misses` with higher PPS: likely a real locality win
- lower `instructions` with similar PPS: simpler hot path
- lower `cycles` per packet: real datapath improvement
- unchanged counters with noisy PPS: likely no meaningful win

## 14. Bottom Line

For the current code:

- hot/cold split is justified
- it is not redundant
- it is currently only partially exploited because worker still touches `cold[]` on the common path

The most important next optimization is not another padding trick. It is reducing expensive background and cross-path work:

1. make aging truly sublinear per tick
2. stop touching `cold[]` on worker common path when avoidable
3. reduce dispatcher duplicate-handling overhead
4. improve visibility into table-full behavior

That is the right next stage for this project: less "more structure", more "less unnecessary work".
