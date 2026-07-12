# FlowCore Current Optimization Notes

Tài liệu này giải thích các kỹ thuật tối ưu chính đang thật sự xuất hiện trong code. Các kỹ thuật nhỏ như "mỗi worker một TX queue" hoặc "mỗi worker một SP/SC ring" được nhắc trong kiến trúc, nhưng không phải trọng tâm ở đây.

## 1. Nguyên tắc tối ưu chung

FlowCore tối ưu theo hướng data plane:

- Nhận/gửi packet theo burst để giảm chi phí trên mỗi packet.
- Giữ hot path ngắn.
- Tránh global lock.
- Tránh parse lại packet nếu dispatcher đã parse.
- Tránh nhiều core cùng ghi vào một cache line.
- Đẩy việc ít xảy ra hoặc tốn chi phí sang slow path: flow creation, aging, RCU reclaim, SPI reload.
- Dùng DPDK primitive thay vì tự viết concurrent hash/ring.

Hot path phổ biến nhất:

```text
dispatcher RX burst
  -> parse key
  -> rte_hash_lookup_bulk hit
  -> update last_seen
  -> write flow_idx/flow_gen to mbuf
  -> enqueue to worker ring
  -> worker validates generation
  -> worker snapshots cold metadata
  -> match SPI on snapshot
  -> TX burst
```

## 2. Bulk packet processing

DPDK API được dùng theo burst:

- `rte_eth_rx_burst()`
- `rte_hash_lookup_bulk()`
- `rte_ring_enqueue_burst()`
- `rte_ring_dequeue_burst()`
- `rte_eth_tx_burst()`

Lý do:

- Một lần gọi API xử lý nhiều packet.
- Tận dụng locality của stack arrays như `keys[]`, `positions[]`, `worker_buffers[][]`.
- Giảm overhead vòng lặp và function call trên từng packet.

Tradeoff:

- Burst lớn có thể tăng latency tail vì packet chờ gom batch.
- Burst nhỏ giảm latency nhưng giảm throughput.
- `BURST_SIZE = 32` là lựa chọn thực dụng cho bài mini project; muốn tối ưu thật cần benchmark theo workload.

Alternative:

- Dùng `BURST_SIZE = 64` hoặc 128 nếu NIC/CPU phù hợp.
- Dùng adaptive burst hoặc timeout-based flush nếu quan tâm latency.

## 3. Lock-free `rte_hash` + QSBR RCU

Flow table dùng DPDK `rte_hash` với flag:

```c
RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF
```

và gắn QSBR RCU:

```c
rte_hash_rcu_qsbr_add(hash, &rcu_cfg)
```

Mục tiêu:

- Dispatcher lookup/add.
- Stats/aging delete expired entries.
- Không dùng một global mutex/rwlock quanh mọi lookup.
- Cho phép delete an toàn khi reader có thể đang ở gần hash entry.

QSBR nghĩa là reader phải báo "quiescent state". Trong code:

- Dispatcher register QSBR và gọi quiescent sau mỗi burst hoặc khi không nhận được packet.
- Stats/aging register QSBR, offline trong lúc sleep, online lại khi làm việc, quiescent sau aging tick.

RCU bảo vệ gì:

- Vòng đời internal key/data bên trong `rte_hash` sau khi delete.
- Cho hash biết khi nào an toàn để reclaim entry đã defer.

RCU không bảo vệ gì:

- Ý nghĩa semantic của `hot[flow_idx]` và `cold[flow_idx]`.
- Nếu slot side-array được reuse, RCU không tự biết packet cũ trong ring vẫn cầm `flow_idx`.
- Vì vậy code cần `flow_gen`.

Tradeoff:

- Phải gọi quiescent đúng. Nếu reader không quiescent, reclaim có thể bị trì hoãn.
- Debug concurrency khó hơn lock.
- RCU không thay thế mọi synchronization; side-array vẫn cần design ownership/generation rõ ràng.

Alternative:

