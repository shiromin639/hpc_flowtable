#!/bin/bash
# =============================================================================
# Benchmark Suite - Chạy nhiều kịch bản test liên tiếp
# Sử dụng pktgen CLI commands để thay đổi tham số giữa các test
# =============================================================================
#
# Kịch bản test:
#   1. Throughput Test   - Max rate, fixed flows
#   2. Flow Creation     - Rate sweep, max flow diversity 
#   3. Mixed Workload    - Existing + new flows
#   4. Small Packet      - 64 byte packets (worst case PPS)
#   5. Large Packet      - 1518 byte packets (max throughput Mbps)
# =============================================================================

PKTGEN_PATH="/home/kinosaki-mei/Pktgen-DPDK/build/app/pktgen"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MEMIF_SOCKET="/tmp/memif-flowcore.sock"
PKTGEN_CORES="6-8"

echo "============================================"
echo "  Pktgen Benchmark Suite"
echo "============================================"
echo ""
echo "Script này tạo file Lua tạm cho từng kịch bản"
echo "và chạy pktgen với từng kịch bản."
echo ""
echo "QUAN TRỌNG: flowcore phải đang chạy trước!"
echo "============================================"

# Kiểm tra flowcore đang chạy
if [ ! -S "${MEMIF_SOCKET}" ]; then
    echo "LỖI: Không tìm thấy memif socket!"
    echo "Hãy chạy ./scripts/run_app_pktgen.sh trước."
    exit 1
fi

run_scenario() {
    local SCENARIO_NAME=$1
    local LUA_FILE=$2
    local DURATION=$3

    echo ""
    echo ">> [$SCENARIO_NAME] Đang chạy... (${DURATION}s)"
    echo "   Lua: ${LUA_FILE}"
    
    sudo timeout $((DURATION + 10)) ${PKTGEN_PATH} \
        -l ${PKTGEN_CORES} \
        --file-prefix=pktgen \
        --no-pci \
        --vdev "net_memif0,role=client,id=0,socket=${MEMIF_SOCKET},socket-abstract=no" \
        -- \
        -T \
        -P \
        -m "[7-8].0" \
        -f ${LUA_FILE}
    
    echo ">> [$SCENARIO_NAME] Hoàn tất."
    sleep 2
}

# ---- Kịch bản 1: Max Throughput (ít flow) ----
cat > /tmp/bench_throughput.lua << 'LUAEOF'
package.path = package.path .. ";?.lua;test/?.lua;app/?.lua;../?.lua;/home/kinosaki-mei/Pktgen-DPDK/?.lua"
require "Pktgen"

pktgen.stop("0")
pktgen.delay(500)

pktgen.set("0", "size", 64)
pktgen.set("0", "rate", 100)
pktgen.set("0", "burst", 64)
pktgen.set("0", "count", 0)
pktgen.set_mac("0", "dst", "00:11:22:33:44:55")
pktgen.set_ipaddr("0", "src", "10.0.0.1/24")
pktgen.set_ipaddr("0", "dst", "192.168.1.1")
pktgen.set_proto("0", "udp")

-- Chỉ 256 flow (ít flow -> test raw throughput)
pktgen.page("range")
pktgen.src_ip("0", "start", "10.0.0.1")
pktgen.src_ip("0", "min",   "10.0.0.1")
pktgen.src_ip("0", "max",   "10.0.0.254")
pktgen.src_ip("0", "inc",   "0.0.0.1")
pktgen.dst_ip("0", "start", "192.168.1.1")
pktgen.dst_ip("0", "min",   "192.168.1.1")
pktgen.dst_ip("0", "max",   "192.168.1.1")
pktgen.dst_ip("0", "inc",   "0.0.0.0")
pktgen.src_port("0", "start", 1024)
pktgen.src_port("0", "min",   1024)
pktgen.src_port("0", "max",   1024)
pktgen.src_port("0", "inc",   0)
pktgen.dst_port("0", "start", 80)
pktgen.dst_port("0", "min",   80)
pktgen.dst_port("0", "max",   80)
pktgen.dst_port("0", "inc",   0)
pktgen.pkt_size("0", "start", 64)
pktgen.pkt_size("0", "min",   64)
pktgen.pkt_size("0", "max",   64)
pktgen.pkt_size("0", "inc",   0)
pktgen.ip_proto("0", "udp")
pktgen.range("0", "on")

