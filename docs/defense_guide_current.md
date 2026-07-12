# FlowCore Defense Guide: Current Code

Tài liệu này dùng để ôn bảo vệ theo đúng source code hiện tại. Trọng tâm của project là Flow Table: parse 5-tuple, lookup/add flow, giữ flow affinity, aging, replacement khi overload, thống kê và chống lỗi concurrency khi `rte_hash`/side-array/ring chạy song song. SPI Engine hiện được giữ đơn giản: parse rule, reload rule, match rule trên snapshot cold-data của worker.

## 1. Câu trả lời tổng quan 60 giây

FlowCore là datapath DPDK có trạng thái. Dispatcher nhận packet theo burst từ `PORT_IN`, parse IPv4 TCP/UDP để lấy 5-tuple, lookup Flow Table bằng `rte_hash_lookup_bulk()`, tạo flow mới nếu miss, gán worker bằng hash của toàn bộ 5-tuple modulo `NUM_WORKERS`, rồi truyền `flow_idx/flow_gen` qua metadata của mbuf xuống worker ring. Flow metadata được tách thành `hot[]` và `cold[]`: `hot[]` chứa `last_seen`, `flow_gen`, `worker_id`; `cold[]` chứa IP, port, protocol và `create_time`.

Worker không lookup hash lại. Worker đọc `flow_idx/flow_gen` từ mbuf, kiểm tra generation, copy `cold[flow_idx]` thành snapshot cục bộ, kiểm tra generation lần nữa, rồi match SPI trên snapshot đó. Nếu metadata stale do flow bị aging/replacement và slot bị reuse, worker parse lại packet để lấy cold-data tạm và vẫn xử lý packet. Stats thread chạy aging, pressure replacement, rule reload, RCU reclaim và in telemetry. Khi Flow Table gần đầy, hệ thống dùng stateful replacement: flow cũ bị evict để flow mới vẫn có entry trong Flow Table.

Điểm cần nhấn mạnh trước hội đồng:

- Project không phải firewall hoàn chỉnh; SPI chỉ là workload/policy minh họa sau khi packet tới worker.
- Flow Table là trung tâm: lifecycle create/update/delete/replacement và concurrency quanh slot reuse.
- Không dùng stateless fallback làm thiết kế chính vì yêu cầu đề bài là quản lý Flow Table.
- Concurrency không chỉ dựa vào RCU; RCU bảo vệ hash entry, còn `flow_gen` + snapshot bảo vệ semantic của `hot[]/cold[]`.

## 2. Project Structure

### Header files

- `include/common.h`: compile-time config, global externs, pressure/replacement constants.
- `include/flow_table.h`: key, hot/cold metadata, Flow Table context, aging/pressure/replacement API.
- `include/flow_packet.h`: parse Ethernet/IPv4/TCP/UDP, convert key sang cold-data, protocol accounting.
- `include/spi_engine.h`: SPI action, rule struct, init/reload/match API.
- `include/stats.h`: per-lcore counters và helper tính rate.
- `include/app_init.h`, `include/app_threads.h`: bootstrap và thread entry declarations.

### Source files

- `src/main.c`: entry point mỏng, gọi install signal handler, init, run dispatcher, cleanup.
- `src/app_init.c`: EAL, mempool, port queues, Flow Table, SPI, rings, threads.
- `src/dispatcher.c`: RX, parse, lookup/add/replacement, route packet sang worker ring.
- `src/worker.c`: dequeue, generation validation, cold snapshot, SPI match, protocol stats, TX/drop.
- `src/flow_table.c`: `rte_hash`, QSBR, aging, pressure mode, victim cache, replacement, reclaim.
- `src/flow_packet.c`: parser và helper cold-data/stats.
- `src/spi_engine.c`: rule parser, wildcard matcher, double-buffer reload.
- `src/stats.c`: stats/aging thread, pressure maintenance, telemetry.
- `src/stats_core.c`: per-lcore stats allocation/aggregation/rate helpers.

### Tests

- `tests/flowcore_unit_tests.c`: parser/protocol/rate helpers.
- `tests/flow_slot_lifecycle_tests.c`: chứng minh generation-first order tránh stale slot.
- `tests/flowcore_functional_tests.c`: chạy binary thật bằng PCAP PMD.
- `tests/flow_table_overload_tests.c`: build hash nhỏ (`HASH_ENTRIES=64`) để test pressure/replacement/unpublished-slot guard.

## 3. Cấu Hình Quan Trọng

Trong `include/common.h`:

```c
#define NUM_WORKERS     4
#define RING_SIZE       4096
#define HASH_ENTRIES    (1024 * 1024)
#define BURST_SIZE      32
#define FLOW_TIMEOUT_SEC   5
#define AGING_NUM_CHUNKS    8
#define AGING_BATCH_SIZE    1024
```

Ý nghĩa:

- `NUM_WORKERS = 4`: cố định số worker. Phù hợp yêu cầu 4-8 worker, giảm độ phức tạp dynamic scaling.
- `HASH_ENTRIES = 1M`: capacity logic của Flow Table. Replacement không làm vượt quá capacity này; nó chỉ thay state cũ bằng state mới.
- `BURST_SIZE = 32`: cân bằng throughput và latency; đủ nhỏ để test dễ, đủ lớn để giảm overhead API.
- `FLOW_TIMEOUT_SEC = 5`: flow idle quá 5 giây bị aging.
- `AGING_NUM_CHUNKS = 8`: không scan toàn bộ 1M entry trong một tick; mỗi tick scan khoảng 1/8 table.

Pressure/replacement:

```c
#define FLOW_PRESSURE_THRESHOLD_PCT    92
#define FLOW_AGGRESSIVE_THRESHOLD_PCT  96
#define FLOW_CRITICAL_THRESHOLD_PCT    99
#define FLOW_PRESSURE_TARGET_PCT       92
#define FLOW_VICTIM_CACHE_SIZE         8192
#define FLOW_VICTIM_FILL_BATCH         512
#define FLOW_REPLACEMENT_RETRIES       8
#define FLOW_EMERGENCY_SCAN_BUDGET     512
#define FLOW_RECLAIM_REPLACEMENT_BUDGET 4
#define FLOW_RECLAIM_BUDGET            64
```

Ý nghĩa:

