#!/bin/bash

sudo ./build/flowcore -l 2-7 \
    --vdev 'net_pcap0,rx_pcap=./pcap/flow_sweep_1000000.pcap,infinite_rx=1' \
    --vdev 'net_null0'
