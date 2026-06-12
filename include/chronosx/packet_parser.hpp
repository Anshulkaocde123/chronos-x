#pragma once

#include <cstddef>
#include <cstdint>

#include "chronosx/types.hpp"

namespace chronosx {

inline constexpr std::uint16_t kEtherTypeIPv4 = 0x0800;
inline constexpr std::uint16_t kEtherTypeARP = 0x0806;
inline constexpr std::uint16_t kEtherTypeVLAN = 0x8100;
inline constexpr std::uint16_t kEtherTypeIPv6 = 0x86DD;

inline constexpr std::uint8_t kIPv4ProtocolICMP = 1;
inline constexpr std::uint8_t kIPv4ProtocolTCP = 6;
inline constexpr std::uint8_t kIPv4ProtocolUDP = 17;

inline constexpr std::uint16_t kIPv4FragmentOffsetMask = 0x1FFF;

struct EthernetHeader {
    Byte dst_mac[6]{};
    Byte src_mac[6]{};
    Byte ether_type[2]{};
};

static_assert(sizeof(EthernetHeader) == kEthernetHeaderBytes);
static_assert(alignof(EthernetHeader) == alignof(Byte));

struct IPv4Header {
    Byte version_ihl{};
    Byte dscp_ecn{};
    Byte total_length[2]{};
    Byte identification[2]{};
    Byte flags_fragment[2]{};
    Byte ttl{};
    Byte protocol{};
    Byte header_checksum[2]{};
    Byte src_addr[4]{};
    Byte dst_addr[4]{};
};

static_assert(sizeof(IPv4Header) == kIPv4BaseHeaderBytes);
static_assert(alignof(IPv4Header) == alignof(Byte));

struct TCPHeader {
    Byte src_port[2]{};
    Byte dst_port[2]{};
    Byte seq_num[4]{};
    Byte ack_num[4]{};
    Byte data_offset_reserved{};
    Byte flags{};
    Byte window_size[2]{};
    Byte checksum[2]{};
    Byte urgent_pointer[2]{};
};

static_assert(sizeof(TCPHeader) == kTCPBaseHeaderBytes);
static_assert(alignof(TCPHeader) == alignof(Byte));

struct UDPHeader {
    Byte src_port[2]{};
    Byte dst_port[2]{};
    Byte length[2]{};
    Byte checksum[2]{};
};

static_assert(sizeof(UDPHeader) == kUDPHeaderBytes);
static_assert(alignof(UDPHeader) == alignof(Byte));

enum class ParseStatus : std::uint8_t {
    Ok = 0,
    NullData = 1,
    TooShortForEthernet = 2,
    NonIPv4 = 3,
    TooShortForIPv4 = 4,
    InvalidIPv4Version = 5,
    InvalidIPv4HeaderLength = 6,
    TruncatedIPv4Header = 7,
    IPv4TotalLengthTooSmall = 8,
    IPv4TotalLengthExceedsFrame = 9,
    IPv4FragmentWithoutL4 = 10,
    UnsupportedTransport = 11,
    TooShortForTCP = 12,
    InvalidTCPHeaderLength = 13,
    TruncatedTCPHeader = 14,
    TooShortForUDP = 15,
    InvalidUDPLength = 16,
    TruncatedUDPPayload = 17,
};

[[nodiscard]] constexpr const char* parse_status_name(const ParseStatus status) noexcept {
    switch (status) {
        case ParseStatus::Ok:
            return "Ok";
        case ParseStatus::NullData:
            return "NullData";
        case ParseStatus::TooShortForEthernet:
            return "TooShortForEthernet";
        case ParseStatus::NonIPv4:
            return "NonIPv4";
        case ParseStatus::TooShortForIPv4:
            return "TooShortForIPv4";
        case ParseStatus::InvalidIPv4Version:
            return "InvalidIPv4Version";
        case ParseStatus::InvalidIPv4HeaderLength:
            return "InvalidIPv4HeaderLength";
        case ParseStatus::TruncatedIPv4Header:
            return "TruncatedIPv4Header";
        case ParseStatus::IPv4TotalLengthTooSmall:
            return "IPv4TotalLengthTooSmall";
        case ParseStatus::IPv4TotalLengthExceedsFrame:
            return "IPv4TotalLengthExceedsFrame";
        case ParseStatus::IPv4FragmentWithoutL4:
            return "IPv4FragmentWithoutL4";
        case ParseStatus::UnsupportedTransport:
            return "UnsupportedTransport";
        case ParseStatus::TooShortForTCP:
            return "TooShortForTCP";
        case ParseStatus::InvalidTCPHeaderLength:
            return "InvalidTCPHeaderLength";
        case ParseStatus::TruncatedTCPHeader:
            return "TruncatedTCPHeader";
        case ParseStatus::TooShortForUDP:
            return "TooShortForUDP";
        case ParseStatus::InvalidUDPLength:
            return "InvalidUDPLength";
        case ParseStatus::TruncatedUDPPayload:
            return "TruncatedUDPPayload";
    }

    return "Unknown";
}

struct PacketView {
    const Byte* data{};
    std::size_t frame_length{};
    ParseStatus status{ParseStatus::TooShortForEthernet};
    std::uint16_t ether_type{};
    std::uint8_t ipv4_protocol{};
    std::size_t ipv4_header_length{};
    std::size_t ipv4_total_length{};
    std::uint32_t src_ip{};
    std::uint32_t dst_ip{};
    std::size_t l3_offset{};
    std::size_t l4_offset{};
    std::size_t payload_offset{};
    std::size_t payload_length{};
    std::size_t tcp_header_length{};
    std::uint16_t src_port{};
    std::uint16_t dst_port{};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == ParseStatus::Ok;
    }

