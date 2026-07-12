# FlowCore Current Architecture

Tài liệu này mô tả kiến trúc hiện tại của FlowCore theo source code trong repository. Nó không mô tả lại bản thiết kế cũ nếu bản cũ đã lệch khỏi code.

## 1. Mục tiêu hệ thống

FlowCore là một datapath DPDK có trạng thái. Hệ thống nhận packet, parse IPv4 TCP/UDP, xác định flow bằng 5-tuple, lưu flow vào Flow Table, giữ flow affinity để mọi packet cùng flow đi về cùng worker, áp dụng SPI rule và cập nhật thống kê realtime.

Pipeline hiện tại:

```text
RX port
  -> dispatcher
  -> parse IPv4 TCP/UDP
  -> rte_hash lookup/add
  -> hot/cold flow metadata
  -> worker ring
  -> worker
  -> SPI decision
  -> TX port hoặc drop/free
```

Điểm khác đáng nhớ so với requirement ban đầu:

- Timeout hiện tại là `FLOW_TIMEOUT_SEC = 5`, đúng với yêu cầu gốc.
- Worker assignment dùng hash của toàn bộ 5-tuple modulo `NUM_WORKERS`, không dùng `SourceIP % N`.
- Không có TX thread riêng; worker gọi `rte_eth_tx_burst()` trực tiếp.
- Dispatcher chỉ cho IPv4 TCP/UDP đi tiếp; ARP, ICMP và non-IPv4/non-TCP/UDP bị lọc ở dispatcher.
- Parser hiện đọc header từ segment đầu tiên của mbuf; fragmented/scattered RX không phải phạm vi hỗ trợ chính.

## 2. Project structure

```text
flowtable_dpdk/
├── include/
│   ├── app_init.h
│   ├── app_threads.h
│   ├── common.h
│   ├── flow_packet.h
│   ├── flow_table.h
│   ├── spi_engine.h
│   └── stats.h
├── src/
│   ├── main.c
│   ├── app_init.c
│   ├── dispatcher.c
│   ├── flow_packet.c
│   ├── flow_table.c
│   ├── spi_engine.c
│   ├── stats_core.c
│   ├── stats.c
│   └── worker.c
├── tests/
│   ├── flowcore_unit_tests.c
│   ├── flowcore_functional_tests.c
│   ├── flow_slot_lifecycle_tests.c
│   ├── flow_table_overload_tests.c
│   ├── generate_test_assets.py
│   ├── run_functional_tests.py
│   ├── run_performance_tests.py
│   └── flowcore_test_lib.py
├── scripts/
├── pcap/
├── rules.cfg
└── meson.build
```

## 3. Header roles

### `include/common.h`

Chứa cấu hình compile-time và các object toàn cục:

- `NUM_WORKERS = 4`
- `BURST_SIZE = 32`
- `HASH_ENTRIES = 1024 * 1024`
- `RING_SIZE = 4096`
- `NUM_MBUFS = 32768`
- `FLOW_TIMEOUT_SEC = 5`
- `AGING_NUM_CHUNKS = 8`
- `AGING_BATCH_SIZE = 1024`
- `FLOW_PRESSURE_THRESHOLD_PCT = 92`
- `FLOW_AGGRESSIVE_THRESHOLD_PCT = 96`
- `FLOW_CRITICAL_THRESHOLD_PCT = 99`
- `FLOW_PRESSURE_TARGET_PCT = 92`
- `FLOW_VICTIM_CACHE_SIZE = 8192`
- `PORT_IN = 0`
- `PORT_OUT = 1`
- `SPI_RULES_PATH = "rules.cfg"`
- `SPI_MAX_RULES = 256`

Global externs:

- `force_quit`
- `mbuf_pool`
- `worker_rings[NUM_WORKERS]`
- `worker_lcore_ids[NUM_WORKERS]`

Tradeoff: cấu hình compile-time đơn giản, dễ bảo vệ và dễ test. Đổi lại, số worker chưa dynamic. Nếu hội đồng hỏi dynamic scaling, trả lời là đây là hướng mở rộng cần runtime config, ring allocation động, TX queue config động và policy xử lý flow đang tồn tại khi thay đổi worker count.

### `include/flow_table.h`

Định nghĩa 5-tuple key, hot/cold metadata, flow table context và API RCU/aging/pressure/replacement.

Struct quan trọng:

- `struct ipv4_5tuple_key`
- `struct flow_hot_data`
- `struct flow_cold_data`
- `struct flow_table_ctx`
- `struct flow_aging_result`
- `enum flow_pressure_mode`
- `struct flow_pressure_result`
- `struct flow_victim_candidate`

### `include/flow_packet.h`

Tách packet parsing khỏi dispatcher/worker để unit test dễ hơn. API chính:

