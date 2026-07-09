#!/bin/bash

# Thư mục chứa project của bạn
APP_BIN="./build/flowcore"

# 1. Tạo các cặp dây mạng ảo veth trong RAM nếu chưa tồn tại
if ! ip link show veth_rx0 > /dev/null 2>&1; then
    echo "Creating virtual ethernet pairs..."
    sudo ip link add veth_tx0 type veth peer name veth_rx0
    sudo ip link add veth_tx1 type veth peer name veth_rx1
    
    # Bật trạng thái các interface
    sudo ip link set veth_tx0 up
    sudo ip link set veth_rx0 up
    sudo ip link set veth_tx1 up
    sudo ip link set veth_rx1 up

    # Tắt cấu hình hardware checksum và offloading của Linux để tránh lỗi drop gói mức nhân
    sudo ethtool -K veth_tx0 tx off rx off gso off tso off
    sudo ethtool -K veth_rx0 tx off rx off gso off tso off
fi

echo "Starting Flowcore Application with net_af_packet driver..."
echo "PORT_IN (0) -> veth_rx0"
echo "PORT_OUT (1) -> veth_tx1"
echo "--------------------------------------------------------"

# 2. Khởi chạy ứng dụng với driver net_af_packet
# Thay vì memif, ta bind trực tiếp vào interface veth_rx0 và veth_tx1
sudo $APP_BIN -l 0-5 -n 4 \
  --vdev='net_af_packet0,iface=veth_rx0,qpairs=1' \
  --vdev='net_af_packet1,iface=veth_tx1,qpairs=4' \
  --in-memory

# 3. Tự động dọn dẹp dây ảo sau khi tắt ứng dụng (nhấn Ctrl+C)
echo "Cleaning up virtual interfaces..."
sudo ip link del veth_tx0
sudo ip link del veth_tx1