- 92% bắt đầu pressure.
- 96% tăng scan/evict mạnh hơn.
- 99% critical: stats/aging thread dùng min-idle bằng 0 và scan budget lớn hơn; dispatcher vẫn chỉ evict khi add flow mới fail.
- Victim cache giúp dispatcher không scan nặng trên hot path.
- `FLOW_EMERGENCY_SCAN_BUDGET` hiện đóng vai trò bật/tắt emergency path; implementation đang sample cố định 16 vòng, mỗi vòng 4 slot ngẫu nhiên, tức tối đa khoảng 64 random probes để tránh cold path treo lâu.
- `FLOW_RECLAIM_REPLACEMENT_BUDGET` được dispatcher dùng sau replacement. `FLOW_RECLAIM_BUDGET` hiện là macro dự phòng; stats thread đang gọi `flow_table_reclaim(1024)` trực tiếp mỗi tick.

Tradeoff:

- Compile-time constants dễ bảo vệ, dễ benchmark repeatable.
- Runtime config linh hoạt hơn nhưng cần parser, validation, telemetry và test thêm.
- Timeout 5 giây dễ demo aging trong functional test nhưng vẫn ngắn hơn production; hệ thống thật thường cần timeout theo protocol/state.

## 4. Data Structures Chi Tiết

### `struct ipv4_5tuple_key`

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

Vai trò:

- Đây là key của `rte_hash`.
- Field giữ theo network byte order lấy trực tiếp từ packet.
- `pad[3]` đưa struct lên 16 bytes, làm layout ổn định và tránh byte padding không kiểm soát.
- `packed` giúp layout byte chính xác khi đưa vào hash.

Tradeoff:

- Tốn thêm 3 bytes/key.
- Đổi lại key deterministic, dễ so sánh bằng byte và phù hợp hash CRC.

Alternative:

- Key 13 bytes không padding: tiết kiệm hơn nhưng dễ dính padding/unaligned access.
- Canonical bidirectional key: tốt nếu muốn hai chiều TCP cùng flow, nhưng project hiện coi 5-tuple một chiều là một flow.
- Include VLAN/IP version: cần nếu mở rộng thực tế.

### `struct flow_hot_data`

```c
struct flow_hot_data {
    uint64_t last_seen;
    uint32_t flow_gen;
    uint8_t  worker_id;
    uint8_t  pad[3];
};
```

Field:

- `last_seen`: TSC timestamp packet hợp lệ gần nhất. Dispatcher update trên hit/miss. Aging/pressure đọc để timeout/evict.
- `flow_gen`: generation của slot side-array. Tăng mỗi lần slot được dùng cho flow mới. Worker dùng để phát hiện mbuf metadata stale.
- `worker_id`: worker được gán cho flow. Flow hit đọc lại field này để giữ flow affinity.
- `pad[3]`: giữ struct compact/alignment rõ ràng.

Điểm quan trọng:

- `last_seen == 0` hiện là marker “slot chưa publish xong”. Dispatcher add key vào hash trước, fill metadata sau, rồi store `last_seen` cuối cùng. Aging/pressure/replacement skip slot có `last_seen == 0`.
- `flow_gen` không reset runtime. Nếu overflow về 0, helper increment tiếp để tránh generation 0.

Tradeoff:

- Hot struct rất nhỏ, memory footprint tốt cho 1M flow.
- Chưa pad mỗi entry ra cache line, nên có thể false sharing nếu nhiều core update adjacent flows. Với 1M flow, padding cache-line sẽ tốn RAM lớn.

Alternative:

- Một struct flow lớn: code đơn giản hơn nhưng dispatcher hit path kéo nhiều cold fields vào cache.
- Cache-line pad hot entry: giảm false sharing nhưng tăng memory footprint.
- Per-worker/per-shard hot arrays: locality tốt hơn nhưng routing và migration phức tạp hơn.

### `struct flow_cold_data`

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

Field:

- `src_ip`, `dst_ip`, `src_port`, `dst_port`, `protocol`: bản sao 5-tuple để worker match SPI và account protocol mà không parse lại packet.
- `create_time`: TSC lúc flow tạo. Hữu ích cho debug/future policy.

Tradeoff:

- Duplicate data với hash key, tốn RAM.
- Đổi lại worker không phải lookup hash hoặc parse packet trong normal path.

### `struct flow_table_ctx`

```c
struct flow_table_ctx {
    struct rte_hash      *hash;
    struct rte_rcu_qsbr  *qsv;
    uint32_t              storage_entries;
    struct flow_hot_data  *hot;
    struct flow_cold_data *cold;
    uint32_t              aging_iter;
    uint32_t              pressure_iter;
};
```

Field:

- `hash`: DPDK hash map 5-tuple -> key id/position.
- `qsv`: QSBR state để `rte_hash` defer reclaim sau delete.
- `storage_entries`: số slot side-array thật, lấy bằng `rte_hash_max_key_id() + 1`, không giả định bằng `HASH_ENTRIES`.
- `hot`, `cold`: side arrays do app quản lý, index bằng position từ `rte_hash`.
- `aging_iter`: iterator state cho aging scan.
- `pressure_iter`: iterator state cho pressure maintenance.

Tại sao aging và pressure có iterator riêng:

- Aging và pressure maintenance có mục tiêu khác nhau.
- Nếu dùng chung iterator, pressure scan có thể làm aging bỏ qua vùng hoặc ngược lại.
- Emergency replacement hiện không dùng iterator; khi victim cache không có candidate hợp lệ, nó dùng bounded random sampling nhỏ.

### `struct flow_aging_result`

```c
struct flow_aging_result {
    uint32_t scanned;
    uint32_t expired;
    uint32_t deleted;
};
```

- `scanned`: số entry đã iterate.
- `expired`: số entry thấy quá timeout ở lần scan đầu.
- `deleted`: số entry delete thành công sau khi recheck `last_seen`.

Vì sao `expired` có thể lớn hơn `deleted`:

- Packet mới có thể update `last_seen` giữa lúc aging scan và lúc flush delete.
- Flow có thể bị delete bởi pressure/replacement trước.
- Position có thể stale.

### Pressure mode

```c
enum flow_pressure_mode {
    FLOW_PRESSURE_NORMAL,
    FLOW_PRESSURE_PRESSURE,
    FLOW_PRESSURE_AGGRESSIVE,
    FLOW_PRESSURE_CRITICAL,
};
```

Ý nghĩa:

- `NORMAL`: active flow dưới 92%.
- `PRESSURE`: bắt đầu chủ động tìm idle victim.
- `AGGRESSIVE`: scan/evict mạnh hơn.
- `CRITICAL`: có thể evict flow bất kỳ trong bounded scan để nhận flow mới.

