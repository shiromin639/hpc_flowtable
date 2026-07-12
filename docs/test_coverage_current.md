# Test Coverage Current

Tài liệu này mô tả từng nhóm test đang có, cách mô phỏng, mức độ tin cậy và giới hạn. Kết luận ngắn: bộ test hiện tại có giá trị regression tốt cho parser, flow lifecycle, SPI, aging, hot reload và overload path, nhưng chưa phải bằng chứng tuyệt đối cho line-rate, NIC thật, NUMA, hoặc mọi race concurrency.

## 1. Unit Tests: `tests/flowcore_unit_tests.c`

Các test này không chạy binary `flowcore` thật. Chúng gọi trực tiếp function C với packet buffer tự dựng trong memory.

| Test | Mô phỏng | Kiểm tra | Giá trị | Giới hạn |
|---|---|---|---|---|
| `test_extract_tcp_key` | Tự dựng Ethernet + IPv4 + TCP header trong `uint8_t packet[64]` | `flow_packet_extract_key()` trả OK và 5-tuple khớp src/dst IP, src/dst port, protocol | Có ý nghĩa. Bắt lỗi parser cơ bản, endian và layout key | Không chứng minh mbuf/scattered RX/NIC thật |
| `test_extract_udp_cold_and_protocol_stats` | Tự dựng UDP/53 packet | Parse sang `flow_cold_data`, gọi `flow_stats_account_protocol()` và kỳ vọng DNS tăng | Có ý nghĩa cho UDP parser và protocol accounting | Chỉ test DNS, không phủ hết TCP/UDP port |
| `test_unsupported_and_truncated_packets` | Tạo ARP, ICMP, TCP bị thiếu port, IPv4 fragment | Kỳ vọng unsupported/truncated đúng | Có ý nghĩa. Đây là negative test quan trọng để tránh đọc quá packet length | Chưa test VLAN, IPv6, IP options phức tạp, scattered mbuf |
| `test_stats_rates` | Gọi helper với counter/TSC giả | Tính elapsed, pps, Mbps, counter rollback | Có ý nghĩa cho pure math helper | Không kiểm tra stats thread thật hoặc stdout format |

Đánh giá thẳng: đây không phải “dump test”. Chúng kiểm tra function lõi có input/output rõ ràng. Nhưng chúng chỉ chứng minh logic cục bộ, không chứng minh datapath end-to-end.

## 2. Slot Lifecycle Model: `tests/flow_slot_lifecycle_tests.c`

Test này không dùng DPDK hash và không tạo thread thật. Nó mô phỏng interleaving bằng struct local.

| Test | Mô phỏng | Kiểm tra | Giá trị | Giới hạn |
|---|---|---|---|---|
| `unsafe_publication_order_has_stale_window` | Giữ `flow_gen` cũ, overwrite cold data trước | Worker vẫn accept generation cũ nhưng đọc cold data mới | Có ý nghĩa như “proof by counterexample” cho thiết kế sai | Không chạy code production thật |
| `generation_first_order_rejects_stale_packet` | Tăng `flow_gen` trước khi overwrite cold data | Worker từ chối packet cũ vì generation lệch | Có ý nghĩa để giải thích tại sao generation phải publish trước cold overwrite | Không chứng minh mọi race theo C memory model |

Đánh giá thẳng: đây là test mô hình, không phải functional proof. Dùng nó khi bảo vệ để giải thích tư duy concurrency, không dùng để khẳng định hệ thống race-free tuyệt đối.

## 3. Flow Table Overload Tests: `tests/flow_table_overload_tests.c`

Các test này chạy DPDK EAL với `--no-huge --no-pci --no-shconf --no-telemetry` và build Flow Table nhỏ bằng `-DHASH_ENTRIES=64`. Chúng gọi trực tiếp API Flow Table, không chạy dispatcher/worker thật.