    [[nodiscard]] constexpr bool is_ipv4() const noexcept {
        return ether_type == kEtherTypeIPv4;
    }

    [[nodiscard]] constexpr bool is_tcp() const noexcept {
        return ok() && ipv4_protocol == kIPv4ProtocolTCP;
    }

    [[nodiscard]] constexpr bool is_udp() const noexcept {
        return ok() && ipv4_protocol == kIPv4ProtocolUDP;
    }

    [[nodiscard]] constexpr const Byte* dst_mac() const noexcept {
        return data != nullptr && frame_length >= kEthernetHeaderBytes ? data : nullptr;
    }

    [[nodiscard]] constexpr const Byte* src_mac() const noexcept {
        return data != nullptr && frame_length >= kEthernetHeaderBytes ? data + 6 : nullptr;
    }

    [[nodiscard]] constexpr const Byte* payload_data() const noexcept {
        return ok() && data != nullptr && payload_offset <= frame_length ? data + payload_offset : nullptr;
    }
};

[[nodiscard]] constexpr std::uint16_t read_be16(const Byte* data, const std::size_t offset) noexcept {
    const std::uint16_t high = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) << 8U);
    const std::uint16_t low = static_cast<std::uint16_t>(data[offset + 1]);
    return static_cast<std::uint16_t>(high | low);
}

