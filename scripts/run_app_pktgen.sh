#!/bin/bash

sudo ./build/flowcore -l 0-5 -n 4 \
  --vdev='net_memif0,id=0,role=server,socket=/tmp/memif_port0.sock' \
  --vdev='net_memif1,id=1,role=server,socket=/tmp/memif_port1.sock' \
  --in-memory \
