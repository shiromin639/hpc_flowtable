#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUN_PKTGEN="${SCRIPT_DIR}/run_pkt_gen.sh"
MEMIF_SOCKET="${MEMIF_SOCKET:-/tmp/flowcore-pktgen.sock}"

if [ ! -S "${MEMIF_SOCKET}" ]; then
    echo "memif socket not found: ${MEMIF_SOCKET}"
    echo "Run ./scripts/run_app_pktgen.sh first."
    exit 1
fi

run_case() {
    local name=$1
    local flows=$2
    local rate=$3
    local duration=$4
    local pkt_size=$5

    echo ""
    echo "============================================"
    echo "Case: ${name}"
    echo "flows=${flows} rate=${rate}% duration=${duration}s pkt_size=${pkt_size}"
    echo "============================================"

    FLOW_COUNT="${flows}" \
    RATE="${rate}" \
    DURATION="${duration}" \
    PKT_SIZE="${pkt_size}" \
    "${RUN_PKTGEN}"
}

echo "============================================"
echo "  Flowcore Pktgen Benchmark Suite"
echo "============================================"
echo "Topology:"
echo "  pktgen(memif client) -> flowcore(memif server RX) -> net_null TX"
echo ""
echo "Start flowcore first:"
echo "  ./scripts/run_app_pktgen.sh"
echo "============================================"

run_case "baseline_256_flows_64B" 256 100 20 64
run_case "flow_pressure_100k_64B" 100000 100 20 64
run_case "flow_pressure_1M_64B" 1000000 100 25 64
run_case "throughput_100k_512B" 100000 100 20 512

echo ""
echo "============================================"
echo "Benchmark suite complete."
echo "Read flowcore console stats for RX/TX PPS, active flows, drops, and worker balance."
echo "============================================"