printf("\n===== SCENARIO: Max Throughput (256 flows) =====\n")
pktgen.start("0")
for i = 1, 30 do
    pktgen.delay(1000)
    local r = pktgen.portStats("0", "rate")
    if r and r[0] then
        printf("[%2d/30] TX: %10d pps | %8d Mbps\n", i, r[0].pkts_tx or 0, r[0].mbits_tx or 0)
    end
end
pktgen.stop("0")
printf("===== SCENARIO DONE =====\n")
LUAEOF

# ---- Kịch bản 2: Flow Creation Stress (nhiều flow mới) ----
cat > /tmp/bench_flow_creation.lua << 'LUAEOF'
package.path = package.path .. ";?.lua;test/?.lua;app/?.lua;../?.lua;/home/kinosaki-mei/Pktgen-DPDK/?.lua"
require "Pktgen"

pktgen.stop("0")
pktgen.delay(500)

pktgen.set("0", "size", 64)
pktgen.set("0", "rate", 100)
pktgen.set("0", "burst", 64)
pktgen.set("0", "count", 0)
pktgen.set_mac("0", "dst", "00:11:22:33:44:55")
pktgen.set_ipaddr("0", "src", "10.0.0.1/24")
pktgen.set_ipaddr("0", "dst", "192.168.0.1")
pktgen.set_proto("0", "udp")

-- Max diversity: 65534 IPs x 254 dst x varied ports = millions flows
pktgen.page("range")
pktgen.src_ip("0", "start", "10.0.0.1")
pktgen.src_ip("0", "min",   "10.0.0.1")
pktgen.src_ip("0", "max",   "10.0.255.254")
pktgen.src_ip("0", "inc",   "0.0.0.1")
pktgen.dst_ip("0", "start", "192.168.0.1")
pktgen.dst_ip("0", "min",   "192.168.0.1")
pktgen.dst_ip("0", "max",   "192.168.0.254")
pktgen.dst_ip("0", "inc",   "0.0.0.1")
pktgen.src_port("0", "start", 1024)
pktgen.src_port("0", "min",   1024)
pktgen.src_port("0", "max",   65535)
pktgen.src_port("0", "inc",   1)
pktgen.dst_port("0", "start", 80)
pktgen.dst_port("0", "min",   80)
pktgen.dst_port("0", "max",   8080)
pktgen.dst_port("0", "inc",   1)
pktgen.pkt_size("0", "start", 64)
pktgen.pkt_size("0", "min",   64)
pktgen.pkt_size("0", "max",   64)
pktgen.pkt_size("0", "inc",   0)
pktgen.ip_proto("0", "udp")
pktgen.range("0", "on")

printf("\n===== SCENARIO: Flow Creation Stress =====\n")
pktgen.start("0")
for i = 1, 30 do
    pktgen.delay(1000)
    local r = pktgen.portStats("0", "rate")
    if r and r[0] then
        printf("[%2d/30] TX: %10d pps | %8d Mbps\n", i, r[0].pkts_tx or 0, r[0].mbits_tx or 0)
    end
end
pktgen.stop("0")
printf("===== SCENARIO DONE =====\n")
LUAEOF

echo "============================================"
echo "  Bắt đầu chạy Benchmark Suite"
echo "============================================"

run_scenario "1-Throughput"     "/tmp/bench_throughput.lua"      40
run_scenario "2-FlowCreation"   "/tmp/bench_flow_creation.lua"   40

echo ""
echo "============================================"
echo "  Benchmark Suite Hoàn Tất!"
echo "============================================"