- Global lock: dễ hiểu nhưng bottleneck nghiêm trọng.
- Per-bucket lock: dễ hơn RCU nhưng lookup vẫn chịu lock/cache contention.
- Per-worker flow table: giảm share nhưng flow ownership/routing phức tạp hơn, nhất là khi dispatcher cần lookup trước khi biết worker.
- Custom lock-free hash: rủi ro cao, khó hơn dùng DPDK primitive đã test.

## 4. Hot/cold metadata split

Flow metadata không nằm trong một struct lớn duy nhất. Code dùng:

```text
hot[flow_idx]
cold[flow_idx]
```

`hot[]` chứa:

- `last_seen`
- `flow_gen`
- `worker_id`

`cold[]` chứa:

- `src_ip`
- `dst_ip`
- `src_port`
- `dst_port`
- `protocol`
- `create_time`

Lý do:

- Dispatcher hit path cần `worker_id`, `last_seen`, `flow_gen`.
- Worker cần generation để xác định có thể dùng snapshot `cold[]` hay phải parse lại packet.
- IP/port/create-time không cần thiết cho dispatcher hit path.
- Tách hot/cold giảm khả năng kéo dữ liệu lạnh vào cache khi không cần.

Tradeoff:

- Có thêm một cấp quản lý side arrays.
- Cần đảm bảo `flow_idx < storage_entries`.
- Cần generation để chống stale metadata.
- Hot/cold hiện chưa padding mỗi entry ra cache line; điều này tốt cho memory density nhưng có thể có false sharing giữa adjacent flows.

Alternative:

- Một struct flow entry duy nhất: đơn giản hơn nhưng dispatcher có thể kéo cả cold fields vào cache.
- Cache-line-align mỗi flow entry: giảm false sharing nhưng tốn nhiều RAM ở 1M+ flows.
- Per-worker flow arrays: tốt cho locality nếu worker sở hữu toàn bộ flow, nhưng dispatcher vẫn cần routing/lookup nhất quán.

## 5. Mbuf metadata shortcut

Dispatcher ghi kết quả lookup vào mbuf:

```c
m->hash.fdir.lo = (uint32_t)flow_idx;
m->hash.fdir.hi = flow_gen;
```

Worker đọc lại:

```c
uint32_t flow_idx = pkts[i]->hash.fdir.lo;
uint32_t flow_gen = pkts[i]->hash.fdir.hi;
```

Lý do:

- Dispatcher đã parse packet và lookup hash.
- Worker không cần parse lại và không cần lookup hash lần nữa.
- Chỉ truyền con trỏ mbuf qua ring, không copy payload.

Tradeoff:

- Tận dụng field `hash.fdir` của `rte_mbuf` theo mục đích nội bộ app; cần đảm bảo không có PMD/feature khác cần field này ở giai đoạn sau.
- Metadata có thể stale nếu flow bị delete/reuse trước khi worker xử lý.
- Vì vậy phải có `flow_gen` và fallback parse.

Alternative:

- Attach custom dynamic field vào mbuf bằng DPDK dynamic mbuf fields: rõ nghĩa hơn nhưng thêm setup.
- Tạo wrapper object chứa mbuf + flow metadata: rõ ràng nhưng thêm allocation hoặc pool.
- Worker lookup hash lại: đơn giản về metadata nhưng tốn CPU và tăng hash pressure.

## 6. Flow generation check

Problem:

1. Dispatcher enqueue packet của flow A với `flow_idx = 10`.
2. Packet còn nằm trong worker ring.
3. Aging xóa flow A.
4. Hash slot 10 được flow B dùng lại.
5. Worker xử lý packet cũ, nếu chỉ nhìn `flow_idx = 10` sẽ đọc nhầm metadata của flow B.

Solution:

- Mỗi slot có `flow_gen`.
- Khi slot được dùng cho flow mới, dispatcher tăng `flow_gen`.
- Dispatcher ghi generation vào mbuf.
- Worker so sánh generation trong mbuf với generation hiện tại trong `hot[]`.

