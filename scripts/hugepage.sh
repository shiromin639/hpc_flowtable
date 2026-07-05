#!/bin/bash
# Tăng lên 2048 pages (4GB) để cả App và Pktgen chạy thoải mái nhất
echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
