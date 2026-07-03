from scapy.all import Ether, IP, UDP, PcapWriter
import random
import socket
import struct

def generate_random_ip():
    # Generate a completely random IPv4 address
    return socket.inet_ntoa(struct.pack('>I', random.randint(1, 0xffffffff)))

NUM_FLOWS = 100000
OUTPUT_FILE = "./pcap/high_entropy_traffic.pcap"

print(f"Generating {NUM_FLOWS} unique flows into {OUTPUT_FILE}...")

# Use PcapWriter to write directly to disk, saving memory
with PcapWriter(OUTPUT_FILE, append=False, sync=True) as pcap:
    for i in range(NUM_FLOWS):
        # Randomize Source IP and Source Port to guarantee unique 5-tuples
        src_ip = generate_random_ip()
        dst_ip = "192.168.1.100"
        src_port = random.randint(1024, 65535)
        dst_port = 53 # Simulating DNS traffic for your classification counter
        
        # Build the packet. 
        # Adding a small payload to reach the standard 64-byte minimum frame size.
        pkt = Ether(dst="00:11:22:33:44:55", src="aa:bb:cc:dd:ee:ff") / \
              IP(src=src_ip, dst=dst_ip) / \
              UDP(sport=src_port, dport=dst_port) / \
              ("X" * 18) 
              
        pcap.write(pkt)
        
        if (i + 1) % 10000 == 0:
            print(f"Generated {i + 1} packets...")

print("Done! You can now use this PCAP with your DPDK application.")
