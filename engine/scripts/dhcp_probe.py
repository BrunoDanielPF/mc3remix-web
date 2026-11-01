"""Build a local DHCP Offer or ACK from a captured SMAP request for RX testing.

Diagnostic only; this does not allocate addresses or answer arbitrary clients.
"""

import argparse
import ipaddress
import struct
from pathlib import Path


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def reply(request: bytes, client_ip: bytes, server_ip: bytes, message_type: int) -> bytes:
    if len(request) < 14 + 20 + 8 + 240 or request[12:14] != b"\x08\x00":
        raise ValueError("expected an Ethernet IPv4 DHCP request")
    ip_size = (request[14] & 15) * 4
    udp = 14 + ip_size
    if request[23] != 17 or request[udp:udp + 4] != b"\x00\x44\x00\x43":
        raise ValueError("expected UDP 68 -> 67")
    payload = request[udp + 8:]
    if payload[0] != 1 or payload[236:240] != b"\x63\x82\x53\x63":
        raise ValueError("expected BOOTP request with DHCP cookie")
    expected_type = 1 if message_type == 2 else 3
    if b"\x35\x01" + bytes([expected_type]) not in payload[240:]:
        raise ValueError("unexpected DHCP request type")

    bootp = bytearray(payload[:236])
    bootp[0] = 2
    # Preserve the request's BOOTP broadcast flag. The Ethernet/IP envelope
    # remains broadcast while the client has no configured address.
    bootp[16:20] = client_ip
    bootp[20:24] = server_ip
    client_id_at = payload.find(b"\x3d\x07", 240)
    client_id = payload[client_id_at:client_id_at + 9] if client_id_at >= 0 else b""
    options = (b"\x63\x82\x53\x63"
               + b"\x35\x01" + bytes([message_type])
               + b"\x36\x04" + server_ip
               + b"\x33\x04" + struct.pack("!I", 3600)
               + b"\x01\x04\xff\xff\xff\x00"
               + b"\x03\x04" + server_ip
               + b"\x06\x04" + server_ip
               + client_id
               + b"\xff")
    dhcp = bytes(bootp) + options
    dhcp += bytes(max(0, 300 - len(dhcp)))
    src, dst = server_ip, b"\xff" * 4
    udp_header = struct.pack("!HHHH", 67, 68, 8 + len(dhcp), 0)
    pseudo = src + dst + b"\0\x11" + struct.pack("!H", len(udp_header) + len(dhcp))
    udp_sum = checksum(pseudo + udp_header + dhcp)
    udp_header = udp_header[:6] + struct.pack("!H", udp_sum or 0xFFFF)
    ip_header = bytearray(b"\x45\x00" + struct.pack("!H", 20 + len(udp_header) + len(dhcp))
                          + b"\0\x02\0\0\x40\x11\0\0" + src + dst)
    ip_header[10:12] = struct.pack("!H", checksum(ip_header))
    return b"\xff" * 6 + b"\x02\x00\x00\x00\xff\x01\x08\x00" + bytes(ip_header) + udp_header + dhcp


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("request_pcap", type=Path)
    parser.add_argument("output_frame", type=Path)
    parser.add_argument("--reply", choices=("offer", "ack"), default="offer")
    parser.add_argument("--client-ip", default="10.0.0.10")
    parser.add_argument("--server-ip", default="10.0.0.1")
    args = parser.parse_args()
    capture = args.request_pcap.read_bytes()
    if capture[:4] != b"\xd4\xc3\xb2\xa1" or len(capture) < 40:
        raise ValueError("expected a little-endian Ethernet PCAP")
    length = struct.unpack_from("<I", capture, 32)[0]
    frame = capture[40:40 + length]
    if len(frame) != length:
        raise ValueError("truncated PCAP frame")
    frame_reply = reply(frame, ipaddress.IPv4Address(args.client_ip).packed,
                        ipaddress.IPv4Address(args.server_ip).packed,
                        2 if args.reply == "offer" else 5)
    args.output_frame.write_bytes(frame_reply)
    print(f"DHCP {args.reply}: {len(frame_reply)} bytes, {args.client_ip}, transaction {frame_reply[46:50].hex()}")


if __name__ == "__main__":
    main()
