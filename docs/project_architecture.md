# DPDK FlowTable: Implemented Architecture

This document details the **actual implementation** of the DPDK-based FlowTable project, focusing on the real codebase structure, structs, functions, system design choices, and optimization techniques.

## 1. Project Structure

The project is structured into `include` for headers and `src` for implementations.

```text
flowtable_dpdk/
├── include/
│   ├── app_init.h     # Bootstrap API for signal/install, init, run, cleanup
│   ├── app_threads.h  # Thread entry declarations and worker arg struct
│   ├── common.h       # System configurations, macros, and shared globals
│   ├── flow_table.h   # Flow table structures, hot/cold data, and RCU APIs
│   └── stats.h        # Per-lcore statistics structure and stats API
├── src/
│   ├── main.c         # Thin entry point: install signals, init app, run, cleanup
│   ├── app_init.c     # EAL/port/mempool/ring bootstrap and thread launch
│   ├── dispatcher.c   # RX thread: packet parsing, hash lookup, and distribution
│   ├── worker.c       # TX thread: dequeuing packets and transmitting
│   ├── flow_table.c   # Lock-free hash initialization, RCU setup, and chunked aging
│   └── stats.c        # Statistics display and periodic aging execution
└── meson.build        # Build configuration
```

## 2. Core Data Structures

### 2.1 The 5-Tuple Key
Used to uniquely identify a network flow.
```c
struct ipv4_5tuple_key {
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t port_src;
    uint16_t port_dst;
    uint8_t  proto;
    uint8_t  pad[3]; // Optimization: Padded to exactly 16 bytes for 2x 8-byte hardware CRC32
} __attribute__((__packed__));
```

### 2.2 Hot and Cold Data Separation (Cache Optimization)
To avoid polluting the CPU cache during the hot path (per-packet processing), flow data is split into Hot and Cold structures. Each is explicitly padded to 64 bytes (`__rte_cache_aligned`) to fit perfectly into a single CPU cache line, preventing False Sharing.

**Hot Data (Accessed on EVERY packet by Dispatcher)**
```c
struct flow_hot_data {
    uint64_t last_seen; // Updated every packet
    uint8_t  worker_id; // Read to route the packet
    uint8_t  pad[7];    // Pad to 16 bytes, rest is implicit cache line padding (64B total)
} __rte_cache_aligned;
```

**Cold Data (Accessed only on creation and by Aging Thread)**
```c
struct flow_cold_data {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  pad[3];
    uint64_t create_time; // Set once at creation
} __rte_cache_aligned;
```

### 2.3 Flow Table Context
Holds the state of the flow table.
```c
struct flow_table_ctx {
    struct rte_hash      *hash;         // Lock-free DPDK hash table
    struct rte_rcu_qsbr  *qsv;          // RCU variable for safe reclamation
    struct flow_hot_data *hot;          // Array of hot data (indexed by hash position)
    struct flow_cold_data *cold;        // Array of cold data
    uint32_t              current_chunk;// Current chunk for chunked aging
};
```

### 2.4 Statistics
```c
struct lcore_stats {
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t flows_created;
    uint64_t flows_deleted;
} __rte_cache_aligned; // Cache aligned per lcore to prevent false sharing
```

## 3. Important Functions

| Component | Function | Description |
| :--- | :--- | :--- |
| **Main** | `ports_init()` | Configures `PORT_IN` for RX and `PORT_OUT` for TX (multi-queue for workers). |
| **Main** | `main()` | Initializes DPDK EAL, Mempools, Rings, Flow Table, and launches lcore threads. |
| **Dispatcher**| `dispatcher_thread()` | Polls NIC, extracts 5-tuple, performs `rte_hash_lookup_bulk`, routes to workers, reports RCU quiescent state. |
| **Worker** | `worker_thread()` | Dequeues from worker-specific ring buffer and transmits via `PORT_OUT`. |
| **Flow Table**| `flow_table_init()` | Creates lock-free hash (`RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`), sets up RCU QSBR. |
| **Flow Table**| `flow_table_aging_tick()`| Scans 1/8th of the flow table to batch-delete flows older than 1 second. |
| **Stats** | `stats_thread()` | Sleeps for 125ms, calls `aging_tick`, aggregates and prints stats every 1 sec. |

## 4. System Design & Optimization Choices

### 4.1 Lock-Free Concurrency (RCU QSBR)
Instead of using slow spinlocks or rwlocks, the system uses DPDK's Lock-Free Hash coupled with **RCU QSBR (Read-Copy-Update Quiescent State-Based Reclamation)**.
*   **Dispatcher (Reader):** Performs lock-free lookups. Calls `flow_table_rcu_quiescent()` after every burst to signal "I am not holding any old references".
*   **Aging (Writer):** Uses `rte_hash_del_key`. The hash table delays actual memory reclamation until all readers have reported a quiescent state.

### 4.2 Chunked Flow Aging
Scanning 1,000,000 entries every second causes massive latency spikes.
*   **Solution:** The hash table is divided logically into `AGING_NUM_CHUNKS` (8). The aging thread runs every 125ms, scanning exactly 1/8 of the table. This spreads the CPU load evenly, ensuring stable latency.
*   **Batching:** Expired keys are collected in `expired_keys[1024]` and deleted in batches.

### 4.3 Data Flow & Affinity
1.  **Ingress:** NIC -> `rte_eth_rx_burst` -> `pkts_burst`.
2.  **Parsing:** Extract 5-tuple. Ensure valid IPv4 TCP/UDP.
3.  **Lookup:** `rte_hash_lookup_bulk` is used for high-performance vectorized lookup.
4.  **Creation (Miss):** If flow doesn't exist, atomically add it. Hash the 5-tuple to assign a `worker_id` (ensuring **Flow Affinity**). Populate Cold and Hot arrays.
5.  **Forwarding (Hit):** Read `worker_id` from Hot data, update `last_seen`.
6.  **Dispatch:** Enqueue packets into Single-Producer/Single-Consumer (SP/SC) `worker_rings`.
7.  **Egress:** Worker dequeues from ring and transmits via `rte_eth_tx_burst`.

### 4.4 Hardware Optimization Alignment
*   **16-byte Key:** The 5-tuple key is exactly 13 bytes. It is padded with 3 bytes to reach 16 bytes. This allows `rte_hash_crc` to execute two hardware-accelerated 8-byte CRC32 instructions perfectly.
*   **NUMA-Aware Allocation:** Hash tables and hot/cold arrays are allocated using `rte_zmalloc_socket` to ensure they reside on the same memory node as the processing CPU cores.
