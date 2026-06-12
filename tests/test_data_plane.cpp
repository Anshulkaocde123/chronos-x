#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

#include "chronosx/data_plane.hpp"
#include "chronosx/packet_parser.hpp"
#include "chronosx/types.hpp"

namespace {

void write_be16(chronosx::Byte* data, const std::size_t offset, const std::uint16_t value) {
    data[offset] = static_cast<chronosx::Byte>((value >> 8U) & 0xFFU);
    data[offset + 1] = static_cast<chronosx::Byte>(value & 0xFFU);
}

void write_be32(chronosx::Byte* data, const std::size_t offset, const std::uint32_t value) {
    data[offset] = static_cast<chronosx::Byte>((value >> 24U) & 0xFFU);
    data[offset + 1] = static_cast<chronosx::Byte>((value >> 16U) & 0xFFU);
    data[offset + 2] = static_cast<chronosx::Byte>((value >> 8U) & 0xFFU);
    data[offset + 3] = static_cast<chronosx::Byte>(value & 0xFFU);
}

void write_ethernet_header(chronosx::Byte* frame, const std::uint16_t ether_type) {
    const chronosx::Byte dst_mac[6]{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const chronosx::Byte src_mac[6]{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    std::memcpy(frame, dst_mac, sizeof(dst_mac));
    std::memcpy(frame + 6, src_mac, sizeof(src_mac));
    write_be16(frame, 12, ether_type);
}

void write_ipv4_header(chronosx::Byte* ip,
                       const std::uint16_t total_length,
                       const std::uint8_t protocol,
                       const std::uint32_t src_ip,
                       const std::uint32_t dst_ip) {
    ip[0] = static_cast<chronosx::Byte>((4U << 4U) | 5U);
    ip[1] = 0;
    write_be16(ip, 2, total_length);
    write_be16(ip, 4, 0x1234);
    write_be16(ip, 6, 0);
    ip[8] = 64;
    ip[9] = protocol;
    write_be16(ip, 10, 0);
    write_be32(ip, 12, src_ip);
    write_be32(ip, 16, dst_ip);
}

void write_tcp_header(chronosx::Byte* tcp, const std::uint16_t src_port, const std::uint16_t dst_port) {
    write_be16(tcp, 0, src_port);
    write_be16(tcp, 2, dst_port);
    write_be32(tcp, 4, 0x01020304);
    write_be32(tcp, 8, 0x05060708);
    tcp[12] = static_cast<chronosx::Byte>(5U << 4U);
    tcp[13] = 0x18;
    write_be16(tcp, 14, 4096);
    write_be16(tcp, 16, 0);
    write_be16(tcp, 18, 0);
}

std::size_t build_tcp_frame(chronosx::Byte* frame,
                            const std::uint16_t dst_port,
                            const char* payload) {
    const std::size_t payload_length = std::strlen(payload);
    const std::size_t payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                                       chronosx::kTCPBaseHeaderBytes;
    const auto ipv4_total_length = static_cast<std::uint16_t>(chronosx::kIPv4BaseHeaderBytes +
                                                              chronosx::kTCPBaseHeaderBytes + payload_length);

    std::memset(frame, 0, 160);
    write_ethernet_header(frame, chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame + chronosx::kEthernetHeaderBytes,
                      ipv4_total_length,
                      chronosx::kIPv4ProtocolTCP,
                      0x0A000001,
                      0x0A000002);
    write_tcp_header(frame + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes, 12345, dst_port);
    std::memcpy(frame + payload_offset, payload, payload_length);
    return payload_offset + payload_length;
}

chronosx::ChaosRule base_rule() {
    return chronosx::ChaosRule{
        .seed = 0xABCDEF,
        .delay_ns = 0,
        .id = 10,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = 8080,
        .probability = chronosx::kProbabilityScale,
        .protocol = chronosx::kIPv4ProtocolTCP,
        .action = chronosx::PacketAction::Drop,
        .enabled = 1,
        .reserved = {},
    };
}

void test_layout_and_helpers() {
    static_assert(sizeof(chronosx::PacketDescriptor) == 16);
    static_assert(sizeof(chronosx::ProcessedFrame) == 24);
    assert(chronosx::delay_ns_to_ticks(0, 1'000'000) == 1);
    assert(chronosx::delay_ns_to_ticks(1, 1'000'000) == 1);
    assert(chronosx::delay_ns_to_ticks(1'500'000, 1'000'000) == 2);
}

void test_pass_packet_is_tx_and_mac_swapped() {
    std::array<chronosx::Byte, 4096> umem{};
    const std::size_t length = build_tcp_frame(umem.data(), 9090, "pass");
    const chronosx::Byte old_dst = umem[0];
    const chronosx::Byte old_src = umem[6];

    chronosx::TimingWheel<8, 4> wheel;
    std::array<chronosx::PacketDescriptor, 1> rx{chronosx::PacketDescriptor{
        .frame_address = 0,
        .frame_length = static_cast<chronosx::PacketLength>(length),
        .options = 0,
    }};
    std::array<chronosx::ProcessedFrame, 1> tx{};
    std::array<chronosx::ProcessedFrame, 1> recycle{};

    const std::array<chronosx::ChaosRule, 1> rules{base_rule()};
    const auto result = chronosx::process_packet_batch(umem.data(),
                                                       std::span<const chronosx::PacketDescriptor>{rx},
                                                       std::span<const chronosx::ChaosRule>{rules},
                                                       wheel,
                                                       std::span<chronosx::ProcessedFrame>{tx},
                                                       std::span<chronosx::ProcessedFrame>{recycle});

    assert(result.tx_count == 1);
    assert(result.recycle_count == 0);
    assert(result.stats.tx_frames == 1);
    assert(result.stats.packets_dropped == 0);
    assert(tx[0].action == chronosx::PacketAction::Pass);
    assert(umem[0] == old_src);
    assert(umem[6] == old_dst);
}

void test_drop_packet_is_recycled() {
    std::array<chronosx::Byte, 4096> umem{};
    const std::size_t length = build_tcp_frame(umem.data(), 8080, "drop");

    chronosx::TimingWheel<8, 4> wheel;
    const chronosx::PacketDescriptor rx_descriptor{
        .frame_address = 0,
        .frame_length = static_cast<chronosx::PacketLength>(length),
        .options = 0,
    };
    std::array<chronosx::ProcessedFrame, 1> tx{};
    std::array<chronosx::ProcessedFrame, 1> recycle{};

    const std::array<chronosx::ChaosRule, 1> rules{base_rule()};
    const auto result = chronosx::process_packet_batch(umem.data(),
                                                       std::span<const chronosx::PacketDescriptor>{&rx_descriptor, 1},
                                                       std::span<const chronosx::ChaosRule>{rules},
                                                       wheel,
                                                       std::span<chronosx::ProcessedFrame>{tx},
                                                       std::span<chronosx::ProcessedFrame>{recycle});

    assert(result.tx_count == 0);
    assert(result.recycle_count == 1);
    assert(result.stats.packets_seen == 1);
    assert(result.stats.packets_dropped == 1);
    assert(recycle[0].action == chronosx::PacketAction::Drop);
    assert(recycle[0].rule_id == 10);
}

void test_delay_packet_enters_timing_wheel() {
    std::array<chronosx::Byte, 4096> umem{};
    const std::size_t length = build_tcp_frame(umem.data(), 8080, "delay");

    auto rule = base_rule();
    rule.action = chronosx::PacketAction::Delay;
    rule.delay_ns = 2'000'000;

    chronosx::TimingWheel<8, 4> wheel;
    const chronosx::PacketDescriptor rx_descriptor{
        .frame_address = 0,
        .frame_length = static_cast<chronosx::PacketLength>(length),
        .options = 0,
    };
    std::array<chronosx::ProcessedFrame, 1> tx{};
    std::array<chronosx::ProcessedFrame, 1> recycle{};

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto result = chronosx::process_packet_batch(umem.data(),
                                                       std::span<const chronosx::PacketDescriptor>{&rx_descriptor, 1},
                                                       std::span<const chronosx::ChaosRule>{rules},
                                                       wheel,
                                                       std::span<chronosx::ProcessedFrame>{tx},
                                                       std::span<chronosx::ProcessedFrame>{recycle});

    assert(result.tx_count == 0);
    assert(result.recycle_count == 0);
    assert(result.stats.packets_delayed == 1);
    assert(wheel.pending_count() == 1);
    assert(wheel.tick().empty());
    const auto expired = wheel.tick();
    assert(expired.size() == 1);
    assert(expired[0].rule_id == 10);
}

void test_corrupt_packet_flips_payload_and_forwards() {
    std::array<chronosx::Byte, 4096> umem{};
    const std::size_t length = build_tcp_frame(umem.data(), 8080, "Z");
    const std::size_t payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                                       chronosx::kTCPBaseHeaderBytes;
    const chronosx::Byte old_payload = umem[payload_offset];

    auto rule = base_rule();
    rule.action = chronosx::PacketAction::Corrupt;

    chronosx::TimingWheel<8, 4> wheel;
    const chronosx::PacketDescriptor rx_descriptor{
        .frame_address = 0,
        .frame_length = static_cast<chronosx::PacketLength>(length),
        .options = 0,
    };
    std::array<chronosx::ProcessedFrame, 1> tx{};
    std::array<chronosx::ProcessedFrame, 1> recycle{};

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto result = chronosx::process_packet_batch(umem.data(),
                                                       std::span<const chronosx::PacketDescriptor>{&rx_descriptor, 1},
                                                       std::span<const chronosx::ChaosRule>{rules},
                                                       wheel,
                                                       std::span<chronosx::ProcessedFrame>{tx},
                                                       std::span<chronosx::ProcessedFrame>{recycle});

    assert(result.tx_count == 1);
    assert(result.recycle_count == 0);
    assert(result.stats.packets_corrupted == 1);
    assert(tx[0].action == chronosx::PacketAction::Corrupt);
    assert(umem[payload_offset] == static_cast<chronosx::Byte>(old_payload ^ 0x01U));
}

void test_malformed_packet_is_recycled() {
    std::array<chronosx::Byte, 4096> umem{};
    write_ethernet_header(umem.data(), chronosx::kEtherTypeARP);

    chronosx::TimingWheel<8, 4> wheel;
    const chronosx::PacketDescriptor rx_descriptor{
        .frame_address = 0,
        .frame_length = 64,
        .options = 0,
    };
    std::array<chronosx::ProcessedFrame, 1> tx{};
    std::array<chronosx::ProcessedFrame, 1> recycle{};

    const std::array<chronosx::ChaosRule, 1> rules{base_rule()};
    const auto result = chronosx::process_packet_batch(umem.data(),
                                                       std::span<const chronosx::PacketDescriptor>{&rx_descriptor, 1},
                                                       std::span<const chronosx::ChaosRule>{rules},
                                                       wheel,
                                                       std::span<chronosx::ProcessedFrame>{tx},
                                                       std::span<chronosx::ProcessedFrame>{recycle});

    assert(result.tx_count == 0);
    assert(result.recycle_count == 1);
    assert(result.stats.packets_malformed == 1);
    assert(result.stats.packets_dropped == 1);
    assert(recycle[0].action == chronosx::PacketAction::Pass);
}

void test_out_of_bounds_descriptor_is_recycled() {
    std::array<chronosx::Byte, 128> umem{};

    chronosx::TimingWheel<8, 4> wheel;
    const chronosx::PacketDescriptor rx_descriptor{
        .frame_address = 96,
        .frame_length = 64,
        .options = 0,
    };
    std::array<chronosx::ProcessedFrame, 1> tx{};
    std::array<chronosx::ProcessedFrame, 1> recycle{};

    const auto result = chronosx::process_packet_batch(umem.data(),
                                                       umem.size(),
                                                       std::span<const chronosx::PacketDescriptor>{&rx_descriptor, 1},
                                                       std::span<const chronosx::ChaosRule>{},
                                                       wheel,
                                                       std::span<chronosx::ProcessedFrame>{tx},
                                                       std::span<chronosx::ProcessedFrame>{recycle});

    assert(result.tx_count == 0);
    assert(result.recycle_count == 1);
    assert(result.stats.invalid_descriptors == 1);
    assert(result.stats.packets_malformed == 1);
    assert(result.stats.packets_dropped == 1);
}

}  // namespace

int main() {
    test_layout_and_helpers();
    test_pass_packet_is_tx_and_mac_swapped();
    test_drop_packet_is_recycled();
    test_delay_packet_enters_timing_wheel();
    test_corrupt_packet_flips_payload_and_forwards();
    test_malformed_packet_is_recycled();
    test_out_of_bounds_descriptor_is_recycled();

    std::cout << "data plane tests passed\n";
    return 0;
}
