#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

#include "chronosx/chaos_engine.hpp"
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

void write_udp_header(chronosx::Byte* udp,
                      const std::uint16_t src_port,
                      const std::uint16_t dst_port,
                      const std::uint16_t length) {
    write_be16(udp, 0, src_port);
    write_be16(udp, 2, dst_port);
    write_be16(udp, 4, length);
    write_be16(udp, 6, 0);
}

std::size_t build_tcp_frame(std::array<chronosx::Byte, 160>& frame,
                            const std::uint16_t src_port,
                            const std::uint16_t dst_port,
                            const char* payload) {
    const std::size_t payload_length = std::strlen(payload);
    const std::size_t payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                                       chronosx::kTCPBaseHeaderBytes;
    const auto ipv4_total_length = static_cast<std::uint16_t>(chronosx::kIPv4BaseHeaderBytes +
                                                              chronosx::kTCPBaseHeaderBytes + payload_length);
    assert(payload_offset + payload_length <= frame.size());

    frame.fill(0);
    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      ipv4_total_length,
                      chronosx::kIPv4ProtocolTCP,
                      0x0A000001,
                      0x0A000002);
    write_tcp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes,
                     src_port,
                     dst_port);
    std::memcpy(frame.data() + payload_offset, payload, payload_length);
    return payload_offset + payload_length;
}

std::size_t build_udp_frame(std::array<chronosx::Byte, 160>& frame,
                            const std::uint16_t src_port,
                            const std::uint16_t dst_port,
                            const char* payload) {
    const std::size_t payload_length = std::strlen(payload);
    const std::size_t payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                                       chronosx::kUDPHeaderBytes;
    const auto udp_length = static_cast<std::uint16_t>(chronosx::kUDPHeaderBytes + payload_length);
    const auto ipv4_total_length = static_cast<std::uint16_t>(chronosx::kIPv4BaseHeaderBytes + udp_length);
    assert(payload_offset + payload_length <= frame.size());

    frame.fill(0);
    write_ethernet_header(frame.data(), chronosx::kEtherTypeIPv4);
    write_ipv4_header(frame.data() + chronosx::kEthernetHeaderBytes,
                      ipv4_total_length,
                      chronosx::kIPv4ProtocolUDP,
                      0x0A000001,
                      0x0A000002);
    write_udp_header(frame.data() + chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes,
                     src_port,
                     dst_port,
                     udp_length);
    std::memcpy(frame.data() + payload_offset, payload, payload_length);
    return payload_offset + payload_length;
}

template <std::size_t RuleCount>
chronosx::ChaosDecision decide_with(const chronosx::PacketView& packet,
                                    const std::array<chronosx::ChaosRule, RuleCount>& rules) {
    return chronosx::decide_packet(packet, std::span<const chronosx::ChaosRule>{rules.data(), rules.size()});
}

chronosx::ChaosRule base_rule() {
    return chronosx::ChaosRule{
        .seed = 0xC001CAFE,
        .delay_ns = 0,
        .id = 7,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = 0,
        .probability = chronosx::kProbabilityScale,
        .protocol = 0,
        .action = chronosx::PacketAction::Drop,
        .enabled = 1,
        .reserved = {},
    };
}

void test_exact_tcp_drop_match() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_tcp_frame(frame, 12345, 8080, "order=buy");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto rule = base_rule();
    rule.dst_port = 8080;
    rule.protocol = chronosx::kIPv4ProtocolTCP;

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto decision = decide_with(packet, rules);

    assert(decision.matched());
    assert(decision.rule_id == 7);
    assert(decision.should_drop());
    assert(!decision.should_forward());
}

void test_non_matching_rule_passes_without_match() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_tcp_frame(frame, 12345, 9090, "order=buy");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto rule = base_rule();
    rule.dst_port = 8080;
    rule.protocol = chronosx::kIPv4ProtocolTCP;

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto decision = decide_with(packet, rules);

    assert(!decision.matched());
    assert(decision.action == chronosx::PacketAction::Pass);
    assert(decision.should_forward());
}

void test_udp_delay_with_wildcards() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_udp_frame(frame, 5353, 53, "dns");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto rule = base_rule();
    rule.id = 11;
    rule.action = chronosx::PacketAction::Delay;
    rule.delay_ns = 2'000'000;
    rule.dst_port = 53;
    rule.protocol = 0;

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto decision = decide_with(packet, rules);

    assert(decision.matched());
    assert(decision.rule_id == 11);
    assert(decision.should_delay());
    assert(decision.should_forward());
    assert(decision.delay_ns == 2'000'000);
}

