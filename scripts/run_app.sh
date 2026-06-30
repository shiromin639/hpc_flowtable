#!/bin/bash

sudo ./build/flowcore -l 0-4 \
    --vdev 'net_pcap0,rx_pcap=./pcap/input_traffic.pcap' \
    --vdev 'net_pcap1,tx_pcap=./pcap/out_worker0.pcap,tx_pcap=./pcap/out_worker1.pcap,tx_pcap=./pcap/out_worker2.pcap,tx_pcap=./pcap/out_worker3.pcap'