[[nodiscard]] constexpr std::uint32_t read_be32(const Byte* data, const std::size_t offset) noexcept {
    const std::uint32_t b0 = static_cast<std::uint32_t>(data[offset]);
    const std::uint32_t b1 = static_cast<std::uint32_t>(data[offset + 1]);
    const std::uint32_t b2 = static_cast<std::uint32_t>(data[offset + 2]);
    const std::uint32_t b3 = static_cast<std::uint32_t>(data[offset + 3]);
    return static_cast<std::uint32_t>((b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3);
}

[[nodiscard]] constexpr std::uint8_t ipv4_version(const Byte version_ihl) noexcept {
    return static_cast<std::uint8_t>(version_ihl >> 4U);
}

[[nodiscard]] constexpr std::uint8_t ipv4_ihl_words(const Byte version_ihl) noexcept {
    return static_cast<std::uint8_t>(version_ihl & 0x0FU);
}

[[nodiscard]] constexpr std::uint8_t tcp_data_offset_words(const Byte data_offset_reserved) noexcept {
    return static_cast<std::uint8_t>(data_offset_reserved >> 4U);
}

[[nodiscard]] inline PacketView parse_packet(const Byte* data, const std::size_t length) noexcept {
    PacketView view{
        .data = data,
        .frame_length = length,
    };

    if (data == nullptr) {
        view.status = length == 0 ? ParseStatus::TooShortForEthernet : ParseStatus::NullData;
        return view;
    }

    if (length < kEthernetHeaderBytes) {
        view.status = ParseStatus::TooShortForEthernet;
        return view;
    }

    view.ether_type = read_be16(data, 12);

    if (view.ether_type != kEtherTypeIPv4) {
        view.status = ParseStatus::NonIPv4;
        return view;
    }

    view.l3_offset = kEthernetHeaderBytes;

    if (length < view.l3_offset + kIPv4BaseHeaderBytes) {
        view.status = ParseStatus::TooShortForIPv4;
        return view;
    }

    const Byte version_ihl = data[view.l3_offset];
    const std::uint8_t version = ipv4_version(version_ihl);
    const std::uint8_t ihl_words = ipv4_ihl_words(version_ihl);

    if (version != 4) {
        view.status = ParseStatus::InvalidIPv4Version;
        return view;
    }

    if (ihl_words < 5) {
        view.status = ParseStatus::InvalidIPv4HeaderLength;
        return view;
    }

    view.ipv4_header_length = static_cast<std::size_t>(ihl_words) * 4U;

    if (length < view.l3_offset + view.ipv4_header_length) {
        view.status = ParseStatus::TruncatedIPv4Header;
        return view;
    }

    view.ipv4_total_length = read_be16(data, view.l3_offset + 2);

    if (view.ipv4_total_length < view.ipv4_header_length) {
        view.status = ParseStatus::IPv4TotalLengthTooSmall;
        return view;
    }

    if (length < view.l3_offset + view.ipv4_total_length) {
        view.status = ParseStatus::IPv4TotalLengthExceedsFrame;
        return view;
    }

    const std::uint16_t flags_fragment = read_be16(data, view.l3_offset + 6);
    const std::uint16_t fragment_offset = static_cast<std::uint16_t>(flags_fragment & kIPv4FragmentOffsetMask);

    if (fragment_offset != 0) {
        view.status = ParseStatus::IPv4FragmentWithoutL4;
        return view;
    }

    view.ipv4_protocol = data[view.l3_offset + 9];
    view.src_ip = read_be32(data, view.l3_offset + 12);
    view.dst_ip = read_be32(data, view.l3_offset + 16);
    view.l4_offset = view.l3_offset + view.ipv4_header_length;

    const std::size_t transport_length = view.ipv4_total_length - view.ipv4_header_length;

    if (view.ipv4_protocol == kIPv4ProtocolTCP) {
        if (transport_length < kTCPBaseHeaderBytes) {
            view.status = ParseStatus::TooShortForTCP;
            return view;
        }

        const std::uint8_t data_offset_words = tcp_data_offset_words(data[view.l4_offset + 12]);

        if (data_offset_words < 5) {
            view.status = ParseStatus::InvalidTCPHeaderLength;
            return view;
        }

        view.tcp_header_length = static_cast<std::size_t>(data_offset_words) * 4U;

        if (transport_length < view.tcp_header_length) {
            view.status = ParseStatus::TruncatedTCPHeader;
            return view;
        }

        view.src_port = read_be16(data, view.l4_offset);
        view.dst_port = read_be16(data, view.l4_offset + 2);
        view.payload_offset = view.l4_offset + view.tcp_header_length;
        view.payload_length = transport_length - view.tcp_header_length;
        view.status = ParseStatus::Ok;
        return view;
    }

    if (view.ipv4_protocol == kIPv4ProtocolUDP) {
        if (transport_length < kUDPHeaderBytes) {
            view.status = ParseStatus::TooShortForUDP;
            return view;
        }

        const std::uint16_t udp_length = read_be16(data, view.l4_offset + 4);

        if (udp_length < kUDPHeaderBytes) {
            view.status = ParseStatus::InvalidUDPLength;
            return view;
        }

        if (static_cast<std::size_t>(udp_length) > transport_length) {
            view.status = ParseStatus::TruncatedUDPPayload;
            return view;
        }

        view.src_port = read_be16(data, view.l4_offset);
        view.dst_port = read_be16(data, view.l4_offset + 2);
        view.payload_offset = view.l4_offset + kUDPHeaderBytes;
        view.payload_length = static_cast<std::size_t>(udp_length) - kUDPHeaderBytes;
        view.status = ParseStatus::Ok;
        return view;
    }

    view.status = ParseStatus::UnsupportedTransport;
    return view;
}

}
