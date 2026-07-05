-- =============================================================================
-- Pktgen Lua Script: Dynamic Flow Traffic Generator
-- Mục đích: Sinh traffic động liên tục với IP/Port thay đổi để test flow
--           creation/deletion trong flowcore. Không dùng pcap file tĩnh.
-- =============================================================================
-- Cách dùng:
--   Trong CLI pktgen: Pktgen> load /path/to/pktgen_dynamic_traffic.lua
--   Hoặc qua tham số: pktgen ... -- ... -f pktgen_dynamic_traffic.lua
-- =============================================================================

package.path = package.path .. ";?.lua;test/?.lua;app/?.lua;../?.lua;/home/kinosaki-mei/Pktgen-DPDK/?.lua"

require "Pktgen"

-- ======================== CẤU HÌNH ========================
local SEND_PORT    = "0"          -- Port ID gửi traffic
local DURATION     = 60           -- Thời gian test (giây)
local PKT_SIZE     = 64           -- Kích thước gói tin (bytes)
local TX_RATE      = 100          -- % tốc độ line rate 

-- Dải IP nguồn (tạo nhiều flow khác nhau)
local SRC_IP_START = "10.0.0.1"
local SRC_IP_END   = "10.0.255.254"   -- ~65534 IPs
local SRC_IP_INC   = "0.0.0.1"

-- Dải IP đích
local DST_IP_START = "192.168.0.1"
local DST_IP_END   = "192.168.0.254"  -- 254 IPs  
local DST_IP_INC   = "0.0.0.1"

-- Dải Port nguồn (tăng entropy cho hash)
local SRC_PORT_MIN = 1024
local SRC_PORT_MAX = 65535
local SRC_PORT_INC = 1

-- Dải Port đích
local DST_PORT_MIN = 80
local DST_PORT_MAX = 8080
local DST_PORT_INC = 1

-- ======================== SETUP ========================
printf("\n============================================\n")
printf("  Pktgen Dynamic Traffic Generator\n")
printf("  Target: flowcore flow table benchmark\n")
printf("============================================\n\n")

-- Dừng mọi traffic cũ
pktgen.stop(SEND_PORT)
pktgen.delay(500)

-- Cấu hình cơ bản cho port
pktgen.set(SEND_PORT, "size", PKT_SIZE)
pktgen.set(SEND_PORT, "rate", TX_RATE)
pktgen.set(SEND_PORT, "burst", 64)
pktgen.set(SEND_PORT, "count", 0)        -- 0 = vô hạn

-- Cấu hình Layer 2
pktgen.set_mac(SEND_PORT, "dst", "00:11:22:33:44:55")

-- Cấu hình Layer 3 - IP
pktgen.set_ipaddr(SEND_PORT, "src", SRC_IP_START .. "/24")
pktgen.set_ipaddr(SEND_PORT, "dst", DST_IP_START)
pktgen.set_proto(SEND_PORT, "udp")

-- ======================== RANGE MODE ========================
-- Range mode cho phép pktgen tự động thay đổi các trường trong packet header
-- theo dải cấu hình, tạo traffic động mà không cần pcap file

printf(">> Cấu hình Range Mode cho traffic động...\n")
pktgen.page("range")

-- Cấu hình dải IP nguồn (tự động tăng dần)
pktgen.src_ip(SEND_PORT, "start", SRC_IP_START)
pktgen.src_ip(SEND_PORT, "min",   SRC_IP_START)
pktgen.src_ip(SEND_PORT, "max",   SRC_IP_END)
pktgen.src_ip(SEND_PORT, "inc",   SRC_IP_INC)

-- Cấu hình dải IP đích
pktgen.dst_ip(SEND_PORT, "start", DST_IP_START)
pktgen.dst_ip(SEND_PORT, "min",   DST_IP_START)
pktgen.dst_ip(SEND_PORT, "max",   DST_IP_END)
pktgen.dst_ip(SEND_PORT, "inc",   DST_IP_INC)

-- Cấu hình dải Port nguồn
pktgen.src_port(SEND_PORT, "start", SRC_PORT_MIN)
pktgen.src_port(SEND_PORT, "min",   SRC_PORT_MIN)
pktgen.src_port(SEND_PORT, "max",   SRC_PORT_MAX)
pktgen.src_port(SEND_PORT, "inc",   SRC_PORT_INC)

-- Cấu hình dải Port đích
pktgen.dst_port(SEND_PORT, "start", DST_PORT_MIN)
pktgen.dst_port(SEND_PORT, "min",   DST_PORT_MIN)
pktgen.dst_port(SEND_PORT, "max",   DST_PORT_MAX)
pktgen.dst_port(SEND_PORT, "inc",   DST_PORT_INC)

-- Cấu hình kích thước gói tin trong range
pktgen.pkt_size(SEND_PORT, "start", PKT_SIZE)
pktgen.pkt_size(SEND_PORT, "min",   PKT_SIZE)
pktgen.pkt_size(SEND_PORT, "max",   PKT_SIZE)
pktgen.pkt_size(SEND_PORT, "inc",   0)

-- Protocol: UDP (17) hoặc TCP (6)
pktgen.ip_proto(SEND_PORT, "udp")

-- Bật Range mode
pktgen.range(SEND_PORT, "on")

printf(">> Range mode đã được bật.\n")
printf(">> Tổng số flow unique tối đa: ~%d flows\n", 65534 * 254)

-- ======================== BẮT ĐẦU PHÁT ========================
printf("\n>> Bắt đầu phát traffic động tại %d%% line rate...\n", TX_RATE)
printf(">> Duration: %d giây\n", DURATION)
printf(">> Packet size: %d bytes\n\n", PKT_SIZE)

pktgen.start(SEND_PORT)

-- Chờ và in stats
for i = 1, DURATION do
    pktgen.delay(1000)
    
    -- Lấy stats
    local stats = pktgen.portStats(SEND_PORT, "port")
    local rate  = pktgen.portStats(SEND_PORT, "rate")
    
    if stats and stats[tonumber(SEND_PORT)] then
        local s = stats[tonumber(SEND_PORT)]
        local r = rate[tonumber(SEND_PORT)]
        printf("[%3d/%d] TX: %12d pkts | Rate: %10d pps | %8d Mbps\n",
            i, DURATION,
            s.opackets or 0,
            r.pkts_tx or 0,
            (r.mbits_tx or 0))
    end
end

-- ======================== DỪNG ========================
printf("\n>> Dừng phát traffic.\n")
pktgen.stop(SEND_PORT)
pktgen.delay(1000)

-- In kết quả cuối cùng
local final_stats = pktgen.portStats(SEND_PORT, "port")
if final_stats and final_stats[tonumber(SEND_PORT)] then
    local fs = final_stats[tonumber(SEND_PORT)]
    printf("\n============ KẾT QUẢ CUỐI CÙNG ============\n")
    printf("Total TX Packets : %d\n", fs.opackets or 0)
    printf("Total TX Bytes   : %d\n", fs.obytes or 0)
    printf("Total TX Errors  : %d\n", fs.oerrors or 0)
    printf("=============================================\n")
end

printf("\n>> Script hoàn tất!\n")
