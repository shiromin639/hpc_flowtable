#!/bin/bash

sudo ./build/flowcore -l 1-2 \
    --vdev 'net_pcap0,rx_pcap=./pcap/input_traffic.pcap' \
    --vdev 'net_pcap1,tx_pcap=./pcap/output_traffic.pcap'

