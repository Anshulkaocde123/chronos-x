#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>

#include "chronosx/control_plane.hpp"
#include "chronosx/protocol.hpp"
#include "chronosx/types.hpp"

namespace {

// Helper: encode a control message into a buffer.
std::size_t encode_request(const chronosx::Opcode opcode,
                           const std::uint32_t sequence,
                           std::span<const chronosx::Byte> payload,
                           std::span<chronosx::Byte> output) {
    const chronosx::MessageHeader header{.opcode = opcode, .sequence = sequence};
    const auto result = chronosx::encode_message(header, payload, output);
    assert(result.status == chronosx::ProtocolStatus::Ok);
    return result.bytes_written;
}

void test_connection_buffer_basic() {
    chronosx::ConnectionBuffer buffer;

    assert(buffer.readable_bytes() == 0);
    assert(!buffer.has_complete_message());

    std::array<chronosx::Byte, 128> msg{};
    const std::size_t msg_len = encode_request(
        chronosx::Opcode::Ping, 1, std::span<const chronosx::Byte>{},
        std::span<chronosx::Byte>{msg});

    const std::size_t appended = buffer.append(
        std::span<const chronosx::Byte>{msg.data(), msg_len});
    assert(appended == msg_len);
    assert(buffer.readable_bytes() == msg_len);
    assert(buffer.has_complete_message());
    assert(buffer.next_message_length() == msg_len);
}

void test_connection_buffer_partial() {
    chronosx::ConnectionBuffer buffer;

    std::array<chronosx::Byte, 128> msg{};
    const std::size_t msg_len = encode_request(
        chronosx::Opcode::Ping, 1, std::span<const chronosx::Byte>{},
        std::span<chronosx::Byte>{msg});

    // Feed only 10 bytes (partial header)
    (void)buffer.append(std::span<const chronosx::Byte>{msg.data(), 10});
    assert(!buffer.has_complete_message());

    // Feed the rest
    (void)buffer.append(std::span<const chronosx::Byte>{msg.data() + 10, msg_len - 10});
    assert(buffer.has_complete_message());
    assert(buffer.next_message_length() == msg_len);
}

void test_connection_buffer_consume() {
    chronosx::ConnectionBuffer buffer;

    std::array<chronosx::Byte, 256> msg1{};
    const std::size_t len1 = encode_request(
        chronosx::Opcode::Ping, 1, std::span<const chronosx::Byte>{},
        std::span<chronosx::Byte>{msg1});

    std::array<chronosx::Byte, 256> msg2{};
    const std::size_t len2 = encode_request(
        chronosx::Opcode::GetStats, 2, std::span<const chronosx::Byte>{},
        std::span<chronosx::Byte>{msg2});

    (void)buffer.append(std::span<const chronosx::Byte>{msg1.data(), len1});
    (void)buffer.append(std::span<const chronosx::Byte>{msg2.data(), len2});

    assert(buffer.has_complete_message());
    buffer.consume(len1);
    assert(buffer.has_complete_message());
    assert(buffer.readable_bytes() == len2);
}

void test_connection_buffer_reset() {
    chronosx::ConnectionBuffer buffer;

    std::array<chronosx::Byte, 128> msg{};
    const std::size_t msg_len = encode_request(
        chronosx::Opcode::Ping, 1, std::span<const chronosx::Byte>{},
        std::span<chronosx::Byte>{msg});

    (void)buffer.append(std::span<const chronosx::Byte>{msg.data(), msg_len});
    assert(buffer.readable_bytes() > 0);

    buffer.reset();
    assert(buffer.readable_bytes() == 0);
    assert(!buffer.has_complete_message());
}

void test_connection_buffer_overflow_detection() {
    chronosx::ConnectionBuffer buffer;
    std::array<chronosx::Byte, chronosx::kControlReadBufferBytes + 1> bytes{};

    const std::size_t appended = buffer.append(std::span<const chronosx::Byte>{bytes});
    assert(appended == chronosx::kControlReadBufferBytes);
    assert(buffer.overflowed());

    buffer.reset();
    assert(!buffer.overflowed());
    assert(buffer.readable_bytes() == 0);
}

void test_connection_buffer_oversized_payload_closes_message() {
    chronosx::ConnectionBuffer buffer;
    std::array<chronosx::Byte, chronosx::kProtocolHeaderBytes> header{};

    chronosx::write_u16_be(std::span<chronosx::Byte>{header}, 0, chronosx::kProtocolMagic);
    header[2] = chronosx::kProtocolVersion;
    header[3] = static_cast<chronosx::Byte>(chronosx::Opcode::AddRule);
    chronosx::write_u32_be(std::span<chronosx::Byte>{header}, 8,
                           static_cast<std::uint32_t>(chronosx::kMaxControlPayloadBytes + 1));

    assert(buffer.append(std::span<const chronosx::Byte>{header}) == header.size());
    assert(buffer.has_complete_message());
    assert(buffer.next_message_length() == header.size());
}

void test_rule_snapshot() {
    const chronosx::RuleSnapshot empty;
    assert(empty.empty());
    assert(empty.size() == 0);

    const std::array<chronosx::ChaosRule, 2> rules{
        chronosx::ChaosRule{.seed = 42, .id = 1, .dst_port = 8080,
                            .probability = 5000,
                            .action = chronosx::PacketAction::Drop,
                            .enabled = 1},
        chronosx::ChaosRule{.seed = 99, .id = 2, .dst_port = 9090,
                            .probability = 10000,
                            .action = chronosx::PacketAction::Delay,
                            .enabled = 1},
    };

    const chronosx::RuleSnapshot snapshot{std::span<const chronosx::ChaosRule>{rules}};
    assert(snapshot.size() == 2);
    assert(!snapshot.empty());
    assert(snapshot.rules()[0].id == 1);
    assert(snapshot.rules()[1].id == 2);
}

void test_atomic_rule_snapshot() {
    chronosx::AtomicRuleSnapshot atomic_snap;
    atomic_snap.store(std::make_shared<const chronosx::RuleSnapshot>());

    auto snap1 = atomic_snap.load(std::memory_order_acquire);
    assert(snap1->empty());

    const std::array<chronosx::ChaosRule, 1> rules{
        chronosx::ChaosRule{.seed = 42, .id = 1},
    };

    auto snap2 = std::make_shared<const chronosx::RuleSnapshot>(
        std::span<const chronosx::ChaosRule>{rules});
    atomic_snap.store(snap2, std::memory_order_release);

    auto loaded = atomic_snap.load(std::memory_order_acquire);
    assert(loaded->size() == 1);
    assert(loaded->rules()[0].id == 1);

    // snap1 still valid (RCU: old readers keep old snapshot alive)
    assert(snap1->empty());
}

void test_control_plane_core_ping() {
    chronosx::ControlPlaneCore core;
    assert(core.rule_count() == 0);

    std::array<chronosx::Byte, 256> request{};
    const std::size_t req_len = encode_request(
        chronosx::Opcode::Ping, 42, std::span<const chronosx::Byte>{},
        std::span<chronosx::Byte>{request});

    std::array<chronosx::Byte, 256> response{};
    const auto result = core.handle_request(
        std::span<const chronosx::Byte>{request.data(), req_len},
        std::span<chronosx::Byte>{response});

    assert(result.status == chronosx::ProtocolStatus::Ok);
    assert(result.bytes_written > 0);

    const auto decoded = chronosx::decode_message(
        std::span<const chronosx::Byte>{response.data(), result.bytes_written});
    assert(decoded.status == chronosx::ProtocolStatus::Ok);
    assert(decoded.message.header.opcode == chronosx::Opcode::Pong);
    assert(decoded.message.header.sequence == 42);
}

void test_control_plane_core_add_rule() {
    chronosx::ControlPlaneCore core;

    const chronosx::ChaosRule rule{
        .seed = 0xBEEF,
        .id = 1,
        .dst_port = 8080,
        .probability = chronosx::kProbabilityScale,
        .protocol = chronosx::kIPv4ProtocolTCP,
        .action = chronosx::PacketAction::Drop,
        .enabled = 1,
    };

    std::array<chronosx::Byte, 512> request{};
    const std::size_t req_len = encode_request(
        chronosx::Opcode::AddRule, 1,
        std::span<const chronosx::Byte>{reinterpret_cast<const chronosx::Byte*>(&rule),
                                         sizeof(rule)},
        std::span<chronosx::Byte>{request});

    std::array<chronosx::Byte, 512> response{};
    const auto result = core.handle_request(
        std::span<const chronosx::Byte>{request.data(), req_len},
        std::span<chronosx::Byte>{response});

    assert(result.status == chronosx::ProtocolStatus::Ok);
    assert(core.rule_count() == 1);
    assert(core.rules()[0].id == 1);
    assert(core.rules()[0].dst_port == 8080);
}

void test_control_plane_core_rule_snapshot_published() {
    chronosx::ControlPlaneCore core;

    auto snap_before = core.current_rules();
    assert(snap_before->empty());

    const chronosx::ChaosRule rule{
        .seed = 42,
        .id = 1,
        .probability = 5000,
        .action = chronosx::PacketAction::Drop,
        .enabled = 1,
    };

    std::array<chronosx::Byte, 512> request{};
    const std::size_t req_len = encode_request(
        chronosx::Opcode::AddRule, 1,
        std::span<const chronosx::Byte>{reinterpret_cast<const chronosx::Byte*>(&rule),
                                         sizeof(rule)},
        std::span<chronosx::Byte>{request});

    std::array<chronosx::Byte, 512> response{};
    (void)core.handle_request(
        std::span<const chronosx::Byte>{request.data(), req_len},
        std::span<chronosx::Byte>{response});

    auto snap_after = core.current_rules();
    assert(snap_after->size() == 1);

    // Old snapshot still valid (RCU guarantee)
    assert(snap_before->empty());
}

void test_control_plane_core_stats() {
    chronosx::ControlPlaneCore core;

    const chronosx::StatUpdate update{
        .timestamp_cycles = 100,
        .packets_seen = 42,
        .packets_dropped = 7,
        .bytes_seen = 42 * 64,
        .average_latency_ns = 150,
        .last_action = chronosx::PacketAction::Drop,
    };
    core.publish_stats(update);

    std::array<chronosx::Byte, 256> request{};
    const std::size_t req_len = encode_request(
        chronosx::Opcode::GetStats, 5, std::span<const chronosx::Byte>{},
        std::span<chronosx::Byte>{request});

    std::array<chronosx::Byte, 256> response{};
    const auto result = core.handle_request(
        std::span<const chronosx::Byte>{request.data(), req_len},
        std::span<chronosx::Byte>{response});

    assert(result.status == chronosx::ProtocolStatus::Ok);

    const auto decoded = chronosx::decode_message(
        std::span<const chronosx::Byte>{response.data(), result.bytes_written});
    assert(decoded.status == chronosx::ProtocolStatus::Ok);
    assert(decoded.message.header.opcode == chronosx::Opcode::StatsResponse);
    assert(decoded.message.payload.size() == sizeof(chronosx::StatUpdate));

    chronosx::StatUpdate received{};
    std::memcpy(&received, decoded.message.payload.data(), sizeof(received));
    assert(received.packets_seen == 42);
    assert(received.packets_dropped == 7);
}

void test_control_plane_status_names() {
    assert(chronosx::control_plane_status_name(chronosx::ControlPlaneStatus::Ok)[0] == 'O');
    assert(chronosx::control_plane_status_name(chronosx::ControlPlaneStatus::BindFailed)[0] == 'B');
    assert(chronosx::control_plane_status_name(chronosx::ControlPlaneStatus::ShutdownRequested)[0] == 'S');
}

void test_control_plane_config_validation() {
    assert(chronosx::ControlPlaneConfig{}.valid());

    chronosx::ControlPlaneConfig too_many{};
    too_many.max_connections = chronosx::kMaxControlConnections + 1;
    assert(!too_many.valid());

    chronosx::ControlPlaneConfig bad_backlog{};
    bad_backlog.listen_backlog = 0;
    assert(!bad_backlog.valid());
}

}  // namespace

int main() {
    test_connection_buffer_basic();
    test_connection_buffer_partial();
    test_connection_buffer_consume();
    test_connection_buffer_reset();
    test_connection_buffer_overflow_detection();
    test_connection_buffer_oversized_payload_closes_message();
    test_rule_snapshot();
    test_atomic_rule_snapshot();
    test_control_plane_core_ping();
    test_control_plane_core_add_rule();
    test_control_plane_core_rule_snapshot_published();
    test_control_plane_core_stats();
    test_control_plane_status_names();
    test_control_plane_config_validation();

    std::cout << "all control plane tests passed\n";
    return 0;
}