Tradeoff:

- Watermark policy dễ hiểu, dễ test.
- Không phải exact LRU; có thể evict flow chưa phải “ít dùng nhất”.

### `struct flow_victim_candidate`

```c
struct flow_victim_candidate {
    struct ipv4_5tuple_key key;
    uint32_t position;
    uint32_t flow_gen;
    uint64_t last_seen;
};
```

Tại sao lưu đủ cả key, position, generation:

- `key`: cần để delete bằng `rte_hash_del_key()`.
- `position`: xác nhận lookup key vẫn trả đúng slot cũ.
- `flow_gen`: chống ABA slot reuse. Nếu slot đã được flow khác dùng lại, generation đổi, candidate stale.
- `last_seen`: dùng để chọn oldest hoặc debug policy.

### Victim cache ring

Trong `flow_table.c`, victim cache hiện là một `rte_ring *g_victim_ring` tạo với flag:

```c
RING_F_SP_ENQ | RING_F_SC_DEQ
```

Vai trò:

- Stats/aging thread scan trước và push victim candidate vào cache bằng single-producer enqueue.
- Dispatcher khi add fail pop candidate bằng single-consumer dequeue burst, validate lại và delete. Nó không phải scan full table trong trường hợp cache đã có sẵn candidate.
- Khi enqueue, code pack `position` và `flow_gen` vào một object 64 bit. Khi dequeue, key được dựng lại từ `cold[position]`.

Tradeoff:

- Ring SP/SC không cần spinlock trong model một producer chính là stats/aging thread và một consumer chính là dispatcher.
- Candidate có thể stale; vì vậy `candidate_delete_if_valid()` luôn revalidate bằng key/position/generation/last_seen.
- Kích thước ring dùng `FLOW_VICTIM_CACHE_SIZE`, hiện là 8192 candidate.

Alternative:

- Exact LRU list: cập nhật mỗi packet, hot path nặng.
- Random eviction: rẻ nhưng có thể evict hot flow.
- Min-heap theo last_seen: update heap trên mỗi hit rất đắt.
- Timing wheel: tốt cho timeout, nhưng replacement theo pressure vẫn cần victim policy.

### `struct lcore_stats`

Counters quan trọng:

- RX/TX: `rx_pkts`, `rx_bytes`, `tx_pkts`, `tx_bytes`.
- Worker: `worker_rx_pkts`.
- Drop/error: `rx_filtered_pkts`, `ring_drop_pkts`, `hash_add_failures`, `tx_drop_pkts`.
- Flow lifecycle: `flows_created`, `flows_deleted`.
- Replacement: `replacement_attempts`, `replacement_success`, `replacement_failures`, `victim_evicted_flows`, `victim_cache_empty`, `flow_add_retry_success`, `flow_add_retry_failures`.
- SPI: `spi_pkts_forwarded`, `spi_pkts_dropped`, `spi_rule_checks`.
- Aging: `aging_scanned`, `aging_expired`, `aging_deleted`, `aging_reclaimed`.
- Protocol: `http_pkts`, `https_pkts`, `dns_pkts`, `tcp_pkts`, `udp_pkts`, `other_pkts`.

Lưu ý về `spi_rule_checks`:

- Counter này đếm số lần worker chạy SPI check trên packet.
- Nó không phải số hit theo từng rule; packet đi theo default-forward vẫn được tính là một check.

Lưu ý về semantics runtime:

- `flows_deleted`: tổng flow bị xóa từ timeout hoặc pressure/replacement.
- `aging_expired`: số flow timeout được phát hiện ở vòng scan đầu, chưa chắc đã delete thành công.
- `aging_deleted`: số flow timeout delete thành công sau bước recheck.
- `victim_evicted_flows`: số flow bị evict do pressure/replacement.

Tradeoff per-lcore stats:

- Mỗi lcore ghi counter của mình, tránh atomic global counter trên hot path.
- Stats thread đọc snapshot không transactional. Với telemetry realtime, chấp nhận được.
- Nếu dùng cho billing tuyệt đối, cần cơ chế chính xác hơn và tốn chi phí hơn.

## 5. Bootstrap Và Thread Model

### `app_init()`

Thứ tự:

1. Đọc env:
   - `FLOWCORE_NUM_MBUFS`
   - `FLOWCORE_MBUF_DATA_SIZE`
   - `FLOWCORE_RULES_PATH`
2. `rte_eal_init(argc, argv)`.
3. `stats_init()`.
4. Check port count và lcore count.
5. Tạo mbuf pool bằng `rte_pktmbuf_pool_create()`.
6. Configure/start ports.
7. `flow_table_init()`.
8. `spi_rule_engine_init()`.
9. Tạo worker rings.
10. Launch stats thread và worker threads.

Yêu cầu lcore:

- Main lcore chạy dispatcher.
- 1 lcore chạy stats/aging.
- 4 lcore chạy worker.
- Tổng tối thiểu: `NUM_WORKERS + 2 = 6`.

### `ports_init()`

Thiết kế:

- `PORT_IN`: 1 RX queue, 1 TX queue setup phụ.
- `PORT_OUT`: 1 RX queue setup phụ, `NUM_WORKERS` TX queues.
- Worker `w` TX qua queue `w`.
- Ứng dụng hiện yêu cầu tối thiểu 2 ports khả dụng vì cố định `PORT_IN = 0`, `PORT_OUT = 1`.
- Mọi queue setup hiện được check lỗi và fail-fast nếu cấu hình port không đáp ứng topology này.

Tradeoff:

- Mỗi worker TX queue riêng tránh lock/serialization TX queue.
- Chưa có dedicated TX thread; worker xử lý luôn TX. Đơn giản, ít ring hơn, nhưng nếu TX là bottleneck thì có thể cần TX thread hoặc batching tập trung.

### `rings_init()`

Mỗi worker có một ring:

```text
dispatcher -> worker_ring_i -> worker i
```

Flag:

```c
RING_F_SP_ENQ | RING_F_SC_DEQ
```

Vì chỉ có một producer là dispatcher và một consumer là worker tương ứng, SP/SC đúng ownership model và nhanh hơn MP/MC.

### Signal handling

- `SIGINT`, `SIGTERM`: set `force_quit = 1`.
- `SIGUSR1`: gọi `spi_rule_engine_request_reload()`.
- `force_quit` là `volatile sig_atomic_t`, phù hợp để signal handler set flag đơn giản.