Nếu khớp:

- Worker copy `cold[]` sang biến cục bộ.
- Worker kiểm tra lại `flow_gen` sau khi copy.
- Nếu generation vẫn khớp, worker match SPI trên snapshot này.

Nếu lệch:

- Worker parse packet lại sang `flow_cold_data` tạm.
- Worker match SPI trực tiếp bằng cold tạm.

Tradeoff:

- Thêm 32-bit generation và atomic load/increment.
- Snapshot và double-check thêm một lần load/copy nhỏ, nhưng tránh race khi slot bị reuse ngay lúc worker đang xử lý.
- Fallback parse hiếm nhưng cần tồn tại để đúng.
- Nếu generation wrap về 0, code tránh generation 0 bằng cách increment tiếp.

Alternative:

- Không reuse slot cho tới khi chắc ring đã drain: khó và tốn memory.
- Reference counting per flow: chính xác hơn nhưng tăng atomic hot-path.
- Copy cold metadata vào mbuf/wrapper: đúng nhưng tăng per-packet memory/copy.

Publication guard:

- Khi dispatcher tạo flow mới, key có thể đã được add vào hash trước khi toàn bộ `hot[]/cold[]` được fill xong.
- Vì vậy `last_seen == 0` được dùng như trạng thái "slot chưa publish".
- Dispatcher ghi `cold[]`, `flow_gen`, `worker_id` trước, rồi store `last_seen` cuối cùng.
- Aging, pressure maintenance, cached victim deletion và emergency replacement đều skip slot có `last_seen == 0`.

Tradeoff của guard này là `last_seen` vừa là timestamp vừa là publication marker, nên code phải giữ quy ước rõ ràng: timestamp hợp lệ không được là 0. Vì `rte_get_tsc_cycles()` trong runtime bình thường tăng rất nhanh sau init, quy ước này thực dụng cho project. Alternative sạch hơn là thêm field `published` riêng hoặc packed state atomic, nhưng sẽ tăng kích thước hot entry và số thao tác trên hot path.

Điểm cần nói rõ khi bảo vệ là hot/cold split không tự giải quyết race slot-reuse. Nó chỉ giảm dữ liệu chạm vào trên fast path. Tính đúng của concurrent reuse đến từ `flow_gen`, publication order và fallback parse khi generation không khớp.

## 7. SPI rule matching bằng cold snapshot

SPI match tuyến tính qua rule list hiện được thực hiện trên `flow_cold_data` snapshot của packet/flow.

Worker logic:

1. Lấy `flow_idx/flow_gen` từ mbuf.
2. Nếu generation khớp, copy `cold[flow_idx]` sang biến cục bộ.
3. Load lại `flow_gen`; nếu generation đổi trong lúc copy, parse lại packet.
4. Gọi `spi_rule_engine_match_cold()` trên snapshot hoặc cold data tạm.

Lý do:

- Slot `hot[]/cold[]` có thể bị aging/replacement delete rồi reuse trong lúc packet cũ vẫn nằm trong worker ring.
- Nếu worker vừa kiểm tra generation xong đã dùng trực tiếp con trỏ `cold[]`, dispatcher có thể reuse slot ngay sau đó.
- Snapshot + double-check đảm bảo packet được xử lý theo metadata nhất quán, hoặc fallback parse packet nếu metadata stale.

Tradeoff:

- Worker phải match rule trên mỗi packet, thay vì cache action theo flow.
- Với `SPI_MAX_RULES = 256`, chi phí này vẫn chấp nhận được cho mini project và đổi lại concurrency an toàn hơn.
- Nếu rule list rất lớn, linear match vẫn là giới hạn.

Một giới hạn khác là parser hiện làm việc trên dữ liệu đầu của mbuf và chưa xử lý đầy đủ fragmented/scattered RX. Với traffic nhỏ từ PCAP PMD hoặc NIC path không scatter, giới hạn này chấp nhận được; nếu mở rộng sang jumbo/scattered path thì parser cần được thiết kế lại theo `rte_pktmbuf_read()` hoặc logic tương đương.

