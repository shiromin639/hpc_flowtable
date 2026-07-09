# Thiết Kế Kiến Trúc Nâng Cao: SPI Rule Engine, Worker Statistics & Hot Reload

Tài liệu này mô tả chi tiết thiết kế kỹ thuật (Design Architecture) để triển khai các tính năng bắt buộc và nâng cao trong Mini Project Flowtable DPDK, đảm bảo hệ thống giữ được ngưỡng Throughput chục triệu PPS mà không bị nghẽn (bottleneck).

---

## 1. Zero-Parsing Payload (Truyền Dữ Liệu Tốc Độ Cao)

**Vấn đề:** Dispatcher đã mất công phân tích (parse) gói tin để lấy bộ 5 tham số (5-tuple). Khi sang Worker, nếu Worker lại phân tích gói tin lần nữa để tìm IP/Port đem so khớp với Rule thì sẽ tiêu tốn CPU vô ích.

**Giải pháp:** Tận dụng không gian bộ nhớ Metadata đính kèm của DPDK `rte_mbuf` và kiến trúc `cold_data`.

*   **Tại Dispatcher:**
    Khi tra cứu xong Flow Table, ta thu được `flow_idx`. Gắn ID này thẳng vào gói tin trước khi đẩy vào Ring:
    ```c
    m->hash.usr = flow_idx; // Gắn ID luồng vào metadata của mbuf
    ```
*   **Tại Worker:**
    Thay vì parse header, Worker chỉ việc lôi `flow_idx` ra và truy cập mảng `cold_data` (nơi đã lưu sẵn 5-tuple từ lúc Dispatcher tạo luồng):
    ```c
    uint32_t flow_idx = m->hash.usr;
    struct flow_cold_data *cold = &ft->cold[flow_idx];
    
    // Sử dụng cold->src_ip, cold->dst_port... để check Rules hoặc đếm Stats
    ```

---

## 2. Thống Kê Các Loại Packet (Worker Statistics)

**Vấn đề:** Việc cập nhật các biến toàn cục (Global Variables) như `total_tcp`, `total_http` từ 4 Worker đồng thời sẽ gây ra tranh chấp bộ nhớ (Cache-line bouncing / Lock contention), làm sụt giảm nghiêm trọng tốc độ.

**Giải pháp:** Thiết kế mảng cục bộ (Local Counters).

1.  Tạo một struct chứa số liệu thống kê (HTTP, HTTPS, DNS, TCP, UDP, OTHER) và tạo thành một mảng có độ dài bằng số lượng Worker.
    ```c
    struct protocol_stats {
        uint64_t http_pkts;
        uint64_t https_pkts;
        uint64_t dns_pkts;
        uint64_t total_tcp;
        uint64_t total_udp;
        uint64_t other;
    } __rte_cache_aligned;
    
    struct protocol_stats worker_proto_stats[NUM_WORKERS];
    ```
2.  Mỗi Worker chỉ được phép cộng dồn (++) vào đúng index của riêng nó: `worker_proto_stats[lcore_id - offset].http_pkts++`.
3.  Thread In Thống Kê (`stats.c`) cứ mỗi giây sẽ đi vòng quanh mảng này, cộng gộp lại và in ra màn hình. Tuyệt đối không dùng Lock.

---

## 3. SPI Rule Engine (Fast-Path & Slow-Path Separation)

**Vấn đề:** So khớp mẫu (Pattern Matching) 5-tuple với một danh sách gồm hàng trăm Rules là thao tác cực tốn CPU. Không thể làm việc này cho mọi gói tin.

**Giải pháp:** Áp dụng nguyên lý Caching (Chỉ check 1 lần duy nhất cho mỗi luồng).

*   Thêm trường `spi_action` vào `struct flow_hot_data`. Khởi tạo mặc định là `ACTION_UNKNOWN`.
*   **Slow-Path (Gói tin đầu tiên):**
    Worker kiểm tra thấy `hot[flow_idx].spi_action == ACTION_UNKNOWN`. Worker tiến hành chạy vòng lặp so khớp 5-tuple với bảng Rule. Trả ra kết quả là `FORWARD` hay `DROP`, lưu kết quả này đè lên `spi_action`.
*   **Fast-Path (Các gói tin tiếp theo của luồng):**
    Worker nhìn vào `spi_action`, thấy đã là `FORWARD` hoặc `DROP`, liền lập tức thực thi hành động mà bỏ qua hoàn toàn bước check Rule. Thao tác tốn O(1) độ phức tạp.

---

## 4. Tính Năng Hot Reload Runtime (Zero-Downtime Rule Update)

**Vấn đề:** Thay đổi danh sách luật (reload từ file `rules.cfg`) trong khi hệ thống đang chạy chục triệu gói tin mỗi giây mà không làm chết ứng dụng.

**Giải pháp Kiến trúc:** Kết hợp **Double Buffering** và **Lazy Invalidation**.

### Bước 4.1: Double Buffering cho Bảng Luật
*   Tạo 2 bảng chứa Rule: `RuleTable A` và `RuleTable B`.
*   Sử dụng một con trỏ toàn cục `struct rule_table *active_rules = &A;`
*   Khi có lệnh Reload (qua SIGUSR1 hoặc CLI), Thread quản lý đọc file và nạp vào bảng đang rảnh (Bảng `B`).
*   Nạp xong, đổi con trỏ bằng một phép toán nguyên tử (Atomic):
    ```c
    __atomic_store_n(&active_rules, &B, __ATOMIC_RELEASE);
    ```
*   Tất cả Worker ngay lập tức trỏ sang dùng Bảng Luật mới mượt mà.

### Bước 4.2: Lazy Invalidation (Cập nhật cho luồng đang chạy)
Làm sao để luồng cũ biết luật đã đổi để tính toán lại `spi_action`? 
*   Dùng kỹ thuật Versioning: Thêm biến toàn cục `uint32_t global_rule_version;`. Mỗi lần reload, tăng biến này lên 1.
*   Cập nhật `hot_data`:
    ```c
    struct flow_hot_data {
        uint64_t last_seen;
        uint8_t  worker_id;
        uint8_t  spi_action;
        uint32_t action_version; // <--- Thêm biến này
    } __rte_cache_aligned;
    ```
*   **Logic thông minh ở Worker:**
    Khi xử lý gói tin, Worker luôn kiểm tra Version:
    ```c
    if (ft->hot[flow_idx].action_version != global_rule_version) {
        // Cache lỗi thời! Check lại rule mới và lưu lại kết quả
        ft->hot[flow_idx].spi_action = match_spi_rules(active_rules, cold_data);
        ft->hot[flow_idx].action_version = global_rule_version;
    }
    // Thực thi Fast-path với spi_action
    ```
*   Nhờ cách này, hệ thống không cần mất công dọn dẹp hàng triệu luồng cũ khi reload. Các luồng sẽ tự động "lười biếng" cập nhật khi và chỉ khi có gói tin mới của luồng đó đi vào Worker.
