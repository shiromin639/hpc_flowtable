#!/usr/bin/env python3
import os
from scapy.all import Ether, IP, UDP, wrpcap

def generate_stress_pcap():
    packets = []
    num_packets = 2000000  # 2,000,000 gói tin = 2,000,000 flows
    print(f"Đang sinh {num_packets} gói tin với IP ngẫu nhiên để tạo flows...")
    
    base_mac_src = "02:00:00:00:00:00"
    base_mac_dst = "02:00:00:00:00:01"
    
    for i in range(num_packets):
        # Tạo IP hợp lệ từ index i (tránh quá 255)
        # VD: i=0 -> 10.0.0.0, i=256 -> 10.0.1.0
        b2 = (i // 65536) % 256
        b3 = (i // 256) % 256
        b4 = i % 256
        src_ip = f"10.{b2}.{b3}.{b4}"
        src_port = 1024 + (i % 60000)
        
        pkt = Ether(src=base_mac_src, dst=base_mac_dst) / IP(src=src_ip, dst="192.168.1.1") / UDP(sport=src_port, dport=80)
        packets.append(pkt)
        
        if (i+1) % 10000 == 0:
            print(f"Đã tạo {i+1} gói...")

    # Ghi ra file pcap
    os.makedirs("./pcap", exist_ok=True)
    output_file = "./pcap/stress_test.pcap"
    print(f"Đang ghi ra file {output_file}... (có thể tốn vài chục giây)")
    wrpcap(output_file, packets)
    print("Hoàn tất!")

if __name__ == "__main__":
    generate_stress_pcap()