## 6. Packet Lifecycle Chi Tiết

### Case A: packet unsupported/truncated

Trong dispatcher:

1. RX bằng `rte_eth_rx_burst()`.
2. `flow_packet_extract_key()` kiểm tra Ethernet type, IPv4 header, protocol TCP/UDP, đủ port.
3. Nếu unsupported/truncated:
   - tăng `rx_filtered_pkts`
   - `rte_pktmbuf_free(m)`
   - packet không vào Flow Table.

Lý do:

- Project tập trung IPv4 TCP/UDP flow. Non-TCP/UDP không có 5-tuple L4 phù hợp với current key.

### Case B: flow hit

Dispatcher:

1. `rte_hash_lookup_bulk()` trả `flow_idx >= 0`.
2. Check `flow_idx < storage_entries`.
3. Đọc `target_worker = hot[flow_idx].worker_id`.
4. Load `flow_gen`.
5. Store `last_seen = current_tsc`.
6. Ghi metadata vào mbuf:
   - `m->hash.fdir.lo = flow_idx`
   - `m->hash.fdir.hi = flow_gen`
7. Đưa mbuf vào buffer của worker.

Worker:

1. Dequeue ring.
2. Đọc `flow_idx/flow_gen`.
3. Nếu generation khớp, copy `cold[flow_idx]`.
4. Check generation lại.
5. Match SPI trên snapshot.
6. Account protocol.
7. Drop hoặc TX.

### Case C: flow miss, add thành công

Dispatcher:

1. Gọi `rte_hash_add_key()`.
2. Nếu add thành công, nhận `flow_idx`.
3. Check bounds.
4. `flow_hot_generation_next(&hot[flow_idx])`:
   - tăng generation
   - nếu wrap về 0, tăng tiếp
   - mục tiêu là invalidate packet cũ đang giữ metadata của occupant trước.
5. Fill `cold[flow_idx]` từ key.
6. Tính worker bằng `rte_hash_hash(key) % NUM_WORKERS`.
7. Set `hot[flow_idx].worker_id`.
8. Store `last_seen` cuối cùng để publish slot.
9. Tăng `flows_created`.

Tại sao `last_seen` được store cuối:

- Sau `rte_hash_add_key`, key có thể visible với aging/pressure.
- Nếu aging/pressure thấy slot trước khi metadata fill xong, có thể delete/reuse sai.
- Code dùng `last_seen == 0` như unpublished marker; maintenance paths skip marker này.

### Case D: flow miss, add fail

Dispatcher:

1. Tăng `hash_add_failures`.
2. Gọi `dispatcher_evict_for_replacement()`.
3. Nếu evict thành công:
   - tăng replacement counters
   - báo quiescent
   - reclaim một phần
   - retry `rte_hash_add_key()`
4. Nếu retry thành công:
   - tăng `flow_add_retry_success`
   - tạo flow như case C
5. Nếu retry fail:
   - tăng `flow_add_retry_failures`
   - free mbuf

Điểm bảo vệ:

- Hệ thống cố nhận flow mới bằng stateful replacement.
- Nếu vẫn fail, nghĩa là capacity/reclaim/pressure không theo kịp hoặc table thật sự quá tải. Không có magic để chứa hơn capacity state cùng lúc.

### Case E: same-burst duplicate miss

Vấn đề:

- `lookup_bulk()` chạy trước add.
- Nếu trong cùng burst có hai packet cùng flow mới, cả hai đều miss.

Giải pháp:

- Dispatcher dùng:
  - `resolved_indices`
  - `resolved_positions`
  - `resolved_workers`
  - `resolved_generations`
- Packet sau cùng key sẽ reuse kết quả packet trước trong cùng burst, tránh add trùng và giữ cùng worker/generation.

Tradeoff:

- Linear scan trong `resolved_count`.
- `BURST_SIZE = 32`, chi phí chấp nhận được.
- Alternative mini-hash trong burst phức tạp hơn.

### Case F: ring full

Dispatcher:

- `rte_ring_enqueue_burst()` có thể enqueue ít hơn `worker_counts[w]`.
- Phần không enqueue:
  - tăng `ring_drop_pkts`
  - free mbuf.

Đây là backpressure/drop point rõ ràng. Alternative là retry/spin, nhưng sẽ block dispatcher và làm RX backlog nặng hơn.

### Case G: TX partial

Worker:

- `rte_eth_tx_burst()` có thể nhận ít hơn `tx_count`.
- Packet TX thành công do PMD/TX path sở hữu.
- Packet chưa TX:
  - worker giữ trong pending TX cục bộ
  - worker ưu tiên flush pending trước khi dequeue thêm packet từ ring
  - nếu pending không còn chỗ hoặc khi thoát chương trình vẫn chưa gửi được thì tăng `tx_drop_pkts` và free mbuf.

## 7. Worker: Vì Sao Không Parse Lại Mọi Packet

Normal path:

- Dispatcher đã parse packet để lấy 5-tuple.
- Dispatcher đã lookup hash.
- Mbuf metadata mang `flow_idx/flow_gen`.
- Worker chỉ cần validate generation và dùng `cold[]` snapshot.

Khi nào worker phải parse lại:

- `flow_idx >= storage_entries`.
- `flow_gen` trong mbuf không khớp `hot[flow_idx].flow_gen`.
- Generation khớp trước copy nhưng đổi sau copy.

Tại sao cần double-check generation:

```text
worker check gen OK
aging/replacement delete flow A
dispatcher reuse slot cho flow B, increment gen, overwrite cold[]
worker đọc cold[] trực tiếp -> có thể đọc nhầm B
```

Code hiện:

```text
check generation
copy cold[] to slot_cold
check generation again
if changed -> parse packet fallback
```

Tradeoff:

- Thêm một copy `flow_cold_data` nhỏ và một generation load.
- Đổi lại không cần worker lookup hash lại và tránh stale side-array bug.

Alternative:

- Worker lookup hash lại bằng packet parse: đúng nhưng tốn parse + hash contention.
- Copy full cold metadata vào mbuf ở dispatcher: đúng nhưng tăng per-packet metadata/copy.
- Inflight refcount: ngăn reuse khi packet pending, chính xác hơn nhưng thêm atomic increment/decrement hot path.

## 8. Flow Lifecycle

### Creation