Alternative:

- Per-flow SPI cache: nhanh hơn nhưng cần cơ chế pin/refcount hoặc atomic packed state để không ghi cache nhầm vào slot đã reuse.
- Eager invalidate toàn bộ flow khi reload: reload chậm, tốn scan 1M flows.
- `rte_acl`: phù hợp rule set lớn, phức tạp hơn về build table và memory.
- Hash exact-match rule: nhanh nếu rule chủ yếu exact; wildcard phức tạp hơn.

## 8. Double-buffered rule reload

Rule reload dùng hai bảng:

```text
g_rule_tables[0]
g_rule_tables[1]
g_active_rules -> một trong hai bảng
```

Reload flow:

1. `SIGUSR1` set reload flag.
2. Stats thread phát hiện flag.
3. Load file vào bảng inactive.
4. Set version mới.
5. Atomic store active pointer sang bảng mới.

Lý do:

- Worker đang đọc bảng active cũ vẫn thấy bảng hợp lệ.
- Không cần lock lớn quanh worker.
- Nếu reload fail, giữ bảng cũ.

Tradeoff:

- Chỉ có hai bảng; cần đảm bảo không free bảng đang có reader. Vì bảng là static array nên không bị free, an toàn hơn.
- Reload do stats thread xử lý nên delay tối đa tới tick kế tiếp.
- Rule parse lỗi sẽ giữ version cũ, cần log rõ.

Alternative:

- Mutex quanh rule table: dễ nhưng làm worker hot path có lock.
- RCU rule table heap allocation: linh hoạt hơn nhưng cần reclaim lifecycle.
- CLI/control socket reload trực tiếp: tiện hơn nhưng thêm thread/control plane.

## 9. Chunked aging and batch delete

Flow aging không scan toàn bộ hash mỗi lần in stats. Nó dùng:

- `AGING_NUM_CHUNKS = 8`
- `AGING_INTERVAL_US = 1000000 / 8`
- persistent `aging_iter`
- scan budget khoảng `storage_entries / 8`
- expired key batch size `1024`

Lý do:

- Full scan 1M entries mỗi giây có thể tạo CPU spike.
- Chia nhỏ scan làm chi phí đều hơn.
- Batch delete giảm overhead khi nhiều flow hết hạn.

Correctness detail:

- Khi phát hiện expired, trước khi delete code re-load `last_seen`.
- Nếu packet mới vừa update `last_seen`, delete bị bỏ qua.

Tradeoff:

- Aging vẫn là hash iteration, chi phí phụ thuộc capacity/table occupancy.
- Flow timeout không chính xác tuyệt đối tại đúng 5 giây; có độ trễ theo chu kỳ scan.
- Với 10M flows, hash scan có thể trở thành bottleneck.

Alternative:

- Timing wheel: aging cost gần số flow hết hạn, nhưng mỗi packet update bucket phức tạp hoặc cần lazy bucket.
- Min-heap theo expiry: delete chính xác hơn nhưng update mỗi packet đắt.
- Per-worker aging partition: tốt nếu flow table partition theo worker.
- Approximate idle buckets: giảm update hot path nhưng cần xử lý stale bucket entries.

## 10. Per-lcore stats

Stats dùng `rte_lcore_var`, mỗi lcore có `struct lcore_stats` riêng.

Lý do:

- Dispatcher ghi RX counters.
- Worker ghi TX/SPI/protocol counters.
- Stats thread đọc định kỳ.
- Không cần mọi packet atomic increment vào global counter.

Tradeoff:

- Snapshot không tuyệt đối nhất quán tại từng cycle.
- Stats thread có thể đọc khi lcore đang update.
- Với telemetry, approximate consistency chấp nhận được.

Alternative:

