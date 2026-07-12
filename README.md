# Flowcore DPDK - Quick Review

Project này là một datapath DPDK theo kiểu **stateful flow table**. Ý chính là hệ thống không chỉ lọc gói tin, mà còn tạo, giữ, cập nhật và xóa flow trong runtime. SPI là phần policy gắn thêm lên trên flow table đó.

- `include/flow_table.h`: định nghĩa `ipv4_5tuple_key`, `flow_hot_data`, `flow_cold_data`, `flow_table_ctx`
- `include/common.h`: các constant như `NUM_WORKERS`, `BURST_SIZE`, `HASH_ENTRIES`, timeout aging, ring, port...
- `include/spi_engine.h`, `include/stats.h`, `include/app_threads.h`: interface cho SPI, stats và thread

Các file `.c` chính:

- `src/dispatcher.c`: nhận packet từ RX, parse key, lookup flow table, tạo flow mới nếu cần, rồi route packet sang worker
- `src/worker.c`: đọc packet từ ring, áp SPI rule, rồi forward/drop
- `src/flow_table.c`: quản lý hash table, hot/cold flow data, RCU/QSBR, aging
- `src/stats.c`: chạy stats định kỳ, đồng thời gọi aging và reclaim
- `src/spi_engine.c`: parse rules, match rules, reload ruleset
- `src/app_init.c`: init EAL, port, mempool, ring, launch thread

Kiến trúc tổng thể của project là pipeline:

```text
RX -> Dispatcher -> Flow lookup/create -> Worker ring -> Worker -> SPI -> TX
```

Dispatcher nhận burst packet từ `PORT_IN`, parse IPv4/TCP/UDP để lấy 5-tuple, rồi gọi `rte_hash_lookup_bulk()` để lookup nhiều flow cùng lúc. Nếu flow đã có thì lấy `flow_idx`, update `last_seen`, đọc `worker_id` và đẩy packet vào đúng worker ring. Nếu flow chưa có thì thêm flow mới vào hash table, khởi tạo metadata hot/cold, gán worker bằng hash của toàn bộ 5-tuple modulo số worker, rồi route packet sang worker đó.

Trong dispatcher có thêm các mảng `resolved_indices`, `resolved_positions`, `resolved_workers`. Phần này dùng để xử lý trường hợp trong cùng một burst có nhiều packet thuộc cùng một flow mới. Vì `lookup_bulk()` chạy trước, nên có thể cả 2 packet đều thấy miss. Packet đầu tiên sẽ tạo flow, packet sau thì reuse kết quả vừa resolve thay vì add lại lần nữa.

Flow table dùng `rte_hash` để map từ `5-tuple key -> flow_idx`. Hash table giữ index, còn metadata thực của flow được giữ trong 2 mảng:

- `hot[flow_idx]`
- `cold[flow_idx]`

`hot data` chứa các field hay đụng trên datapath như `last_seen`, `flow_gen`, `worker_id`. `cold data` chứa các field mô tả flow như `src/dst ip`, `src/dst port`, `protocol`, `create_time`. Ý tưởng là dispatcher và aging chủ yếu đụng `hot`, còn worker/SPI mới cần `cold`. `flow_gen` được dùng để phát hiện trường hợp `flow_idx` cũ đã bị reuse cho flow khác; worker copy `cold data` thành snapshot trước khi match SPI để tránh đọc nhầm khi slot bị reuse.

Project dùng `rte_hash` ở mode lock-free (`RW_CONCURRENCY_LF`) và gắn thêm `RCU/QSBR`. Mục đích là để aging có thể xóa flow trong khi dispatcher/worker vẫn đang chạy mà không cần global lock. Các thread reader sẽ register vào QSBR và báo `quiescent state` định kỳ. Nhờ đó việc reclaim entry sau khi delete sẽ an toàn hơn.

Flow aging chạy trong `stats_thread()`. Mỗi flow có `last_seen`, nếu quá `FLOW_TIMEOUT_SEC` thì sẽ bị xóa. Aging dùng iterator tăng dần và scan theo budget mỗi tick, thay vì restart từ đầu hash table mỗi lần. Key nào expired sẽ được gom batch rồi delete, sau đó QSBR reclaim dần.

Stats của project được làm theo kiểu **per-lcore counters** bằng `rte_lcore_var`. Mỗi lcore có `lcore_stats` riêng, không dùng một global counter chung cho tất cả thread. Sau đó `stats_thread()` định kỳ cộng dồn lại, tính rate theo thời gian TSC thực tế và in ra:

- RX/TX throughput
- active flows
- số flow tạo / xóa
- SPI drop / TX drop
- filtered RX / ring drop / hash add failure
- aging scanned / expired / deleted / reclaimed
- số rule đang active
- protocol counters
- chi tiết từng worker

Telemetry hiện tại tách rõ:

- `Deleted Flows`: tổng flow bị xóa
- `Timeout Delete`: số flow bị timeout và delete thành công
- `Pressure Evict`: số flow bị evict do overload/replacement
- `SPI Forwarded | Rule Checks`: số packet forward và số lần worker chạy SPI check

Giới hạn cần nói rõ khi bảo vệ:

- parser hiện chỉ xử lý IPv4 TCP/UDP trên segment đầu tiên của mbuf
- không có xử lý đầy đủ cho IPv4 fragment hoặc scattered RX/jumbo path
- ứng dụng hiện yêu cầu tối thiểu 2 DPDK ports (`PORT_IN = 0`, `PORT_OUT = 1`)
