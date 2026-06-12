#!/usr/bin/env python3
"""
chronos_client.py — Python CLI client for the Chronos-X control plane.

Speaks the binary protocol defined in include/chronosx/protocol.hpp.
Wire format (big-endian):
  [ magic:2 | version:1 | opcode:1 | sequence:4 | payload_length:4 | crc32:4 | reserved:8 ]
  [ payload: payload_length bytes ]

Usage:
  python3 chronos_client.py ping
  python3 chronos_client.py stats
  python3 chronos_client.py add-rule --port 8080 --action drop --prob 10000
  python3 chronos_client.py list-rules
  python3 chronos_client.py clear-rules
"""

import argparse
import socket
import struct
import sys

# Protocol constants (must match protocol.hpp)
PROTOCOL_MAGIC = 0x4358
PROTOCOL_VERSION = 1
HEADER_SIZE = 24

# Opcodes
OP_PING = 0x01
OP_PONG = 0x02
OP_GET_STATS = 0x10
OP_STATS_RESPONSE = 0x11
OP_ADD_RULE = 0x20
OP_REMOVE_RULE = 0x21
OP_CLEAR_RULES = 0x22
OP_LIST_RULES = 0x23
OP_RULES_RESPONSE = 0x24
OP_ERROR = 0xFF

# ChaosRule struct (must match chaos_engine.hpp)
# seed:8 | delay_ns:8 | id:4 | src_ip:4 | dst_ip:4 | src_port:2 | dst_port:2 |
# probability:2 | protocol:1 | action:1 | enabled:1 | reserved:3
CHAOS_RULE_FORMAT = ">QQIIIHHHBBBxxx"
CHAOS_RULE_SIZE = struct.calcsize(CHAOS_RULE_FORMAT)

# StatUpdate struct (must match types.hpp)
# timestamp_cycles:8 | packets_seen:8 | packets_dropped:8 | bytes_seen:8 |
# average_latency_ns:8 | last_action:1 | reserved0:1 | reserved1:2 | pad:4
STAT_UPDATE_FORMAT = ">QQQQQBxHxxxx"
STAT_UPDATE_SIZE = 48

ACTION_MAP = {"pass": 0, "drop": 1, "delay": 2, "corrupt": 3}
ACTION_NAMES = {0: "pass", 1: "drop", 2: "delay", 3: "corrupt"}


def crc32_table():
    """Build a CRC32 lookup table matching the project's implementation."""
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
        table.append(crc & 0xFFFFFFFF)
    return table


CRC_TABLE = crc32_table()