- Global atomic counters: đơn giản nhưng tốn atomic/cache bouncing.
- Per-worker arrays manually aligned: được, nhưng `rte_lcore_var` phù hợp DPDK hơn.
- Lock around stats: không phù hợp hot path.

## 11. Stateful overload replacement

FlowCore hiện có thêm cơ chế overload replacement để xử lý trường hợp flow mới vượt sức chứa hiện tại của Flow Table. Mục tiêu không phải làm hash table 1M chứa được hơn 1M active states cùng lúc. Mục tiêu là khi bảng gần đầy, hệ thống thay flow cũ bằng flow mới theo policy stateful, để packet của flow mới vẫn được xử lý bằng một Flow Table entry.

Các thành phần chính:

- Watermark trong `common.h`: pressure 92%, aggressive 96%, critical 99%, target sau evict là 92%.
- Stats/aging thread chạy `flow_table_pressure_maintenance()` khi active flow vượt watermark.
- Victim cache là `rte_ring` SP/SC nội bộ. Khi enqueue, code pack `position` và `flow_gen` vào một object 64 bit; khi dequeue, key được dựng lại từ `cold[]` rồi validate lại bằng key/position/generation/`last_seen`.
- Dispatcher dùng `flow_table_evict_for_replacement()` khi flow mới add fail.
- Dispatcher retry `rte_hash_add_key()` sau khi evict victim và reclaim một phần.
- Counters mới theo dõi attempts/success/failures/victim evictions/retry add.
- Trước khi xóa victim, code lookup lại key và kiểm tra position/generation/`last_seen != 0`; nếu slot đã đổi chủ hoặc chưa publish xong thì bỏ candidate đó.

Vì sao không dùng stateless fallback:

- Requirement của project là quản lý Flow Table, flow lifecycle và flow affinity.
- Packet của flow mới nên được đưa vào Flow Table nếu hệ thống còn có thể thay flow cũ.
- Stateless fallback sẽ làm packet được xử lý nhưng không có `created_flow`, `last_seen`, `worker_id` hay timeout lifecycle cho flow đó.

Vì sao dispatcher không scan nặng:

- Dispatcher chỉ evict trong cold path của flow mới.
- Candidate chủ yếu do stats thread chuẩn bị trước trong victim cache.
- Nếu cache rỗng, dispatcher chỉ chạy emergency bounded scan với budget nhỏ, không scan toàn hash.

Tradeoff:

- Flow mới được ưu tiên, nhưng flow cũ có thể mất state.
- Nếu flow cũ bị evict rồi packet của nó quay lại, nó sẽ lookup miss và được add lại như new flow.
- Nếu tất cả hơn 1M flow đều active, replacement có thể gây churn/thrashing. Khi đó cần tăng `HASH_ENTRIES`, shard table hoặc thêm RSS/multi-dispatcher partitioning.
- RCU delete không đồng nghĩa slot reusable ngay lập tức, nên stats thread phải giữ free reserve trước khi full.

Alternative:

- Drop new flow: đơn giản nhưng không đáp ứng yêu cầu "flow mới phải nhận".
- Exact LRU: chọn victim tốt hơn nhưng cần update LRU trên mọi packet, không hợp hot path.
- Random eviction: rẻ nhưng có thể evict flow active nóng.
- Timing wheel/lazy expiry bucket: tốt hơn cho scale lớn, nhưng phức tạp hơn và cần xử lý stale entries.

## 12. What is not optimized yet

Các điểm có thể bị hội đồng hỏi:

- SPI match vẫn linear.
- Aging vẫn hash-scan based.
- Không có RSS/multi-dispatcher.
- Không có per-NUMA partition thật.
- Fixed worker count.
- Mbuf metadata dùng field có sẵn thay vì dynamic field.
- Không có JSON stats/export mode.
- Không benchmark với NIC thật.
- Replacement hiện là idle-first/scan-based approximation, chưa phải timing wheel hoặc exact LRU.

Các điểm này nên được trình bày là giới hạn đã biết và có hướng cải tiến, không nên nói là hoàn hảo.
