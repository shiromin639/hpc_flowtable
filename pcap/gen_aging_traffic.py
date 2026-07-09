#!/usr/bin/env python3
import os
from scapy.all import Ether, IP, TCP, UDP, wrpcap

def generate_pcap():
    packets = []
    
    # Định nghĩa 3 luồng (Flows) để theo dõi hành vi
    # Flow 1 (TCP): Sẽ sống suốt kịch bản vì được lặp lại liên tục
    flow1 = Ether()/IP(src="10.0.0.1", dst="10.0.0.2")/TCP(sport=1111, dport=80)
    # Flow 2 (UDP): Tạo ở giây thứ 0, không lặp lại -> Sẽ chết ở giây thứ 5
    flow2 = Ether()/IP(src="192.168.1.1", dst="192.168.1.2")/UDP(sport=2222, dport=53)
    # Flow 3 (TCP): Tạo muộn ở giây thứ 3, không lặp lại -> Sẽ chết ở giây thứ 8
    flow3 = Ether()/IP(src="172.16.0.1", dst="172.16.0.2")/TCP(sport=3333, dport=443)

    print("--- Bắt đầu tạo kịch bản gói tin có timestamp ---")
    
    # === GIÂY THỨ 0: Tạo Luồng 1 và Luồng 2 ===
    pkt1 = flow1.copy(); pkt1.time = 0.0; packets.append(pkt1)
    pkt2 = flow2.copy(); pkt2.time = 0.05; packets.append(pkt2)
    
    # === GIÂY THỨ 2: Duy trì Luồng 1 (Hot Path), Luồng 2 im lặng ===
    pkt3 = flow1.copy(); pkt3.time = 2.0; packets.append(pkt3)
    
    # === GIÂY THỨ 3: Duy trì Luồng 1, Tạo mới Luồng 3 (Cold Path) ===
    pkt4 = flow1.copy(); pkt4.time = 3.0; packets.append(pkt4)
    pkt5 = flow3.copy(); pkt5.time = 3.1; packets.append(pkt5)
    
    # === GIÂY THỨ 4: Duy trì Luồng 1 ===
    pkt6 = flow1.copy(); pkt6.time = 4.0; packets.append(pkt6)
    
    # === GIÂY THỨ 6: Luồng 2 đã chết (6s - 0.05s = 5.95s > 5s timeout) ===
    # Tiếp tục duy trì Luồng 1 để xem nó có sống sót không
    pkt7 = flow1.copy(); pkt7.time = 6.0; packets.append(pkt7)
    
    # === GIÂY THỨ 9: Luồng 3 đã chết (9s - 3.1s = 5.9s > 5s timeout) ===
    pkt8 = flow1.copy(); pkt8.time = 9.0; packets.append(pkt8)

    # Xuất ra file pcap
    os.makedirs("./pcap", exist_ok=True)
    output_file = "./pcap/aging_scenario.pcap"
    wrpcap(output_file, packets)
    print(self_name := f"Đã sinh xong file PCAP tại: {output_file}")
    print("Tổng số gói tin: 8 gói. Thời lượng kịch bản gốc: 9 giây.")

if __name__ == "__main__":
    generate_pcap()
