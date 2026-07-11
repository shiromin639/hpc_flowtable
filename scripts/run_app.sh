#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FLOWCORE_BIN="${REPO_ROOT}/build/flowcore"
PCAP_PATH="${PCAP_PATH:-${REPO_ROOT}/pcap/fixed_packets_1000000_pkts_10000_flows.pcap}"
RULES_PATH="${RULES_PATH:-${REPO_ROOT}/rules.cfg}"

APP_LCORES="${APP_LCORES:-0-5}"
APP_MEM_MB="${APP_MEM_MB:-2048}"

# The net_pcap PMD keeps one mbuf per packet when infinite_rx=1.
# flowcore multiplies this value by the number of ports, so 600k gives
# roughly 1.2M mbufs with the current net_pcap + net_null topology.
FLOWCORE_NUM_MBUFS="${FLOWCORE_NUM_MBUFS:-600000}"
FLOWCORE_MBUF_DATA_SIZE="${FLOWCORE_MBUF_DATA_SIZE:-256}"

if [ ! -x "${FLOWCORE_BIN}" ]; then
    echo "flowcore binary not found: ${FLOWCORE_BIN}"
    echo "Build it first with: meson compile -C build flowcore"
    exit 1
fi

if [ ! -f "${PCAP_PATH}" ]; then
    echo "PCAP not found: ${PCAP_PATH}"
    exit 1
fi

if [ ! -f "${RULES_PATH}" ]; then
    echo "Rules file not found: ${RULES_PATH}"
    exit 1
fi

echo "============================================"
echo "  Flowcore fixed-packet net_pcap Run"
echo "============================================"
echo "binary      : ${FLOWCORE_BIN}"
echo "pcap        : ${PCAP_PATH}"
echo "rules       : ${RULES_PATH}"
echo "lcores      : ${APP_LCORES}"
echo "mem MB      : ${APP_MEM_MB}"
echo "mbufs/env   : ${FLOWCORE_NUM_MBUFS}"
echo "mbuf size   : ${FLOWCORE_MBUF_DATA_SIZE}"
echo "topology    : net_pcap RX -> flowcore -> net_null TX"
echo "mode        : infinite replay"
echo "============================================"

sudo env \
    XDG_RUNTIME_DIR=/tmp \
    FLOWCORE_NUM_MBUFS="${FLOWCORE_NUM_MBUFS}" \
    FLOWCORE_MBUF_DATA_SIZE="${FLOWCORE_MBUF_DATA_SIZE}" \
    FLOWCORE_RULES_PATH="${RULES_PATH}" \
    "${FLOWCORE_BIN}" \
    --no-pci \
    --in-memory \
    --no-shconf \
    --no-telemetry \
    -l "${APP_LCORES}" \
    -n 4 \
    -m "${APP_MEM_MB}" \
    --vdev "net_pcap0,rx_pcap=${PCAP_PATH},infinite_rx=1" \
    --vdev "net_null0"
