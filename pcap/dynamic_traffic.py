import sys
import os

# 1. Định nghĩa đường dẫn tuyệt đối tới bộ thư viện TRex ngoài thư mục Home của bạn
# Vì bạn đang đăng nhập với user 'kinosaki-mei', đường dẫn chuẩn sẽ là:
TREX_INTERACTIVE_PATH = "/home/kinosaki-mei/v3.04/automation/trex_control_plane/interactive"

# 2. Bơm thẳng đường dẫn này vào đầu danh sách tìm kiếm của Python trước khi import
if TREX_INTERACTIVE_PATH not in sys.path:
    sys.path.insert(0, TREX_INTERACTIVE_PATH)

# 3. Import mã script liên kết hệ thống nội bộ của Cisco TRex
import trex_from_everywhere

# 4. Bây giờ bạn import trex_stl_lib hay scapy đều sẽ chạy mượt mà 100%
from trex_stl_lib.api import *
import time

def create_stream():
    base_pkt = STLPktBuilder(
        pkt = Ether()/IP(src="192.168.1.1", dst="10.0.0.1")/UDP(sport=1024, dport=80)
    )

    vm = STLScVmRaw([
        STLVmFlowVar(name="src_ip", min_value="192.168.1.1", max_value="192.168.1.254", size=4, op="inc"),
        STLVmWrFlowVar(fv_name="src_ip", pkt_offset="IP.src"),

        STLVmFlowVar(name="src_port", min_value=1024, max_value=65535, size=2, op="inc"),
        STLVmWrFlowVar(fv_name="src_port", pkt_offset="UDP.sport"),

        STLVmFixIpv4(offset="IP")
    ])

    return STLStream(packet = base_pkt, mode = STModeContinuous(), vm = vm)

def main():
    c = STLClient(server = "localhost")
    c.connect()
    c.reset(ports = [0])

    stream = create_stream()
    c.add_streams(stream, ports = [0])

    print("--> Bắt đầu phát bom Traffic động với tốc độ tối đa vào flowcore...")
    c.start(ports = [0], mult = "100%")

    time.sleep(60)

    print("--> Dừng phát.")
    c.stop(ports = [0])
    c.disconnect()

if __name__ == "__main__":
    main()
