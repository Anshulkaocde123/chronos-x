#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace chronosx {

inline constexpr std::size_t kCacheLineSize = 64;
inline constexpr std::size_t kDefaultStatsQueueCapacity = 4096;
inline constexpr std::size_t kEthernetHeaderBytes = 14;
inline constexpr std::size_t kIPv4BaseHeaderBytes = 20;
inline constexpr std::size_t kTCPBaseHeaderBytes = 20;
inline constexpr std::size_t kUDPHeaderBytes = 8;
inline constexpr std::size_t kMaxEthernetFrameBytes = 1518;
inline constexpr std::size_t kMaxVlanEthernetFrameBytes = 1522;

[[nodiscard]] constexpr bool is_power_of_two(const std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

static_assert(is_power_of_two(kDefaultStatsQueueCapacity));
static_assert(is_power_of_two(kCacheLineSize));

using Byte = std::uint8_t;
using PacketLength = std::uint32_t;
using PacketCount = std::uint64_t;
using ByteCount = std::uint64_t;
using Nanoseconds = std::uint64_t;
using CycleCount = std::uint64_t;

static_assert(sizeof(Byte) == 1);
static_assert(sizeof(PacketLength) == 4);
static_assert(sizeof(PacketCount) == 8);
static_assert(sizeof(ByteCount) == 8);
static_assert(sizeof(Nanoseconds) == 8);
static_assert(sizeof(CycleCount) == 8);

enum class PacketAction : std::uint8_t {
    Pass = 0,
    Drop = 1,
    Delay = 2,
    Corrupt = 3,
};

[[nodiscard]] constexpr std::uint8_t to_wire_value(const PacketAction action) noexcept {
    return static_cast<std::uint8_t>(action);
}

struct StatUpdate {
    CycleCount timestamp_cycles{};
    PacketCount packets_seen{};
    PacketCount packets_dropped{};
    ByteCount bytes_seen{};
    Nanoseconds average_latency_ns{};
    PacketAction last_action{PacketAction::Pass};
    std::uint8_t reserved0{};
    std::uint16_t reserved1{};
};

static_assert(sizeof(StatUpdate) == 48);
static_assert(alignof(StatUpdate) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<StatUpdate>);
static_assert(std::is_standard_layout_v<StatUpdate>);

}  // namespace chronosx