def compute_crc32(data: bytes) -> int:
    """Compute CRC32 matching protocol.hpp's crc32_compute."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc = CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def encode_header(opcode: int, sequence: int, payload_length: int) -> bytes:
    """Encode a protocol header (24 bytes)."""
    # Pack without CRC first to compute CRC over the header
    header_no_crc = struct.pack(
        ">HBBI I xxxx xxxx",  # magic, version, opcode, sequence, payload_length
        PROTOCOL_MAGIC, PROTOCOL_VERSION, opcode, sequence, payload_length,
    )
    # The above won't work cleanly — let's do it properly
    buf = bytearray(HEADER_SIZE)
    struct.pack_into(">H", buf, 0, PROTOCOL_MAGIC)
    buf[2] = PROTOCOL_VERSION
    buf[3] = opcode
    struct.pack_into(">I", buf, 4, sequence)
    struct.pack_into(">I", buf, 8, payload_length)
    # CRC at offset 12 — compute over [0:12]
    crc = compute_crc32(bytes(buf[:12]))
    struct.pack_into(">I", buf, 12, crc)
    # buf[16:24] = reserved (zeros)
    return bytes(buf)


def decode_header(data: bytes):
    """Decode a protocol header, return (magic, version, opcode, seq, payload_len, crc)."""
    if len(data) < HEADER_SIZE:
        raise ValueError(f"Header too short: {len(data)} < {HEADER_SIZE}")
    magic = struct.unpack_from(">H", data, 0)[0]
    version = data[2]
    opcode = data[3]
    sequence = struct.unpack_from(">I", data, 4)[0]
    payload_length = struct.unpack_from(">I", data, 8)[0]
    crc = struct.unpack_from(">I", data, 12)[0]
    return magic, version, opcode, sequence, payload_length, crc


def encode_message(opcode: int, sequence: int, payload: bytes = b"") -> bytes:
    """Encode a complete protocol message with CRC matching protocol.hpp."""
    buf = bytearray(HEADER_SIZE)
    struct.pack_into(">H", buf, 0, PROTOCOL_MAGIC)
    buf[2] = PROTOCOL_VERSION
    buf[3] = opcode
    struct.pack_into(">I", buf, 4, sequence)
    struct.pack_into(">I", buf, 8, len(payload))
    struct.pack_into(">I", buf, 12, 0)
    crc = compute_crc32(bytes(buf) + payload)
    struct.pack_into(">I", buf, 12, crc)
    return bytes(buf) + payload


def verify_message_crc(header: bytes, payload: bytes) -> bool:
    """Verify a received message CRC using the same algorithm as protocol.hpp."""
    if len(header) != HEADER_SIZE:
        return False
    expected = struct.unpack_from(">I", header, 12)[0]
    normalized = bytearray(header)
    struct.pack_into(">I", normalized, 12, 0)
    return compute_crc32(bytes(normalized) + payload) == expected


def send_message(sock: socket.socket, opcode: int, sequence: int,
                 payload: bytes = b"") -> tuple:
    """Send a message and receive the response."""
    sock.sendall(encode_message(opcode, sequence, payload))

    # Read response header
    resp_header = recv_exact(sock, HEADER_SIZE)
    magic, version, resp_op, resp_seq, resp_plen, resp_crc = decode_header(resp_header)

    # Read response payload
    resp_payload = recv_exact(sock, resp_plen) if resp_plen > 0 else b""
    if not verify_message_crc(resp_header, resp_payload):
        raise ValueError("Response CRC mismatch")

    return resp_op, resp_seq, resp_payload


def recv_exact(sock: socket.socket, nbytes: int) -> bytes:
    """Receive exactly nbytes from the socket."""
    data = b""
    while len(data) < nbytes:
        chunk = sock.recv(nbytes - len(data))
        if not chunk:
            raise ConnectionError("Connection closed")
        data += chunk
    return data


def cmd_ping(sock: socket.socket, seq: int):
    """Send a Ping and expect a Pong."""
    opcode, _, _ = send_message(sock, OP_PING, seq)
    if opcode == OP_PONG:
        print(f"Pong received (seq={seq})")
    else:
        print(f"Unexpected response opcode: 0x{opcode:02X}")


def cmd_stats(sock: socket.socket, seq: int):
    """Request and display stats."""
    opcode, _, payload = send_message(sock, OP_GET_STATS, seq)
    if opcode == OP_STATS_RESPONSE and len(payload) >= STAT_UPDATE_SIZE:
        ts, seen, dropped, bytes_seen, avg_lat = struct.unpack_from(">QQQQQ", payload, 0)
        action = payload[40]
        print(f"Stats:")
        print(f"  timestamp_cycles:   {ts}")
        print(f"  packets_seen:       {seen}")
        print(f"  packets_dropped:    {dropped}")
        print(f"  bytes_seen:         {bytes_seen}")
        print(f"  average_latency_ns: {avg_lat}")
        print(f"  last_action:        {ACTION_NAMES.get(action, 'unknown')}")
    elif opcode == OP_ERROR:
        print(f"Error response")
    else:
        print(f"Unexpected response: opcode=0x{opcode:02X} payload_len={len(payload)}")


def cmd_add_rule(sock: socket.socket, seq: int, args):
    """Add a chaos rule."""
    action_val = ACTION_MAP.get(args.action, 0)
    payload = struct.pack(
        CHAOS_RULE_FORMAT,
        args.seed,        # seed
        args.delay,       # delay_ns
        args.rule_id,     # id
        0,                # src_ip
        0,                # dst_ip
        0,                # src_port
        args.port,        # dst_port
        args.prob,        # probability
        6,                # protocol (TCP)
        action_val,       # action
        1,                # enabled
    )
    opcode, _, _ = send_message(sock, OP_ADD_RULE, seq, payload)
    if opcode == OP_PONG:
        print(f"Rule {args.rule_id} added: port={args.port} action={args.action} prob={args.prob}")
    elif opcode == OP_ERROR:
        print(f"Failed to add rule")
    else:
        print(f"Unexpected response: 0x{opcode:02X}")


def cmd_clear_rules(sock: socket.socket, seq: int):
    """Clear all rules."""
    opcode, _, _ = send_message(sock, OP_CLEAR_RULES, seq)
    if opcode == OP_PONG:
        print("All rules cleared")
    else:
        print(f"Unexpected response: 0x{opcode:02X}")


def cmd_list_rules(sock: socket.socket, seq: int):
    """List all rules."""
    opcode, _, payload = send_message(sock, OP_LIST_RULES, seq)
    if opcode == OP_RULES_RESPONSE:
        count = len(payload) // CHAOS_RULE_SIZE
        print(f"Rules ({count}):")
        for i in range(count):
            offset = i * CHAOS_RULE_SIZE
            fields = struct.unpack_from(CHAOS_RULE_FORMAT, payload, offset)
            seed, delay_ns, rule_id, src_ip, dst_ip, src_port, dst_port, prob, proto, action, enabled = fields
            print(f"  [{rule_id}] port={dst_port} action={ACTION_NAMES.get(action, '?')} "
                  f"prob={prob}/{10000} enabled={'yes' if enabled else 'no'}")
    elif opcode == OP_ERROR:
        print("Error listing rules")
    else:
        print(f"Unexpected response: 0x{opcode:02X}")


def main():
    parser = argparse.ArgumentParser(
        description="Chronos-X control plane client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s ping
  %(prog)s stats
  %(prog)s add-rule --port 8080 --action drop --prob 10000
  %(prog)s list-rules
  %(prog)s clear-rules
""")
    parser.add_argument("--host", default="127.0.0.1", help="Server host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9090, help="Server port (default: 9090)")

    sub = parser.add_subparsers(dest="command", help="Command to execute")

    sub.add_parser("ping", help="Ping the server")
    sub.add_parser("stats", help="Get statistics")
    sub.add_parser("list-rules", help="List all rules")
    sub.add_parser("clear-rules", help="Clear all rules")

    add_parser = sub.add_parser("add-rule", help="Add a chaos rule")
    add_parser.add_argument("--rule-id", type=int, default=1, help="Rule ID")
    add_parser.add_argument("--port", type=int, dest="rule_port", default=8080, help="Destination port")
    add_parser.add_argument("--action", choices=["pass", "drop", "delay", "corrupt"], default="drop")
    add_parser.add_argument("--prob", type=int, default=10000, help="Probability 0-10000")
    add_parser.add_argument("--seed", type=int, default=0xBEEF, help="Rule seed")
    add_parser.add_argument("--delay", type=int, default=0, help="Delay in nanoseconds")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    try:
        with socket.create_connection((args.host, args.port), timeout=5) as sock:
            seq = 1
            if args.command == "ping":
                cmd_ping(sock, seq)
            elif args.command == "stats":
                cmd_stats(sock, seq)
            elif args.command == "add-rule":
                args.port = args.rule_port
                cmd_add_rule(sock, seq, args)
            elif args.command == "list-rules":
                cmd_list_rules(sock, seq)
            elif args.command == "clear-rules":
                cmd_clear_rules(sock, seq)
    except ConnectionRefusedError:
        print(f"Error: Could not connect to {args.host}:{args.port}")
        print("Is the Chronos-X control server running? (./chronosx --server)")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
