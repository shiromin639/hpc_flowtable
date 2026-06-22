from scapy.all import *

# Tạo 5 gói tin TCP đi từ IP 192.168.1.100 -> 10.0.0.1
# Port 5000 -> 80
packets = []
for i in range(5):
    pkt = (
        # Gán cứng MAC ảo để Scapy không tự động đi tìm ARP nữa
        Ether(src="00:11:22:33:44:55", dst="66:77:88:99:aa:bb")
        / IP(src="192.168.1.100", dst="10.0.0.1")
        / TCP(sport=5000, dport=80)
        / Raw(load=f"Payload_Test_{i}")
    )
    packets.append(pkt)

# Lưu thành file PCAP
wrpcap("./pcap/input_traffic.pcap", packets)
print("Đã tạo file input.pcap với 5 gói tin!")
