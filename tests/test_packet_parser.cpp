#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "chronosx/packet_parser.hpp"
#include "chronosx/types.hpp"

using namespace std;

namespace {

void write_be16(chronosx::Byte* data, const size_t offset, const uint16_t value) {
    data[offset] = static_cast<chronosx::Byte>((value >> 8U) & 0xFFU);
    data[offset + 1] = static_cast<chronosx::Byte>(value & 0xFFU);
}

void write_be32(chronosx::Byte* data, const size_t offset, const uint32_t value) {
    data[offset] = static_cast<chronosx::Byte>((value >> 24U) & 0xFFU);
    data[offset + 1] = static_cast<chronosx::Byte>((value >> 16U) & 0xFFU);
    data[offset + 2] = static_cast<chronosx::Byte>((value >> 8U) & 0xFFU);
    data[offset + 3] = static_cast<chronosx::Byte>(value & 0xFFU);
}

void write_ethernet_header(chronosx::Byte* frame, const uint16_t ether_type) {
    const chronosx::Byte dst_mac[6]{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const chronosx::Byte src_mac[6]{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    write_be16(frame, 12, ether_type);
}

void write_ipv4_header(chronosx::Byte* ip,
                       const uint8_t ihl_words,
                       const uint16_t total_length,
                       const uint16_t flags_fragment,
                       const uint8_t protocol,
                       const uint32_t src_ip,
                       const uint32_t dst_ip) {
    ip[0] = static_cast<chronosx::Byte>((4U << 4U) | (ihl_words & 0x0FU));
    ip[1] = 0;
    write_be16(ip, 2, total_length);
    write_be16(ip, 4, 0x1234);
    write_be16(ip, 6, flags_fragment);
    ip[8] = 64;
    ip[9] = protocol;
    write_be16(ip, 10, 0);
    write_be32(ip, 12, src_ip);
    write_be32(ip, 16, dst_ip);
}

void write_tcp_header(chronosx::Byte* tcp,
                      const uint16_t src_port,
                      const uint16_t dst_port,
                      const uint8_t data_offset_words,
                      const uint8_t flags) {
    write_be16(tcp, 0, src_port);
    write_be16(tcp, 2, dst_port);
    write_be32(tcp, 4, 0x01020304);
    write_be32(tcp, 8, 0x05060708);
    tcp[12] = static_cast<chronosx::Byte>(data_offset_words << 4U);
    tcp[13] = flags;
    write_be16(tcp, 14, 4096);
    write_be16(tcp, 16, 0);
    write_be16(tcp, 18, 0);
}

void write_udp_header(chronosx::Byte* udp, const uint16_t src_port, const uint16_t dst_port, const uint16_t length) {
    write_be16(udp, 0, src_port);
    write_be16(udp, 2, dst_port);
    write_be16(udp, 4, length);
    write_be16(udp, 6, 0);
}

void test_struct_sizes() {
    static_assert(sizeof(chronosx::EthernetHeader) == chronosx::kEthernetHeaderBytes);
    static_assert(sizeof(chronosx::IPv4Header) == chronosx::kIPv4BaseHeaderBytes);
    static_assert(sizeof(chronosx::TCPHeader) == chronosx::kTCPBaseHeaderBytes);
    static_assert(sizeof(chronosx::UDPHeader) == chronosx::kUDPHeaderBytes);
}

void test_tcp_packet_parse() {
    array<chronosx::Byte, 128> frame{};
    constexpr size_t payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                                      chronosx::kTCPBaseHeaderBytes;
    constexpr const char payload[] = "chronos";
    constexpr size_t payload_length = sizeof(payload) - 1;
    constexpr uint16_t ipv4_total_length = static_cast<uint16_t>(chronosx::kIPv4BaseHeaderBytes +
                                                                 chronosx::kTCPBaseHeaderBytes + payload_length);

    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      ipv4_total_length,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0xC0A80001,
                      0xC0A80002);
    write_tcp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes,
                     12345,
                     8080,
                     5,
                     0x18);
    memcpy(frame.data() + payload_offset, payload, payload_length);

    const auto view = chronosx::parse_packet(frame.data(), payload_offset + payload_length);

    assert(view.ok());
    assert(view.status == chronosx::ParseStatus::Ok);
    assert(view.is_ipv4());
    assert(view.is_tcp());
    assert(!view.is_udp());
    assert(view.ether_type == chronosx::kEtherTypeIPv4);
    assert(view.ipv4_protocol == chronosx::kIPv4ProtocolTCP);
    assert(view.ipv4_header_length == chronosx::kIPv4BaseHeaderBytes);
    assert(view.ipv4_total_length == ipv4_total_length);
    assert(view.src_ip == 0xC0A80001);
    assert(view.dst_ip == 0xC0A80002);
    assert(view.l3_offset == chronosx::kEthernetHeaderBytes);
    assert(view.l4_offset == chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes);
    assert(view.tcp_header_length == chronosx::kTCPBaseHeaderBytes);
    assert(view.src_port == 12345);
    assert(view.dst_port == 8080);
    assert(view.payload_offset == payload_offset);
    assert(view.payload_length == payload_length);
    assert(view.payload_data() == frame.data() + payload_offset);
    assert(memcmp(view.payload_data(), payload, payload_length) == 0);
}

void test_udp_packet_parse() {
    array<chronosx::Byte, 128> frame{};
    constexpr size_t payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                                      chronosx::kUDPHeaderBytes;
    constexpr uint16_t udp_length = 12;
    constexpr uint16_t ipv4_total_length = chronosx::kIPv4BaseHeaderBytes + udp_length;

    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      ipv4_total_length,
                      0,
                      chronosx::kIPv4ProtocolUDP,
                      0x0A000001,
                      0x0A000002);
    write_udp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes,
                     5353,
                     53,
                     udp_length);
    frame[payload_offset] = 0xDE;
    frame[payload_offset + 1] = 0xAD;
    frame[payload_offset + 2] = 0xBE;
    frame[payload_offset + 3] = 0xEF;

    const auto view = chronosx::parse_packet(frame.data(), payload_offset + 4);

    assert(view.ok());
    assert(view.is_udp());
    assert(!view.is_tcp());
    assert(view.ipv4_protocol == chronosx::kIPv4ProtocolUDP);
    assert(view.src_ip == 0x0A000001);
    assert(view.dst_ip == 0x0A000002);
    assert(view.src_port == 5353);
    assert(view.dst_port == 53);
    assert(view.payload_offset == payload_offset);
    assert(view.payload_length == 4);
    assert(view.payload_data()[0] == 0xDE);
    assert(view.payload_data()[3] == 0xEF);
}

