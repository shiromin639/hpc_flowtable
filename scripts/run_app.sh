#!/bin/bash

sudo ./build/flowcore -l 1-5 \
    --vdev 'net_pcap0,rx_pcap=./pcap/input_traffic.pcap,tx_pcap=./pcap/output_traffic.pcap' \
    --vdev 'net_pcap1,rx_pcap=./pcap/input_traffic.pcap,tx_pcap=./pcap/output_traffic.pcap'

