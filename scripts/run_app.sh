#!/bin/bash

sudo ./build/flowcore -l 0-5 \
    --vdev 'net_pcap0,rx_pcap=./pcap/high_entropy_traffic.pcap,infinite_rx=1' \
    --vdev 'net_null0'