void test_ipv4_options_shift_l4_offset() {
    array<chronosx::Byte, 128> frame{};
    constexpr size_t ipv4_header_length = 32;
    constexpr size_t l4_offset = chronosx::kEthernetHeaderBytes + ipv4_header_length;
    constexpr uint16_t ipv4_total_length = ipv4_header_length + chronosx::kTCPBaseHeaderBytes;

    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      8,
                      ipv4_total_length,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0x01020304,
                      0x05060708);
    memset(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes, 0x01, 12);
    write_tcp_header(frame.data() + l4_offset, 1000, 443, 5, 0x10);

    const auto view = chronosx::parse_packet(frame.data(), l4_offset + chronosx::kTCPBaseHeaderBytes);

    assert(view.ok());
    assert(view.ipv4_header_length == ipv4_header_length);
    assert(view.l4_offset == l4_offset);
    assert(view.dst_port == 443);
    assert(view.payload_offset == l4_offset + chronosx::kTCPBaseHeaderBytes);
    assert(view.payload_length == 0);
}

void test_tcp_options_shift_payload_offset() {
    array<chronosx::Byte, 128> frame{};
    constexpr size_t tcp_header_length = 32;
    constexpr size_t l4_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes;
    constexpr size_t payload_offset = l4_offset + tcp_header_length;
    constexpr uint16_t ipv4_total_length = chronosx::kIPv4BaseHeaderBytes + tcp_header_length + 3;

    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      ipv4_total_length,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0x11111111,
                      0x22222222);
    write_tcp_header(frame.data() + l4_offset, 2222, 9000, 8, 0x18);
    memset(frame.data() + l4_offset + chronosx::kTCPBaseHeaderBytes, 0x01, 12);
    frame[payload_offset] = 1;
    frame[payload_offset + 1] = 2;
    frame[payload_offset + 2] = 3;

    const auto view = chronosx::parse_packet(frame.data(), payload_offset + 3);

    assert(view.ok());
    assert(view.tcp_header_length == tcp_header_length);
    assert(view.payload_offset == payload_offset);
    assert(view.payload_length == 3);
    assert(view.payload_data()[0] == 1);
    assert(view.payload_data()[2] == 3);
}

