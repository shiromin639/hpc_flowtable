from scapy.all import rdpcap

try:
    pkts = rdpcap("./pcap/input_traffic.pcap")
    print(f"File hợp lệ, chứa {len(pkts)} gói tin. Link type: Ethernet.")
except Exception as e:
    print(f"File PCAP lỗi: {e}")