| Test | Mô phỏng | Kiểm tra | Giá trị | Giới hạn |
|---|---|---|---|---|
| `test_pressure_mode_thresholds` | Lấy capacity nhỏ, tính active flow theo phần trăm | Mode chuyển đúng tại 92/96/99% | Có ý nghĩa cho policy threshold | Không test eviction thật |
| `test_unpublished_slot_is_not_evicted` | Add key vào `rte_hash` nhưng không publish `last_seen` | Aging, pressure, emergency replacement không delete slot `last_seen == 0` | Rất có ý nghĩa. Bảo vệ publication-order edge case | Synthetic; không chứng minh thread race thật |
| `test_pressure_maintenance_and_victim_cache` | Add nhiều flow published tới trên target, chạy `FLOW_PRESSURE_CRITICAL` | Có scan, hash count không tăng, victim cache có candidate, replacement xóa thêm flow | Có ý nghĩa cho pressure scan + victim cache + cached replacement | Vì critical dùng min-idle 0, không chứng minh idle-first chính xác trong workload thật |
| `test_replacement_allows_add_after_full_table` | Add flow tới khi hash nhỏ reject add, chạy emergency replacement, reclaim, rồi add flow mới | Sau evict/reclaim, flow mới add được | Rất có ý nghĩa cho full-table recovery path | Không chạy RX burst thật, không đo latency khi full |

Đánh giá thẳng: đây là nhóm test có giá trị cao nhất cho overload design. Nó không phải benchmark và không chứng minh mọi lịch trình concurrent, nhưng nó bắt được nhiều lỗi logic thật: threshold sai, delete unpublished slot, victim stale, không reclaim được sau delete.

## 4. Functional Tests: `tests/flowcore_functional_tests.c`

Đây là mentor-facing functional suite bằng C. Test tự sinh PCAP/rule file trong `/tmp`, chạy binary `build/flowcore` thật với DPDK virtual devices:

```text
RX: net_pcap0,rx_pcap=<fixture>,infinite_rx=0/1
TX: net_null0
lcores: 0-5
```

Sau đó test đọc stdout, strip ANSI và parse telemetry counters.

| Test | Mô phỏng | Kiểm tra | Giá trị | Giới hạn |
|---|---|---|---|---|
| `same_flow_reuse` | PCAP 10 packet TCP/80 cùng 5-tuple | `Created Flows = 1`, `SPI Forwarded = 10`, `SPI Drops = 0` | Có ý nghĩa end-to-end cho flow reuse/affinity cơ bản | Không kiểm tra packet ordering tại TX vì dùng `net_null` |
| `unique_flow_creation` | PCAP 32 packet UDP khác 5-tuple | `Created Flows = 32`, `SPI Forwarded = 32` | Có ý nghĩa cho flow creation path | Không test hash collision sâu hoặc overload |
| `rule_drop_ssh` | PCAP 6 packet gồm TCP/22 và các port khác | SSH bị drop: forwarded 5, drops 1 | Có ý nghĩa cho SPI DROP rule | Chỉ test một rule drop đơn giản |
| `protocol_accounting` | Cùng mixed PCAP: HTTP, HTTPS, DNS, TCP khác, UDP khác | Counters HTTP=1, HTTPS=1, DNS=1, TCP=2, UDP=1, OTHER=0 | Có ý nghĩa cho worker protocol accounting | OTHER thường không tăng vì dispatcher lọc non-TCP/UDP |
| `aging_cleanup` | PCAP một TCP/80 packet, runtime 7 giây | Created 1, Deleted >= 1 | Có ý nghĩa cho aging end-to-end vì timeout hiện 5 giây | Phụ thuộc thời gian chạy và stdout tick; không đo chính xác latency timeout |
| `non_tcp_udp_filtered` | PCAP ICMP + ARP | Created 0, RX Filtered 2, Forwarded 0, Drops 0 | Có ý nghĩa cho dispatcher filter path | Không test IPv6/VLAN |
| `hot_reload` | Infinite PCAP TCP/8443; phase 1 allow, phase 2 sửa rule thành drop, gửi `SIGUSR1` | Reload log xuất hiện, rule version >= 2, có forwarded trước reload và drops sau reload | Có ý nghĩa cho runtime reload thật | Không chứng minh zero-packet-loss hoặc atomicity tuyệt đối tại thời điểm reload |