- `flow_packet_extract_key()`
- `flow_packet_extract_cold()`
- `flow_cold_from_key()`
- `flow_stats_account_protocol()`

### `include/spi_engine.h`

Định nghĩa SPI actions, rule format và API init/reload/match.

### `include/stats.h`

Định nghĩa `struct lcore_stats` và API per-lcore stats.

## 4. Source module roles

### `src/main.c`

Entry point mỏng:

1. Cài signal handler.
2. Gọi `app_init()`.
3. Chạy dispatcher bằng `app_run()`.
4. Cleanup khi thoát.

Thiết kế này giúp phần bootstrap nằm trong `app_init.c`, còn `main.c` không chứa logic datapath.

### `src/app_init.c`

Chịu trách nhiệm:

- Cài signal handler cho `SIGINT`, `SIGTERM`, `SIGUSR1`.
- Parse env config:
  - `FLOWCORE_NUM_MBUFS`
  - `FLOWCORE_MBUF_DATA_SIZE`
  - `FLOWCORE_RULES_PATH`
- Gọi `rte_eal_init`.
- Khởi tạo stats.
- Kiểm tra port/lcore.
- Tạo mbuf pool.
- Configure/start `PORT_IN` và `PORT_OUT`.
- Khởi tạo Flow Table.
- Khởi tạo SPI Rule Engine.
- Tạo ring cho worker.
- Launch stats/aging thread.
- Launch 4 worker threads.

Runtime lcore requirement:

- 1 dispatcher trên main lcore.
- 1 stats/aging lcore.
- 4 worker lcores.
- Tổng tối thiểu: `NUM_WORKERS + 2 = 6` lcores.

Runtime port requirement:

- Ứng dụng hiện yêu cầu tối thiểu 2 DPDK ports khả dụng vì cố định `PORT_IN = 0` và `PORT_OUT = 1`.
- `ports_init()` hiện fail ngay nếu bất kỳ RX/TX queue setup nào không thành công, thay vì chỉ in warning.

### `src/dispatcher.c`

Dispatcher là RX thread và flow owner trên ingress path. Nó:

1. Nhận packet bằng `rte_eth_rx_burst(PORT_IN, 0, ...)`.
2. Parse IPv4 TCP/UDP sang `ipv4_5tuple_key`.
3. Lọc packet unsupported/truncated.
4. Gọi `rte_hash_lookup_bulk()` cho batch key hợp lệ.
5. Với flow hit:
   - đọc `hot[flow_idx].worker_id`
   - đọc `flow_gen`
   - update `last_seen`
6. Với flow miss:
   - `rte_hash_add_key()`
   - tăng generation
   - fill `cold[]`
   - init `hot[]`
   - chọn worker bằng `rte_hash_hash(key) % NUM_WORKERS`
   - store `last_seen` cuối cùng để publish slot cho aging/pressure
   - nếu add fail, thử stateful replacement bằng victim cache/emergency bounded eviction rồi retry add
7. Ghi metadata vào mbuf:
   - `m->hash.fdir.lo = flow_idx`
   - `m->hash.fdir.hi = flow_gen`
8. Gom packet theo worker.
9. Enqueue burst vào `worker_rings[w]`.
10. Báo RCU quiescent state.

Dispatcher cũng xử lý duplicate miss trong cùng một burst. Nếu hai packet cùng flow đều lookup miss trước khi packet đầu add key, packet sau sẽ reuse kết quả đã resolved trong burst, tránh add trùng.

Replacement hiện tại vẫn là stateful: flow mới chỉ được xử lý theo đường chính khi đã add được vào Flow Table. Nếu table pressure, stats thread sẽ cố giữ reserve trước; dispatcher chỉ evict ở cold path của flow mới, không scan nặng trên packet hit path.

### `src/worker.c`

Worker là packet processor và egress owner. Nó:

1. Dequeue burst từ ring của worker.
2. Lấy `flow_idx` và `flow_gen` từ mbuf metadata.
3. Nếu generation khớp:
   - copy `cold[flow_idx]` sang snapshot cục bộ
   - kiểm tra lại `flow_gen` sau khi copy
   - gọi `spi_rule_engine_match_cold()` trên snapshot
4. Nếu generation không khớp hoặc index invalid:
   - parse lại packet sang `flow_cold_data` tạm thời
   - gọi `spi_rule_engine_match_cold()`
5. Cập nhật protocol counters.
6. Nếu SPI action là DROP:
   - tăng drop counter
   - free mbuf
7. Nếu FORWARD:
   - gom vào `tx_pkts[]`
   - gọi `rte_eth_tx_burst(PORT_OUT, worker_id, ...)`
8. Free các packet TX không nhận hết.

