#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>

#include "chronosx/types.hpp"

namespace {

void test_fixed_width_type_sizes() {
    static_assert(sizeof(chronosx::Byte) == 1);
    static_assert(sizeof(chronosx::PacketLength) == 4);
    static_assert(sizeof(chronosx::PacketCount) == 8);
    static_assert(sizeof(chronosx::ByteCount) == 8);
    static_assert(sizeof(chronosx::Nanoseconds) == 8);
    static_assert(sizeof(chronosx::CycleCount) == 8);

    assert(sizeof(chronosx::Byte) == 1);
    assert(sizeof(chronosx::PacketLength) == 4);
    assert(sizeof(chronosx::PacketCount) == 8);
}

void test_stat_update_layout_is_wire_safe() {
    static_assert(std::is_trivially_copyable_v<chronosx::StatUpdate>);
    static_assert(std::is_standard_layout_v<chronosx::StatUpdate>);
    static_assert(sizeof(chronosx::StatUpdate) == 48);
    static_assert(alignof(chronosx::StatUpdate) == alignof(std::uint64_t));

    static_assert(offsetof(chronosx::StatUpdate, timestamp_cycles) == 0);
    static_assert(offsetof(chronosx::StatUpdate, packets_seen) == 8);
    static_assert(offsetof(chronosx::StatUpdate, packets_dropped) == 16);
    static_assert(offsetof(chronosx::StatUpdate, bytes_seen) == 24);
    static_assert(offsetof(chronosx::StatUpdate, average_latency_ns) == 32);
    static_assert(offsetof(chronosx::StatUpdate, last_action) == 40);
    static_assert(offsetof(chronosx::StatUpdate, reserved0) == 41);
    static_assert(offsetof(chronosx::StatUpdate, reserved1) == 42);

    const chronosx::StatUpdate update{
        .timestamp_cycles = 100,
        .packets_seen = 10,
        .packets_dropped = 1,
        .bytes_seen = 640,
        .average_latency_ns = 42,
        .last_action = chronosx::PacketAction::Delay,
        .reserved0 = 0,
        .reserved1 = 0,
    };

    assert(update.timestamp_cycles == 100);
    assert(update.last_action == chronosx::PacketAction::Delay);
}

void test_packet_action_wire_values_are_explicit() {
    assert(chronosx::to_wire_value(chronosx::PacketAction::Pass) == 0);
    assert(chronosx::to_wire_value(chronosx::PacketAction::Drop) == 1);
    assert(chronosx::to_wire_value(chronosx::PacketAction::Delay) == 2);
    assert(chronosx::to_wire_value(chronosx::PacketAction::Corrupt) == 3);
}

void test_pointer_arithmetic_scales_by_pointee_type() {
    std::array<std::uint32_t, 4> words{10, 20, 30, 40};

    const auto word0 = reinterpret_cast<std::uintptr_t>(&words[0]);
    const auto word1 = reinterpret_cast<std::uintptr_t>(&words[1]);

    assert(words.data() + 1 == &words[1]);
    assert(word1 - word0 == sizeof(std::uint32_t));
}

void test_packet_offsets_are_byte_offsets() {
    std::array<chronosx::Byte, 64> frame{};

    chronosx::Byte* ethernet = frame.data();
    chronosx::Byte* ipv4 = ethernet + chronosx::kEthernetHeaderBytes;
    chronosx::Byte* tcp = ipv4 + chronosx::kIPv4BaseHeaderBytes;

    assert(ipv4 - frame.data() == static_cast<std::ptrdiff_t>(chronosx::kEthernetHeaderBytes));
    assert(tcp - frame.data() == static_cast<std::ptrdiff_t>(chronosx::kEthernetHeaderBytes +
                                                            chronosx::kIPv4BaseHeaderBytes));
}

}  // namespace

int main() {
    test_fixed_width_type_sizes();
    test_stat_update_layout_is_wire_safe();
    test_packet_action_wire_values_are_explicit();
    test_pointer_arithmetic_scales_by_pointee_type();
    test_packet_offsets_are_byte_offsets();

    std::cout << "types and layout tests passed\n";
    return 0;
}