void test_ethernet_padding_ignored_after_ipv4_total_length() {
    array<chronosx::Byte, 96> frame{};
    constexpr uint16_t ipv4_total_length = chronosx::kIPv4BaseHeaderBytes + chronosx::kUDPHeaderBytes;
    constexpr size_t real_packet_length = chronosx::kEthernetHeaderBytes + ipv4_total_length;

    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      ipv4_total_length,
                      0,
                      chronosx::kIPv4ProtocolUDP,
                      0xABCDEF01,
                      0x10203040);
    write_udp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes, 1, 2, 8);
    memset(frame.data() + real_packet_length, 0xEE, frame.size() - real_packet_length);

    const auto view = chronosx::parse_packet(frame.data(), frame.size());

    assert(view.ok());
    assert(view.payload_offset == real_packet_length);
    assert(view.payload_length == 0);
}

void test_non_ipv4() {
    array<chronosx::Byte, 64> frame{};

    write_ethernet_header(frame.data(), chronosx::kEtherTypeARP);

    const auto view = chronosx::parse_packet(frame.data(), frame.size());

    assert(!view.ok());
    assert(view.status == chronosx::ParseStatus::NonIPv4);
    assert(view.ether_type == chronosx::kEtherTypeARP);
}

void test_malformed_packets() {
    array<chronosx::Byte, 128> frame{};

    assert(chronosx::parse_packet(nullptr, 1).status == chronosx::ParseStatus::NullData);
    assert(chronosx::parse_packet(frame.data(), 13).status == chronosx::ParseStatus::TooShortForEthernet);

    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    assert(chronosx::parse_packet(frame.data(), 33).status == chronosx::ParseStatus::TooShortForIPv4);

    frame[chronosx::kEthernetHeaderBytes] = static_cast<chronosx::Byte>((6U << 4U) | 5U);
    assert(chronosx::parse_packet(frame.data(), 34).status == chronosx::ParseStatus::InvalidIPv4Version);

    frame[chronosx::kEthernetHeaderBytes] = static_cast<chronosx::Byte>((4U << 4U) | 4U);
    assert(chronosx::parse_packet(frame.data(), 34).status == chronosx::ParseStatus::InvalidIPv4HeaderLength);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      8,
                      32,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0,
                      0);
    assert(chronosx::parse_packet(frame.data(), 40).status == chronosx::ParseStatus::TruncatedIPv4Header);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      19,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0,
                      0);
    assert(chronosx::parse_packet(frame.data(), 34).status == chronosx::ParseStatus::IPv4TotalLengthTooSmall);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      60,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0,
                      0);
    assert(chronosx::parse_packet(frame.data(), 34).status == chronosx::ParseStatus::IPv4TotalLengthExceedsFrame);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      40,
                      1,
                      chronosx::kIPv4ProtocolTCP,
                      0,
                      0);
    assert(chronosx::parse_packet(frame.data(), 54).status == chronosx::ParseStatus::IPv4FragmentWithoutL4);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      20,
                      0,
                      chronosx::kIPv4ProtocolICMP,
                      0,
                      0);
    assert(chronosx::parse_packet(frame.data(), 34).status == chronosx::ParseStatus::UnsupportedTransport);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      39,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0,
                      0);
    assert(chronosx::parse_packet(frame.data(), 53).status == chronosx::ParseStatus::TooShortForTCP);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      40,
                      0,
                      chronosx::kIPv4ProtocolTCP,
                      0,
                      0);
    write_tcp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes, 1, 2, 4, 0);
    assert(chronosx::parse_packet(frame.data(), 54).status == chronosx::ParseStatus::InvalidTCPHeaderLength);

    write_tcp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes, 1, 2, 8, 0);
    assert(chronosx::parse_packet(frame.data(), 54).status == chronosx::ParseStatus::TruncatedTCPHeader);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      27,
                      0,
                      chronosx::kIPv4ProtocolUDP,
                      0,
                      0);
    assert(chronosx::parse_packet(frame.data(), 41).status == chronosx::ParseStatus::TooShortForUDP);

    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      5,
                      28,
                      0,
                      chronosx::kIPv4ProtocolUDP,
                      0,
                      0);
    write_udp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes, 1, 2, 7);
    assert(chronosx::parse_packet(frame.data(), 42).status == chronosx::ParseStatus::InvalidUDPLength);

    write_udp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes, 1, 2, 12);
    assert(chronosx::parse_packet(frame.data(), 42).status == chronosx::ParseStatus::TruncatedUDPPayload);
}

}

int main() {
    test_struct_sizes();
    test_tcp_packet_parse();
    test_udp_packet_parse();
    test_ipv4_options_shift_l4_offset();
    test_tcp_options_shift_payload_offset();
    test_ethernet_padding_ignored_after_ipv4_total_length();
    test_non_ipv4();
    test_malformed_packets();

    cout << "packet parser tests passed\n";
    return 0;
}