Đánh giá thẳng: đây không phải “dump test”. Nó chạy binary thật qua DPDK vdev, nên có giá trị functional end-to-end. Nhưng vì TX là `net_null` và dữ liệu được xác minh qua stdout counters, nó không chứng minh payload/header TX đúng, line-rate NIC thật, hoặc mọi race concurrency.

## 5. Python Functional Runner: `tests/run_functional_tests.py`

Python runner chạy các kịch bản tương tự C functional suite bằng helper `flowcore_test_lib.py`. Nó tiện cho dev nhanh, còn C suite phù hợp hơn khi demo với mentor vì self-contained hơn.

| Test | Tương đương C | Điểm khác |
|---|---|---|
| `test_same_flow_reuse` | `same_flow_reuse` | Dùng Python fixture generator và regex parser |
| `test_unique_flow_creation` | `unique_flow_creation` | Same semantics |
| `test_rule_drop_ssh` | `rule_drop_ssh` | Same semantics |
| `test_protocol_accounting` | `protocol_accounting` | Same semantics |
| `test_aging_cleanup` | `aging_cleanup` | Same semantics |
| `test_non_tcp_udp_filtered` | `non_tcp_udp_filtered` | Same semantics |
| `test_hot_reload` | `hot_reload` | Python dễ chỉnh runtime/reload timing hơn |

Đánh giá thẳng: hữu ích cho automation, nhưng không thêm coverage logic lớn so với C suite. Nếu thời gian defense ít, ưu tiên hiểu C suite và chỉ nói Python runner là workflow tiện hơn.

## 6. Performance Smoke: `tests/run_performance_tests.py`

Các scenario:

- `baseline_fixed_256`
- `flow_creation_20k`
- `rule_mix_20k`
- `aging_smoke`

Mục tiêu là chạy binary thật với PCAP/null vdev và xuất JSON metrics như created/deleted/forwarded/dropped/rule_checks/protocols.

Đánh giá thẳng: đây là smoke/regression performance, không phải benchmark khoa học. Nó giúp phát hiện tụt counters hoặc crash khi traffic lớn hơn functional tests, nhưng không chứng minh throughput trên NIC thật. Khi trình bày benchmark, phải nói rõ môi trường PCAP PMD/net_null khác với traffic generator vật lý.

## 7. Những gì test hiện chưa bảo đảm

- Chưa kiểm tra line-rate với NIC thật, RSS thật, multi-queue RX thật.
- Chưa có stress test race deterministic giữa worker ring backlog, aging delete và slot reuse.
- Chưa có sanitizer/thread sanitizer cho C/DPDK path.
- Chưa verify packet output payload/header vì functional tests dùng `net_null`.
- Chưa test IPv6, VLAN, jumbo/scattered mbuf, TCP fragments hợp lệ.
- Chưa test rule set lớn 256 rule ở hiệu năng cao.
- Chưa test NUMA locality hoặc nhiều socket.
- Chưa có exact latency test cho aging/replacement.

## 8. Cách trả lời khi mentor hỏi test có “bias” không

Câu trả lời nên thẳng:

> Bộ test của em không chứng minh hệ thống đúng tuyệt đối trong mọi điều kiện production. Unit tests chứng minh parser/helper cục bộ. Overload tests chứng minh các edge case Flow Table quan trọng với `rte_hash` thật nhưng hash nhỏ. Functional tests chạy binary thật bằng PCAP PMD và kiểm tra counters end-to-end, nên có giá trị regression tốt. Tuy nhiên, vì dùng `net_null`, stdout parsing và môi trường software-only, chúng chưa thay thế benchmark với NIC thật, traffic generator và stress race. Em xem đây là bằng chứng chức năng và regression, không phải bằng chứng line-rate production.
