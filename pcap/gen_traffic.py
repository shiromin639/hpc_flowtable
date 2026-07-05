from scapy.all import *
import random

# Cấu hình
NUM_PACKETS = 500000  # Tạo 500,000 gói tin khác nhau
OUTPUT_FILE = "./pcap/dynamic_flows.pcap"
packets = []

print(f"Đang sinh {NUM_PACKETS} gói tin ngẫu nhiên...")

for i in range(NUM_PACKETS):
    # Sinh ngẫu nhiên IP Source và Port Source
    src_ip = f"10.{random.randint(0,255)}.{random.randint(0,255)}.{random.randint(1,254)}"
    src_port = random.randint(1024, 65535)
    
    # Tạo gói tin MAC -> IP -> UDP
    pkt = Ether(dst="00:11:22:33:44:55", src="aa:bb:cc:dd:ee:ff") / \
          IP(src=src_ip, dst="192.168.1.100") / \
          UDP(sport=src_port, dport=80) / \
          Raw(load="TestPayload1234567890")
          
    packets.append(pkt)

print("Đang ghi ra file PCAP...")
wrpcap(OUTPUT_FILE, packets)
print(f"Đã tạo xong file {OUTPUT_FILE}!")