Trigger: dispatcher lookup miss và add/retry add thành công.

State cập nhật:

- Hash: thêm key.
- `hot[flow_idx].flow_gen`: increment.
- `cold[flow_idx]`: fill key + create_time.
- `hot[flow_idx].worker_id`: set target worker.
- `hot[flow_idx].last_seen`: publish cuối.
- Stats: `flows_created++`.

### Update

Trigger: packet hit hoặc same-burst duplicate.

State cập nhật:

- `last_seen = current_tsc`.
- Packet vẫn dùng generation hiện tại.
- Protocol stats cập nhật ở worker.

### Aging delete

Trigger: stats thread gọi `flow_table_aging_tick()`.

Flow:

1. Tính `timeout_cycles = FLOW_TIMEOUT_SEC * rte_get_tsc_hz()`.
2. Scan tối đa `storage_entries / AGING_NUM_CHUNKS`.
3. Với entry quá timeout, lưu `next_key` và `position` vào batch.
4. Khi batch full hoặc cuối tick, gọi `flush_expired_batch()`.
5. `flush_expired_batch()` recheck:
   - position valid
   - `last_seen != 0`
   - vẫn quá timeout
6. Delete bằng `rte_hash_del_key()`.
7. Stats thread quiescent rồi reclaim bằng `flow_table_reclaim()`.

Vì sao recheck:

- Dispatcher có thể update `last_seen` giữa scan và delete.
- Không recheck sẽ xóa nhầm active flow.

### Replacement delete

Trigger:

- Stats thread pressure maintenance.
- Dispatcher add fail.

Victim validation:

- Lookup candidate key lại.
- Position phải khớp.
- Generation phải khớp.
- `last_seen != 0`.
- Nếu policy yêu cầu idle, idle time phải đủ.

Nếu bất kỳ điều kiện nào fail, candidate stale và không delete.

### Re-add after evict

Nếu packet của flow bị evict đến sau:

- Lookup miss.
- Dispatcher add lại như flow mới.
- Generation của slot mới có thể khác, worker vẫn có safety check.

Đây là đúng với stateful replacement: evict làm mất state cũ, không cấm flow quay lại.

## 9. Aging Và Pressure/Overload

### Normal aging

Mục tiêu:

- Xóa flow idle.
- Giữ memory/hash không đầy vì flow chết.
- Chia scan theo chunk để tránh spike CPU.

Tradeoff:

- Hash iterator scan vẫn phụ thuộc table size, không chỉ active flow.
- Với chục triệu flow, timing wheel/expiry bucket tốt hơn.

### Pressure maintenance

`flow_table_pressure_mode(active_flows)`:

- `<92%`: normal.
- `>=92%`: pressure.
- `>=96%`: aggressive.
- `>=99%`: critical.

`pressure_min_idle_cycles()`:

- Pressure: timeout/2.
- Aggressive: timeout/4.
- Critical: 0, có thể evict bất kỳ published flow.

`pressure_scan_budget()`:

- Base = `storage_entries / AGING_NUM_CHUNKS`.
- Pressure: base.
- Aggressive: base * 2.
- Critical: base * 4.

`pressure_evict_budget()`:

- Budget mục tiêu là `active_flows - target_92%`. Số flow xóa thực tế có thể thấp hơn vì còn phụ thuộc scan budget, min-idle policy và candidate hợp lệ. Code hiện không cap riêng theo mode; mode ảnh hưởng scan budget và min idle.

### Victim cache

Pressure maintenance làm hai việc:

- Trực tiếp evict một số flow theo budget.
- Sau khi hết evict budget, push candidate vào victim cache cho dispatcher dùng khi add fail.

Lý do:

- Dispatcher không nên scan nhiều trên RX hot path.
- Stats thread đang là maintenance/control plane, phù hợp hơn để scan.

### Dispatcher replacement path

`dispatcher_evict_for_replacement()`:

- Tăng `replacement_attempts`.
- Nếu victim cache empty, tăng `victim_cache_empty`.
- Gọi `flow_table_evict_for_replacement(FLOW_REPLACEMENT_RETRIES, FLOW_EMERGENCY_SCAN_BUDGET)`.
- Nếu success:
  - tăng `replacement_success`
  - tăng `victim_evicted_flows`
  - tăng `flows_deleted`
  - quiescent
  - reclaim `FLOW_RECLAIM_REPLACEMENT_BUDGET`
- Nếu fail: `replacement_failures++`.

Tradeoff:

- Có thêm work trên flow miss/add fail, nhưng không ảnh hưởng flow hit.
- Emergency scan hiện là bounded random sampling nhỏ nên không đảm bảo luôn tìm victim, nhưng tránh latency spike.

## 10. Concurrency Model

### Shared objects

- `rte_hash`: dispatcher lookup/add, stats delete, pressure delete.
- `hot[]`: dispatcher writes `last_seen/worker_id/flow_gen`, worker reads `flow_gen`, maintenance reads `last_seen/flow_gen`.
- `cold[]`: dispatcher writes on create, worker snapshots.
- Worker rings: dispatcher enqueues, worker dequeues.
- SPI active rule pointer: stats thread stores on reload, workers load.
- Per-lcore stats: each lcore writes its own counters, stats thread aggregates.

### Synchronization mechanisms

- `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`: DPDK hash supports concurrent read/write.
- QSBR RCU: hash delete/reclaim lifecycle.
- Atomic helpers:
  - `last_seen` relaxed load/store.
  - `flow_gen` acquire load, seq_cst increment.
  - active rule pointer acquire/release.
- `flow_gen`: semantic guard for side-array slot reuse.
- Worker snapshot + double-check: prevents direct stale `cold[]` use.
- Victim validation: key/position/generation/last_seen check before delete.
- Per-lcore stats: avoids global atomic counter hot path.
- SP/SC rings: matches one dispatcher producer, one worker consumer.
- Worker không register QSBR trong code hiện tại vì worker không lookup/delete `rte_hash`; worker chỉ đọc side arrays qua `flow_idx/flow_gen` và dựa vào generation + snapshot để bảo vệ semantic slot reuse.

### RCU bảo vệ gì và không bảo vệ gì

RCU bảo vệ:

- Internal hash key/data reclaim sau delete.
- Reader không đọc memory hash đã free khi chưa quiescent.

RCU không bảo vệ:

- Ý nghĩa của `hot[flow_idx]` và `cold[flow_idx]`.
- Packet cũ trong ring đang giữ `flow_idx`.
- Slot side-array bị reuse cho flow khác.

