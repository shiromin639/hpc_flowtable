#!/bin/bash

# Run testpmd as a memif client connecting to flowcore
sudo dpdk-testpmd -l 6-8 -n 4 \
  --vdev='net_memif0,id=0,role=client,socket=/tmp/memif_port0.sock' \
  --vdev='net_memif1,id=1,role=client,socket=/tmp/memif_port1.sock' \
  --in-memory \
  -- \
  --forward-mode=flowgen \
  --flowgen-flows=1000000 \
  --stats-period=1 \
  --interactive