Lý do worker không lookup hash lại: dispatcher đã lookup rồi, mbuf metadata mang đủ `flow_idx`/`flow_gen`. Lookup lại trong worker sẽ tăng chi phí và tăng contention vô ích.

### `src/flow_table.c`

Quản lý:

- Tạo `rte_hash` với `rte_hash_crc`.
- Bật `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`.
- Lấy `storage_entries = rte_hash_max_key_id(hash) + 1`.
- Cấp phát `hot[]` và `cold[]` bằng `rte_zmalloc_socket`.
- Tạo QSBR variable.
- Attach RCU vào hash bằng `rte_hash_rcu_qsbr_add`.
- Cung cấp API register/quiescent/unregister.
- Thực hiện aging tick.
- Tính pressure mode theo active flow count.
- Chạy pressure maintenance để evict flow cũ khi table vượt watermark.
- Quản lý victim cache chứa packed position/generation của flow có thể bị thay.
- Cung cấp API `flow_table_evict_for_replacement()` để dispatcher evict victim và retry add flow mới.
- Victim cache hiện là một `rte_ring` SP/SC nội bộ. Ring object pack `position` và `flow_gen` vào 64 bit; khi pop thì key được dựng lại từ `cold[]` rồi validate trước khi delete.

Aging tick:

- Tính `timeout_cycles`.
- Scan theo budget khoảng `storage_entries / AGING_NUM_CHUNKS`.
- Dùng `rte_hash_iterate`.
- Nếu flow expired thì gom key vào batch.
- Trước khi delete recheck `last_seen` để tránh xóa flow vừa có packet mới.
- Delete bằng `rte_hash_del_key`.

### `src/flow_packet.c`

Parser hiện tại:

- Yêu cầu packet có Ethernet header.
- Chỉ nhận `RTE_ETHER_TYPE_IPV4`.
- Kiểm tra đủ IPv4 header.
- Dùng `rte_ipv4_hdr_len()` để tôn trọng IHL.
- Chỉ nhận TCP hoặc UDP.
- Kiểm tra đủ 2 port.
- Lưu IP/port/protocol vào key.

Limit:

- Không xử lý IPv6.
- Không xử lý IP fragment đầy đủ.
- Không đưa ICMP/ARP xuống worker.

### `src/spi_engine.c`

SPI là shallow packet inspection dựa trên 5-tuple, không đọc payload.

Rule table:

- Có 2 bảng để double-buffer reload.
- `g_active_rules` trỏ vào bảng đang dùng.
- Reload ghi vào bảng inactive rồi atomic swap pointer.
- Mỗi bảng có `version`.

Rule match:

- Rule có cờ `match_src_ip`, `match_dst_ip`, `match_src_port`, `match_dst_port`, `match_protocol`.
- `*` nghĩa là không match field đó.
- Match tuyến tính từ rule đầu đến rule cuối.
- Rule đầu tiên match sẽ quyết định action.

Tradeoff: linear match đơn giản và đủ với rule set nhỏ. Nếu rule count lớn, alternative là index theo protocol/port, trie, hash theo exact match, hoặc DPDK `rte_acl`.

### `src/stats_core.c`

Quản lý per-lcore stats bằng `rte_lcore_var`:

- `stats_init()`
- `stats_reset_all()`
- `stats_get_current()`
- `stats_get_lcore()`
- `stats_collect_totals()`
- helpers tính elapsed/rate/Mbps.

### `src/stats.c`

Stats thread làm nhiều việc nền:

- Sleep `AGING_INTERVAL_US`.
- Tạm offline/online QSBR quanh sleep.
- Reload SPI nếu có request.
- Chạy `flow_table_aging_tick()`.
- Nếu active flow vượt watermark, chạy `flow_table_pressure_maintenance()` để evict flow cũ, fill victim cache và giữ free reserve.
- Gọi quiescent.
- Drain RCU deferred reclaim.
- Mỗi full cycle 8 ticks thì aggregate stats và in console.

## 5. Runtime topology

### Lcores

Ví dụ chạy với `-l 0-5`:

- lcore 0: dispatcher/main
- lcore 1: stats/aging
- lcore 2: worker 0
- lcore 3: worker 1
- lcore 4: worker 2
- lcore 5: worker 3

Mapping thực tế phụ thuộc thứ tự `RTE_LCORE_FOREACH_WORKER`.

### Ports

`PORT_IN`:

- 1 RX queue.
- 1 TX queue setup phụ nhưng không phải egress chính.

`PORT_OUT`:

- 1 RX queue setup phụ.
- 4 TX queues, mỗi worker dùng queue bằng `worker_id`.

Telemetry runtime:

- `Deleted Flows`: tổng flow bị xóa từ mọi nguyên nhân.
- `Timeout Delete`: chỉ đếm flow timeout và delete thành công từ aging tick.
- `Pressure Evict`: chỉ đếm flow bị evict do pressure/replacement.
- `SPI Forwarded | Rule Checks`: counter thứ hai là số lần worker chạy SPI check trên cold-data snapshot hoặc cold-data parse fallback.