Vì vậy cần `flow_gen` và worker snapshot. Đây là câu trả lời quan trọng nếu hội đồng hỏi “đã có RCU rồi sao còn generation?”.

## 11. SPI Engine: Giữ Đơn Giản

Project tập trung Flow Table nên SPI hiện chỉ làm policy minh họa.

### `struct spi_rule`

```c
struct spi_rule {
    char name[SPI_RULE_NAME_LEN];
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

Các `match_*` flag cho phép wildcard `*`. Nếu `match_dst_port = 0`, rule không so port đích.

### Rule table

Trong `spi_engine.c`:

```c
struct spi_rule_table {
    uint32_t version;
    uint32_t count;
    struct spi_rule rules[SPI_MAX_RULES];
};

static struct spi_rule_table g_rule_tables[2];
static struct spi_rule_table *g_active_rules;
static volatile sig_atomic_t g_reload_requested;
static char g_rules_path[256];
```

Double-buffer reload:

- Một table active.
- Table còn lại inactive để parse file mới.
- Parse thành công thì atomic store pointer sang inactive.
- Parse fail thì giữ active cũ.

Worker matching:

- Worker gọi `spi_rule_engine_match_cold(cold)`.
- Hàm load active rule table bằng acquire.
- Match tuyến tính từ rule 0 tới `count - 1`.
- Rule đầu tiên match quyết định action.
- Nếu không rule nào match, default forward.

Tradeoff:

- Tuyến tính qua tối đa 256 rule, đơn giản và đủ cho mini project.
- Không cache action theo flow để tránh ghi nhầm vào slot reuse.
- Nếu rule set lớn, dùng `rte_acl` hoặc index theo protocol/port.

## 12. Stats Và Telemetry

### Per-lcore stats

`stats_core.c` dùng:

```c
static RTE_LCORE_VAR_HANDLE(struct lcore_stats, g_lcore_stats);
```

Ý nghĩa:

- Mỗi lcore có bản `struct lcore_stats` riêng.
- Dispatcher/worker/stats thread gọi `stats_get_current()` để ghi counter của chính mình.
- Stats thread gọi `stats_collect_totals()` để cộng tất cả lcore.

Tradeoff:

- Hot path không atomic increment global.
- Snapshot có thể không chính xác tuyệt đối từng nanosecond.

### Rate helpers

- `stats_elapsed_seconds(current_tsc, previous_tsc, tsc_hz)`: đổi TSC delta sang giây.
- `stats_rate_per_sec(current, previous, elapsed)`: counter delta / elapsed.
- `stats_mbps(current_bytes, previous_bytes, elapsed)`: bytes delta -> bits -> Mbps.

### Stats thread loop

Trong `stats_thread()`:

1. Offline QSBR trước khi sleep.
2. Sleep `AGING_INTERVAL_US`.
3. Online QSBR.
4. Reload SPI nếu requested.
5. Run `flow_table_aging_tick()`.
6. Nếu pressure mode != normal, run `flow_table_pressure_maintenance()`.
7. Quiescent.
8. Reclaim deferred hash entries.
9. Mỗi `AGING_NUM_CHUNKS` tick, in telemetry.

Vì sao offline khi sleep:

- Thread đang sleep không giữ read-side critical section.
- Cho RCU reclaim không bị chờ một sleeping reader.

## 13. Function-by-Function Ghi Nhớ

### `flow_packet_extract_key()`

- Input: pointer packet data và data_len.
- Check đủ Ethernet header.
- Check EtherType IPv4.
- Check đủ IPv4 header.
- Lấy `l3_len` từ IHL.
- Chỉ nhận TCP/UDP.
- Check đủ 4 bytes port.
- Fill `ipv4_5tuple_key`.

Giới hạn:

- Hiện parser chỉ dùng dữ liệu từ segment đầu tiên của mbuf.
- Không có xử lý đầy đủ cho fragmented IPv4 hoặc scattered RX/jumbo path.

Nếu bị hỏi endian:

- IP/port giữ network byte order.
- Rule parser cũng convert port/IP về network byte order, nên compare trực tiếp.

### `flow_cold_from_key()`

- Zero struct.
- Copy key fields sang cold fields.
- Set `create_time`.

### `flow_stats_account_protocol()`

- TCP/80 -> HTTP.
- TCP/443 -> HTTPS.
- UDP/53 -> DNS.
- TCP còn lại -> TCP.
- UDP còn lại -> UDP.
- Other -> OTHER, nhưng dispatcher hiện filter non-TCP/UDP nên OTHER thường bằng 0.

### `flow_table_init()`

- Reset global context và victim cache.
- Tạo `rte_hash`.
- Lấy `storage_entries`.
- Allocate `hot[]`, `cold[]`, `qsv`.
- Init QSBR.
- Attach QSBR vào hash.
- Reset iterators.

### `flow_table_aging_tick()`

- Scan một chunk hash table.
- Thu expired key/position vào batch.
- Recheck và delete batch.
- Return counters.

### `flow_table_pressure_maintenance()`

- Nếu normal thì return.
- Tính scan/evict budget và min idle.
- Iterate hash bằng `pressure_iter`.
- Skip unpublished slot `last_seen == 0`.
- Nếu còn evict budget, validate/delete candidate.
- Nếu hết evict budget, push candidate vào victim cache.

### `flow_table_evict_for_replacement()`

- Pop tối đa `max_cached_attempts` từ victim cache.
- Mỗi candidate đều revalidate.
- Nếu không evict được cached victim, emergency scan bounded bằng random sampling nhỏ.

### `flow_table_reclaim()`

- Gọi `rte_hash_rcu_qsbr_dq_reclaim()` nhiều round.
- Trả số entry reclaimed.

### `dispatcher_thread()`

Biến nội bộ quan trọng:

- `pkts_burst[BURST_SIZE]`: mbuf nhận từ RX.
- `worker_buffers[NUM_WORKERS][BURST_SIZE]`: gom packet theo worker trước khi enqueue.
- `worker_counts[NUM_WORKERS]`: số packet đã gom cho mỗi worker.
- `keys[BURST_SIZE]`: 5-tuple parse từ packet hợp lệ.
- `key_ptrs[BURST_SIZE]`: pointer cho `rte_hash_lookup_bulk`.
- `positions[BURST_SIZE]`: kết quả lookup.
- `resolved_*`: xử lý duplicate miss trong cùng burst.
- `valid_mbufs[BURST_SIZE]`: chỉ các packet parse OK.

### `worker_thread()`

Biến nội bộ:

- `pkts[BURST_SIZE]`: packet dequeue.
- `tx_pkts[BURST_SIZE]`: packet forward được gom để TX.
- `flow_idx`, `flow_gen`: metadata từ mbuf.
- `parsed_cold`: cold-data tạm khi fallback parse.
- `slot_cold`: snapshot từ side-array khi generation hợp lệ.
- `cold`: pointer tới snapshot hoặc parsed_cold.
- `action`: SPI FORWARD/DROP.

### `stats_thread()`

Biến nội bộ:

- `prev_totals`: snapshot counter lần trước để tính rate.
- `prev_worker_*`: per-worker previous counters.
- `tick_counter`: chỉ in màn hình sau đủ `AGING_NUM_CHUNKS` tick.
- `pressure_active_flows`: active count trước pressure maintenance.
- `victim_cache_count`: telemetry victim cache.

## 14. Tradeoff Và Alternatives Theo Kỹ Thuật

### Full 5-tuple hash vs SourceIP % N

Chọn full 5-tuple:

- Phân phối đều hơn khi nhiều flow cùng source IP.
- NAT/proxy/load test thường có source IP trùng nhiều.
- Gắn với key thật của Flow Table.

Nhược:

- Khác ví dụ tài liệu gốc `SourceIP % N`.
- Hai chiều của cùng kết nối có thể đi worker khác nếu key một chiều.

Alternative:

- SourceIP % N: đơn giản nhưng dễ lệch tải.
- RSS hardware: tốt để pre-shard RX queue, nhưng không thay Flow Table lifecycle.
- Consistent hashing: tốt cho dynamic worker scaling, phức tạp hơn.

### Lock-free `rte_hash` + RCU vs mutex

Chọn `rte_hash` LF + QSBR:

- Lookup/add/delete concurrent.
- Tránh global lock bottleneck.
- Phù hợp điểm cộng “Lock-Free Flow Table”.

Nhược:

- Debug khó hơn.
- Phải register/quiescent/reclaim đúng.
- RCU không tự bảo vệ side arrays.

Alternative:

- Global mutex: dễ nhưng bottleneck.
- Per-bucket lock: đơn giản hơn LF nhưng vẫn contention.
- Per-worker table: giảm shared writes nhưng cần routing/sharding rõ.

### Chunked aging vs timing wheel

Chọn chunked aging:

- Dễ implement với `rte_hash_iterate`.
- Dễ test.
- Không update timer structure trên mỗi packet.

Nhược:

- Chi phí scan phụ thuộc capacity.
- Timeout không chính xác tuyệt đối từng ms.

Alternative:

- Timing wheel/expiry bucket: scale tốt hơn cho nhiều triệu flow nhưng cần update/lazy update phức tạp.
- Min-heap: delete min nhanh nhưng update per packet đắt.

### Victim cache replacement vs exact LRU

Chọn victim cache:

- Stats thread scan, dispatcher chỉ pop/validate.
- Không update LRU trên mọi packet.
- Bounded emergency scan tránh stall.

Nhược:

- Không đảm bảo victim là cold nhất toàn hệ thống.
- Candidate stale cần revalidate.
- Chưa có metric riêng cho số candidate stale trong victim cache; hiện chỉ có replacement success/failure và cache empty.

Alternative:

- Exact LRU: quá nặng trên hot path.
- Random victim: đơn giản nhưng dễ evict hot flow.
- Per-worker/sharded victim queues: tốt hơn khi multi-dispatcher, phức tạp hơn hiện tại.

### Snapshot SPI vs per-flow SPI cache

Chọn snapshot:

- An toàn khi slot reuse.
- Không ghi SPI state vào Flow Table hot data.
- SPI là phụ trợ, không nên làm phức tạp core Flow Table.

Nhược:

- Match rule mỗi packet.

Alternative:

- Per-flow SPI cache: nhanh hơn nhưng cần pin/refcount hoặc atomic state để không ghi nhầm slot reuse.
- `rte_acl`: đúng hướng nếu SPI trở thành trọng tâm.

### Per-lcore stats vs atomic global stats

Chọn per-lcore:

- Không cache-line bouncing trên hot path.
- Cộng dồn định kỳ đủ cho telemetry.

Nhược:

- Snapshot không transactional.

Alternative:

- Atomic global: dễ đọc nhưng hot path chậm.
- Per-worker + periodic flush: tương tự nhưng ít tổng quát hơn DPDK lcore vars.

## 15. Câu Hỏi Hội Đồng Có Thể Hỏi

### Flow Table và lifecycle

**Q: Tại sao không chỉ dùng hardware RSS để chia worker?**

A: RSS chỉ chọn RX queue dựa trên hash, không quản lý lifecycle flow. Project cần create/update/delete/timeout, `last_seen`, `worker_id`, active/created/deleted stats, aging, replacement và policy state. RSS có thể là pre-sharding trong version nâng cao, không thay thế Flow Table.

**Q: Nếu hash table full thì flow mới xử lý thế nào?**

A: Dispatcher thử add. Nếu add fail, nó evict victim qua victim cache/emergency bounded scan, reclaim một phần rồi retry add. Nếu retry thành công, packet hiện tại có flow entry stateful. Nếu vẫn fail, hệ thống đã vượt capacity/reclaim hiện tại; cần tăng capacity, shard table hoặc chấp nhận drop.

**Q: Replacement có làm capacity tăng hơn 1M không?**

A: Không. Nó chỉ thay state cũ bằng state mới. Active states tại một thời điểm vẫn bị giới hạn bởi hash capacity.

**Q: Flow bị evict rồi packet sau quay lại thì sao?**

A: Lookup miss và được add lại như flow mới nếu còn capacity hoặc sau replacement khác. Evict làm mất state cũ, không cấm traffic tương lai.

**Q: Có đảm bảo packet của flow bị evict vẫn được xử lý không?**

A: Packet đã vào worker ring vẫn là mbuf riêng. Nếu metadata stale, worker parse lại packet và match SPI bằng cold-data tạm. Eviction xóa state tương lai, không làm packet in-flight đọc nhầm state.

### Concurrency

**Q: RCU đã có rồi, sao cần `flow_gen`?**

A: RCU bảo vệ memory lifecycle bên trong `rte_hash`. Side arrays `hot[]/cold[]` do app sở hữu và slot có thể reuse. Packet cũ trong ring giữ `flow_idx`; nếu không có `flow_gen`, worker có thể đọc cold-data của flow mới.

**Q: Nếu flow vừa add vào hash nhưng metadata chưa fill xong bị aging quét thì sao?**

A: Dispatcher store `last_seen` cuối cùng. Maintenance paths skip slot `last_seen == 0`. Vì vậy key có thể visible nhưng chưa bị aging/pressure/replacement chọn trước khi publish xong.

**Q: `flow_gen` overflow thì sao?**

A: 32-bit generation chỉ tăng khi slot reuse, không tăng theo packet. Wrap thực tế rất xa. Code tránh generation 0 bằng cách increment tiếp. Nếu production cực dài, alternative là 64-bit generation/dynamic mbuf field.

**Q: Worker copy `cold[]` có data race không?**

A: Dispatcher có thể rewrite `cold[]` khi slot reuse. Worker check generation trước và sau copy. Nếu generation đổi, snapshot bị coi stale và worker parse packet fallback. Cách này bảo vệ semantic. Nếu cần strict C memory model hơn, có thể dùng seqlock/atomic generation protocol hoặc inflight refcount.

**Q: Victim cache candidate stale thì sao?**

A: Trước delete, code lookup lại key, kiểm tra position, generation, `last_seen`, idle condition. Candidate stale thì không delete.

### Performance/scale

**Q: Aging scan 1M entry có tốn không?**

A: Có, nên code chia thành 8 chunks. Mỗi tick scan khoảng 1/8 table. Với scale lớn hơn nhiều, timing wheel/expiry bucket là hướng tốt hơn.

**Q: Exact LRU tốt hơn victim cache không?**

A: Exact LRU chọn victim tốt hơn nhưng phải update order mỗi packet, làm hot path nặng. Victim cache dùng stats thread scan theo batch, ít ảnh hưởng hot path hơn.

**Q: Nếu workload có hơn 1M flow đều active thật sự?**

A: Replacement sẽ hy sinh state cũ để nhận flow mới, nhưng không thể quản lý hơn capacity active states. Giải pháp đúng là tăng `HASH_ENTRIES`, shard table, dùng RSS/multi-dispatcher hoặc scale node.

**Q: Worker match SPI mỗi packet có chậm không?**

A: Có chi phí tuyến tính theo số rule, nhưng SPI là phụ trợ và rule max 256. Chọn snapshot để correctness với slot reuse. Nếu SPI thành bottleneck, dùng `rte_acl` hoặc cache lại nhưng cần inflight/refcount.

### DPDK/OS/C

**Q: Vì sao dùng polling thay interrupt?**

A: Dataplane throughput cao thường dùng polling để tránh interrupt overhead và context switch. Tradeoff là tốn CPU kể cả khi ít traffic.

**Q: Hugepage dùng để làm gì?**

A: Giảm TLB miss, cấp memory physically contiguous/DPDK-friendly cho mbuf/mempool, giúp DMA/PMD hiệu quả hơn.

**Q: `volatile sig_atomic_t` có thay atomic không?**

A: Nó phù hợp cho signal handler set flag đơn giản. Nó không thay thế C11 atomic cho shared counter phức tạp.

**Q: Network byte order có được xử lý không?**

A: Packet parser giữ IP/port network byte order. Rule parser dùng `inet_pton` và `rte_cpu_to_be_16`, nên compare trực tiếp.

## 16. Kiến Thức Nên Ôn

### DPDK

- EAL là gì, lcore là gì, tại sao pin core.
- Hugepages, mempool, mbuf lifecycle.
- `rte_eth_rx_burst()`/`rte_eth_tx_burst()` semantics, partial TX.
- `rte_ring`: SP/SC vs MP/MC.
- `rte_hash`: lookup bulk, add/delete, key id, `rte_hash_max_key_id`.
- QSBR RCU: register/online/offline/quiescent/reclaim.
- PCAP PMD và `net_null` trong test.
- NUMA/socket allocation bằng `rte_zmalloc_socket`.

### Kiến trúc máy tính

- Cache line, cache locality, false sharing.
- TLB và hugepages.
- Memory ordering cơ bản: acquire/release/relaxed/seq_cst.
- Branch prediction, `likely/unlikely`.
- TSC và rate calculation.
- NUMA latency và vì sao socket-local memory quan trọng.

### Hệ điều hành

- Polling vs interrupt.
- Thread/core affinity.
- Signal handler safe/unsafe operations.
- Memory allocation và lifetime.
- File I/O trong SPI reload.
- Backpressure: ring full, TX queue full.

### C

- Struct padding/alignment/packed.
- Pointer lifetime và ownership của mbuf.
- Endianness.
- Integer overflow và generation wrap.
- `memset`, `memcpy`, `memcmp` trên struct đã zero padding.
- Atomic builtins `__atomic_load_n`, `__atomic_store_n`, `__atomic_add_fetch`.
- Undefined behavior khi đọc ngoài packet length; parser luôn check length trước.

## 17. Điểm Yếu Nên Tự Nói Trước

- Worker count fixed 4; dynamic scaling chưa có.
- IPv4 TCP/UDP only; ICMP/IPv6/VLAN chưa xử lý.
- Aging vẫn scan hash chunk, chưa dùng timing wheel.
- Replacement không phải exact LRU.
- SPI matching tuyến tính mỗi packet; nếu rule lớn nên dùng `rte_acl`.
- Chưa có metric riêng cho victim stale/drop trong ring; nếu muốn phân tích overload sâu hơn nên bổ sung sau defense.
- TUI hiện chỉ ở mức tối giản trên stdin: `show statistics`, `show worker`, `reload rule`, `help`, `quit`; chưa có dashboard/JSON API.
- PCAP tests là software-only, không thay thế benchmark NIC line-rate.

## 18. Câu Kết Thúc Khi Bảo Vệ

> Em tập trung vào phần lõi Flow Table: parse 5-tuple, lock-free lookup/add/delete với `rte_hash`, hot/cold metadata, flow affinity, aging, overload replacement và các guard chống stale slot khi packet còn in-flight. SPI Engine được giữ đơn giản để tạo policy workload cho worker. Các quyết định như `flow_gen`, cold snapshot, victim cache và chunked aging đều nhằm giữ hot path nhẹ nhưng vẫn xử lý đúng các edge case concurrency quan trọng.