void test_disabled_rule_is_ignored() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_tcp_frame(frame, 12345, 8080, "order=buy");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto rule = base_rule();
    rule.dst_port = 8080;
    rule.enabled = 0;

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto decision = decide_with(packet, rules);

    assert(!decision.matched());
    assert(decision.action == chronosx::PacketAction::Pass);
}

void test_probability_zero_shadows_later_rules() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_tcp_frame(frame, 12345, 8080, "order=buy");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto pass_rule = base_rule();
    pass_rule.id = 1;
    pass_rule.dst_port = 8080;
    pass_rule.probability = 0;

    auto fallback_drop = base_rule();
    fallback_drop.id = 2;
    fallback_drop.dst_port = 0;
    fallback_drop.probability = chronosx::kProbabilityScale;

    const std::array<chronosx::ChaosRule, 2> rules{pass_rule, fallback_drop};
    const auto decision = decide_with(packet, rules);

    assert(decision.matched());
    assert(decision.rule_id == 1);
    assert(decision.action == chronosx::PacketAction::Pass);
    assert(decision.should_forward());
}

void test_deterministic_probability_decision() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_tcp_frame(frame, 12345, 8080, "same payload");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto rule = base_rule();
    rule.dst_port = 8080;
    rule.probability = 4'000;

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto first = decide_with(packet, rules);

    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto next = decide_with(packet, rules);
        assert(next.action == first.action);
        assert(next.hash == first.hash);
        assert(next.roll == first.roll);
        assert(next.rule_id == first.rule_id);
    }
}

void test_payload_changes_hash() {
    std::array<chronosx::Byte, 160> first_frame{};
    std::array<chronosx::Byte, 160> second_frame{};
    const std::size_t first_length = build_tcp_frame(first_frame, 12345, 8080, "alpha");
    const std::size_t second_length = build_tcp_frame(second_frame, 12345, 8080, "bravo");
    const auto first_packet = chronosx::parse_packet(first_frame.data(), first_length);
    const auto second_packet = chronosx::parse_packet(second_frame.data(), second_length);
    assert(first_packet.ok());
    assert(second_packet.ok());

    auto rule = base_rule();
    rule.dst_port = 8080;
    rule.probability = 5'000;

    const std::uint64_t first_hash = chronosx::stable_packet_hash(rule, first_packet);
    const std::uint64_t second_hash = chronosx::stable_packet_hash(rule, second_packet);

    assert(first_hash != second_hash);
}

void test_probability_clamps_to_always_apply() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_tcp_frame(frame, 12345, 8080, "order=buy");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto rule = base_rule();
    rule.dst_port = 8080;
    rule.probability = 65'000;

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto decision = decide_with(packet, rules);

    assert(decision.matched());
    assert(decision.should_drop());
}

void test_corrupt_decision_forwards_packet() {
    std::array<chronosx::Byte, 160> frame{};
    const std::size_t length = build_tcp_frame(frame, 12345, 8080, "payload");
    const auto packet = chronosx::parse_packet(frame.data(), length);
    assert(packet.ok());

    auto rule = base_rule();
    rule.dst_port = 8080;
    rule.action = chronosx::PacketAction::Corrupt;

    const std::array<chronosx::ChaosRule, 1> rules{rule};
    const auto decision = decide_with(packet, rules);

    assert(decision.matched());
    assert(decision.should_corrupt());
    assert(decision.should_forward());
}

void test_malformed_packet_never_matches() {
    std::array<chronosx::Byte, 16> frame{};
    write_ethernet_header(frame.data(), chronosx::kEtherTypeARP);
    const auto packet = chronosx::parse_packet(frame.data(), frame.size());
    assert(!packet.ok());

    const std::array<chronosx::ChaosRule, 1> rules{base_rule()};
    const auto decision = decide_with(packet, rules);

    assert(!decision.matched());
    assert(decision.action == chronosx::PacketAction::Pass);
}

}

int main() {
    test_exact_tcp_drop_match();
    test_non_matching_rule_passes_without_match();
    test_udp_delay_with_wildcards();
    test_disabled_rule_is_ignored();
    test_probability_zero_shadows_later_rules();
    test_deterministic_probability_decision();
    test_payload_changes_hash();
    test_probability_clamps_to_always_apply();
    test_corrupt_decision_forwards_packet();
    test_malformed_packet_never_matches();

    std::cout << "chaos engine tests passed\n";
    return 0;
}