### Rings

Mỗi worker có một ring:

```text
dispatcher -> worker_ring_0 -> worker 0
dispatcher -> worker_ring_1 -> worker 1
dispatcher -> worker_ring_2 -> worker 2
dispatcher -> worker_ring_3 -> worker 3
```

Flags:

```c
RING_F_SP_ENQ | RING_F_SC_DEQ
```

Trong kiến trúc hiện tại chỉ có một producer là dispatcher và một consumer là worker tương ứng, nên SP/SC là đúng với ownership model.

## 6. Flow lifecycle

### Creation

Flow được tạo khi dispatcher lookup miss:

1. Add key vào `rte_hash`.
2. Nhận `flow_idx`.
3. Tăng `flow_gen`.
4. Fill `cold[flow_idx]`.
5. Tính worker.
6. Set `worker_id`.
7. Set `last_seen = current_tsc` cuối cùng để aging/pressure thấy slot đã publish xong.
8. Tăng `flows_created`.

### Update

Mỗi packet hit:

- Dispatcher update `last_seen`.
- Worker match SPI trên snapshot cold-data.
- Protocol counters tăng trong worker.

### Delete

Flow bị xóa bởi stats/aging thread:

- Aging scan hash.
- Nếu quá timeout thì delete key.
- Hash key reclaim được trì hoãn bởi RCU defer queue.

Side arrays không bị free từng slot. Slot có thể được reuse cho flow mới. Generation bảo vệ worker khỏi stale metadata.

Publication guard: trong lúc tạo flow mới, key có thể đã xuất hiện trong hash trước khi `hot[]/cold[]` được fill xong. Vì vậy dispatcher store `last_seen` cuối cùng. Aging, pressure maintenance và replacement đều skip slot có `last_seen == 0`.

### Replacement dưới overload

Khi số flow tiến gần capacity, hệ thống chuyển sang pressure mode:

1. Stats/aging thread tính pressure mode dựa trên active flow count.
2. Ở `PRESSURE`/`AGGRESSIVE`/`CRITICAL`, stats thread scan thêm một phần hash table.
3. Flow cũ được evict theo chính sách idle-first trong pressure mode; ở critical có thể evict mạnh hơn để tạo reserve.
4. Stats thread đưa candidate vào victim cache để dispatcher không phải scan nặng.
5. Nếu dispatcher gặp flow mới và `rte_hash_add_key()` fail, nó pop victim từ cache hoặc chạy emergency bounded scan nhỏ, delete victim, reclaim một phần, rồi retry add flow mới.
6. Trước khi xóa victim, code validate lại key, position, generation và `last_seen != 0` để tránh xóa nhầm slot đang publish hoặc slot đã đổi chủ.

Điểm quan trọng: replacement xóa state của flow cũ nhưng không cấm flow đó quay lại. Nếu packet của flow bị evict đến sau, nó lookup miss và được add lại như flow mới nếu còn capacity hoặc sau một replacement khác.

## 7. Data ownership model

Packet ownership:

- RX trả mbuf cho dispatcher.
- Nếu dispatcher filter/drop hoặc ring full: dispatcher free.
- Nếu enqueue thành công: worker sở hữu mbuf.
- Nếu worker SPI drop: worker free.
- Nếu TX burst nhận packet: PMD/TX path sở hữu.
- Nếu TX burst không nhận hết: worker free unsent suffix.

Flow table ownership:

- Dispatcher lookup/add/update `last_seen`.
- Dispatcher có thể evict victim trong cold path khi add flow mới fail.
- Stats/aging delete expired entries và pressure-evict victim để giữ reserve.
- Worker đọc `flow_gen`, snapshot `cold[]` và không ghi vào flow slot.

Stats ownership:

- Mỗi lcore ghi stats riêng.
- Stats thread đọc định kỳ và cộng dồn.

## 8. Current boundaries

- Fixed 4 workers.
- IPv4 TCP/UDP only.
- SPI rule match linear.
- Aging scan vẫn phụ thuộc số entry trong hash.
- Replacement không làm capacity vượt quá 1M active states; nó chỉ thay flow cũ bằng flow mới.
- Nếu workload có hơn 1M flow đều active thật sự, cần tăng capacity hoặc shard table; replacement sẽ hy sinh flow cũ.
- Không có machine-readable stats mode.
- PCAP PMD benchmark không đại diện NIC line-rate.
- Chưa có NUMA partitioning thật theo nhiều socket.
- Counter `spi_rule_checks` đếm số lần worker chạy SPI check. Nó không phải số rule hit riêng lẻ theo từng rule.
