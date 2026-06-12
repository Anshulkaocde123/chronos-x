#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

#include "chronosx/protocol.hpp"

namespace {

template <std::size_t Size>
std::span<const chronosx::Byte> written_bytes(const std::array<chronosx::Byte, Size>& buffer,
                                              const std::size_t bytes_written) {
    return std::span<const chronosx::Byte>{buffer.data(), bytes_written};
}

chronosx::ChaosRule make_rule(const std::uint32_t id, const std::uint16_t dst_port) {
    return chronosx::ChaosRule{
        .seed = 1234,
        .delay_ns = 0,
        .id = id,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = dst_port,
        .probability = chronosx::kProbabilityScale,
        .protocol = chronosx::kIPv4ProtocolTCP,
        .action = chronosx::PacketAction::Drop,
        .enabled = 1,
        .reserved = {},
    };
}

void test_encode_decode_ping() {
    std::array<chronosx::Byte, 256> buffer{};
    const chronosx::MessageHeader header{
        .opcode = chronosx::Opcode::Ping,
        .sequence = 99,
        .timestamp_ns = 123456,
    };

    const auto encoded = chronosx::encode_message(header, std::span<const chronosx::Byte>{}, buffer);
    assert(encoded.status == chronosx::ProtocolStatus::Ok);
    assert(encoded.bytes_written == chronosx::kProtocolHeaderBytes);

    const auto decoded = chronosx::decode_message(written_bytes(buffer, encoded.bytes_written));
    assert(decoded.status == chronosx::ProtocolStatus::Ok);
    assert(decoded.message.header.magic == chronosx::kProtocolMagic);
    assert(decoded.message.header.version == chronosx::kProtocolVersion);
    assert(decoded.message.header.opcode == chronosx::Opcode::Ping);
    assert(decoded.message.header.sequence == 99);
    assert(decoded.message.header.timestamp_ns == 123456);
    assert(decoded.message.payload.empty());
}

void test_decode_rejects_crc_mutation() {
    std::array<chronosx::Byte, 256> buffer{};
    const std::array<chronosx::Byte, 4> payload{1, 2, 3, 4};
    const auto encoded = chronosx::encode_message(chronosx::MessageHeader{.opcode = chronosx::Opcode::Ping},
                                                  std::span<const chronosx::Byte>{payload},
                                                  buffer);
    assert(encoded.status == chronosx::ProtocolStatus::Ok);

    buffer[chronosx::kProtocolHeaderBytes + 1] ^= 0xFFU;
    const auto decoded = chronosx::decode_message(written_bytes(buffer, encoded.bytes_written));
    assert(decoded.status == chronosx::ProtocolStatus::CrcMismatch);
}

void test_control_core_add_list_remove_clear() {
    chronosx::ControlCore<4> control;
    std::array<chronosx::Byte, 1024> request{};
    std::array<chronosx::Byte, 1024> response{};

    const chronosx::ChaosRule first_rule = make_rule(7, 8080);
    const auto add_encoded = chronosx::encode_message(
        chronosx::MessageHeader{.opcode = chronosx::Opcode::AddRule, .sequence = 1},
        std::span<const chronosx::Byte>{reinterpret_cast<const chronosx::Byte*>(&first_rule), sizeof(first_rule)},
        request);
    assert(add_encoded.status == chronosx::ProtocolStatus::Ok);

    const auto add_response = control.handle(written_bytes(request, add_encoded.bytes_written), response);
    assert(add_response.status == chronosx::ProtocolStatus::Ok);
    assert(add_response.opcode == chronosx::Opcode::Pong);
    assert(control.rule_count() == 1);
    assert(control.rules()[0].dst_port == 8080);

    const auto list_encoded = chronosx::encode_message(
        chronosx::MessageHeader{.opcode = chronosx::Opcode::ListRules, .sequence = 2},
        std::span<const chronosx::Byte>{},
        request);
    assert(list_encoded.status == chronosx::ProtocolStatus::Ok);

    const auto list_response = control.handle(written_bytes(request, list_encoded.bytes_written), response);
    assert(list_response.status == chronosx::ProtocolStatus::Ok);
    assert(list_response.opcode == chronosx::Opcode::RulesResponse);

    const auto decoded_list = chronosx::decode_message(written_bytes(response, list_response.bytes_written));
    assert(decoded_list.status == chronosx::ProtocolStatus::Ok);
    assert(decoded_list.message.payload.size() == sizeof(chronosx::ChaosRule));

    chronosx::ChaosRule listed_rule{};
    std::memcpy(&listed_rule, decoded_list.message.payload.data(), sizeof(listed_rule));
    assert(listed_rule.id == 7);
    assert(listed_rule.dst_port == 8080);

    std::array<chronosx::Byte, sizeof(std::uint32_t)> remove_payload{};
    chronosx::write_u32_be(std::span<chronosx::Byte>{remove_payload}, 0, 0);
    const auto remove_encoded = chronosx::encode_message(
        chronosx::MessageHeader{.opcode = chronosx::Opcode::RemoveRule, .sequence = 3},
        std::span<const chronosx::Byte>{remove_payload},
        request);
    assert(remove_encoded.status == chronosx::ProtocolStatus::Ok);

    const auto remove_response = control.handle(written_bytes(request, remove_encoded.bytes_written), response);
    assert(remove_response.status == chronosx::ProtocolStatus::Ok);
    assert(control.rule_count() == 0);

    const auto clear_encoded = chronosx::encode_message(
        chronosx::MessageHeader{.opcode = chronosx::Opcode::ClearRules, .sequence = 4},
        std::span<const chronosx::Byte>{},
        request);
    assert(clear_encoded.status == chronosx::ProtocolStatus::Ok);
    const auto clear_response = control.handle(written_bytes(request, clear_encoded.bytes_written), response);
    assert(clear_response.status == chronosx::ProtocolStatus::Ok);
    assert(clear_response.opcode == chronosx::Opcode::Pong);
    assert(control.rule_count() == 0);
}

void test_control_core_stats_response() {
    chronosx::ControlCore<4> control;
    const chronosx::StatUpdate stats{
        .timestamp_cycles = 100,
        .packets_seen = 42,
        .packets_dropped = 3,
        .bytes_seen = 4096,
        .average_latency_ns = 80,
        .last_action = chronosx::PacketAction::Drop,
        .reserved0 = 0,
        .reserved1 = 0,
    };
    control.publish_stats(stats);

    std::array<chronosx::Byte, 1024> request{};
    std::array<chronosx::Byte, 1024> response{};
    const auto encoded = chronosx::encode_message(
        chronosx::MessageHeader{.opcode = chronosx::Opcode::GetStats, .sequence = 5},
        std::span<const chronosx::Byte>{},
        request);
    assert(encoded.status == chronosx::ProtocolStatus::Ok);

    const auto control_response = control.handle(written_bytes(request, encoded.bytes_written), response);
    assert(control_response.status == chronosx::ProtocolStatus::Ok);
    assert(control_response.opcode == chronosx::Opcode::StatsResponse);

    const auto decoded = chronosx::decode_message(written_bytes(response, control_response.bytes_written));
    assert(decoded.status == chronosx::ProtocolStatus::Ok);
    assert(decoded.message.payload.size() == sizeof(chronosx::StatUpdate));

    chronosx::StatUpdate round_trip{};
    std::memcpy(&round_trip, decoded.message.payload.data(), sizeof(round_trip));
    assert(round_trip.packets_seen == 42);
    assert(round_trip.packets_dropped == 3);
    assert(round_trip.bytes_seen == 4096);
    assert(round_trip.last_action == chronosx::PacketAction::Drop);
}

void test_rule_table_capacity_error() {
    chronosx::ControlCore<1> control;
    std::array<chronosx::Byte, 1024> request{};
    std::array<chronosx::Byte, 1024> response{};

    for (std::uint32_t id = 0; id < 2; ++id) {
        const chronosx::ChaosRule rule = make_rule(id, static_cast<std::uint16_t>(9000 + id));
        const auto encoded = chronosx::encode_message(
            chronosx::MessageHeader{.opcode = chronosx::Opcode::AddRule, .sequence = id + 1},
            std::span<const chronosx::Byte>{reinterpret_cast<const chronosx::Byte*>(&rule), sizeof(rule)},
            request);
        assert(encoded.status == chronosx::ProtocolStatus::Ok);

        const auto control_response = control.handle(written_bytes(request, encoded.bytes_written), response);
        if (id == 0) {
            assert(control_response.status == chronosx::ProtocolStatus::Ok);
        } else {
            assert(control_response.status == chronosx::ProtocolStatus::RuleTableFull);
            assert(control_response.opcode == chronosx::Opcode::Error);
        }
    }
}

}  // namespace

int main() {
    test_encode_decode_ping();
    test_decode_rejects_crc_mutation();
    test_control_core_add_list_remove_clear();
    test_control_core_stats_response();
    test_rule_table_capacity_error();

    std::cout << "protocol tests passed\n";
    return 0;
}
