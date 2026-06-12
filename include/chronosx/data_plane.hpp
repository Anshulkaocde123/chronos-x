#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

#include "chronosx/chaos_engine.hpp"
#include "chronosx/packet_parser.hpp"
#include "chronosx/timing_wheel.hpp"
#include "chronosx/types.hpp"

namespace chronosx {

inline constexpr std::size_t kDataPlaneBatchSize = 64;
inline constexpr Nanoseconds kDefaultTimingWheelTickNs = 1'000'000;

struct PacketDescriptor {
    std::uint64_t frame_address{};
    PacketLength frame_length{};
    std::uint32_t options{};
};

static_assert(sizeof(PacketDescriptor) == 16);
static_assert(alignof(PacketDescriptor) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<PacketDescriptor>);
static_assert(std::is_standard_layout_v<PacketDescriptor>);

struct ProcessedFrame {
    std::uint64_t frame_address{};
    PacketLength frame_length{};
    std::uint32_t rule_id{};
    PacketAction action{PacketAction::Pass};
    std::uint8_t reserved[7]{};
};

static_assert(sizeof(ProcessedFrame) == 24);
static_assert(alignof(ProcessedFrame) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<ProcessedFrame>);
static_assert(std::is_standard_layout_v<ProcessedFrame>);

struct DataPlaneBatchStats {
    PacketCount packets_seen{};
    PacketCount packets_dropped{};
    PacketCount packets_delayed{};
    PacketCount packets_corrupted{};
    PacketCount packets_malformed{};
    PacketCount tx_frames{};
    PacketCount recycle_frames{};
    PacketCount schedule_failures{};
    PacketCount output_overflows{};
    PacketCount invalid_descriptors{};
    ByteCount bytes_seen{};
};

static_assert(std::is_trivially_copyable_v<DataPlaneBatchStats>);
static_assert(std::is_standard_layout_v<DataPlaneBatchStats>);

struct DataPlaneBatchResult {
    DataPlaneBatchStats stats{};
    std::size_t tx_count{};
    std::size_t recycle_count{};
};

[[nodiscard]] inline constexpr std::uint64_t delay_ns_to_ticks(const Nanoseconds delay_ns,
                                                               const Nanoseconds tick_ns) noexcept {
    if (delay_ns == 0) {
        return 1;
    }

    if (tick_ns == 0) {
        return delay_ns;
    }

    return (delay_ns + tick_ns - 1) / tick_ns;
}

[[nodiscard]] inline bool swap_ethernet_macs(std::span<Byte> frame) noexcept {
    if (frame.size() < kEthernetHeaderBytes) {
        return false;
    }

    for (std::size_t index = 0; index < 6; ++index) {
        const Byte temporary = frame[index];
        frame[index] = frame[index + 6];
        frame[index + 6] = temporary;
    }

    return true;
}

[[nodiscard]] inline bool corrupt_packet_payload(Byte* frame,
                                                 const PacketView& packet,
                                                 const std::uint64_t selector) noexcept {
    if (frame == nullptr || !packet.ok() || packet.payload_length == 0 ||
        packet.payload_offset >= packet.frame_length) {
        return false;
    }

    const std::size_t byte_offset = packet.payload_offset + (selector % packet.payload_length);
    if (byte_offset >= packet.frame_length) {
        return false;
    }

    frame[byte_offset] ^= 0x01U;
    return true;
}

[[nodiscard]] inline constexpr bool descriptor_within_umem(const PacketDescriptor descriptor,
                                                           const std::size_t umem_size_bytes) noexcept {
    if (descriptor.frame_address > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }

    const auto address = static_cast<std::size_t>(descriptor.frame_address);
    if (address > umem_size_bytes) {
        return false;
    }

    return static_cast<std::size_t>(descriptor.frame_length) <= umem_size_bytes - address;
}

[[nodiscard]] inline bool append_recycle(const ProcessedFrame processed,
                                         std::span<ProcessedFrame> recycle_output,
                                         DataPlaneBatchResult& result) noexcept {
    if (result.recycle_count >= recycle_output.size()) {
        ++result.stats.output_overflows;
        return false;
    }

    recycle_output[result.recycle_count] = processed;
    ++result.recycle_count;
    ++result.stats.recycle_frames;
    return true;
}

template <typename TimingWheelT>
[[nodiscard]] inline DataPlaneBatchResult process_packet_batch(Byte* umem_base,
                                                               const std::size_t umem_size_bytes,
                                                               std::span<const PacketDescriptor> rx_descriptors,
                                                               std::span<const ChaosRule> rules,
                                                               TimingWheelT& timing_wheel,
                                                               std::span<ProcessedFrame> tx_output,
                                                               std::span<ProcessedFrame> recycle_output,
                                                               const Nanoseconds tick_ns =
                                                                   kDefaultTimingWheelTickNs) noexcept {
    DataPlaneBatchResult result{};

    for (const PacketDescriptor descriptor : rx_descriptors) {
        ++result.stats.packets_seen;
        result.stats.bytes_seen += descriptor.frame_length;

        const bool descriptor_valid = umem_base != nullptr &&
                                      descriptor_within_umem(descriptor, umem_size_bytes);
        Byte* frame = descriptor_valid ? umem_base + descriptor.frame_address : nullptr;
        const PacketView packet = parse_packet(frame, descriptor.frame_length);
        ChaosDecision decision = pass_decision();

        if (packet.ok()) {
            decision = decide_packet_fast(packet, rules);
        }

        const ProcessedFrame processed{
            .frame_address = descriptor.frame_address,
            .frame_length = descriptor.frame_length,
            .rule_id = decision.rule_id,
            .action = decision.action,
            .reserved = {},
        };

        if (!descriptor_valid || !packet.ok()) {
            ++result.stats.packets_malformed;
            if (!descriptor_valid) {
                ++result.stats.invalid_descriptors;
            }

            ++result.stats.packets_dropped;
            (void)append_recycle(processed, recycle_output, result);
            continue;
        }

        if (decision.should_drop()) {
            ++result.stats.packets_dropped;
            (void)append_recycle(processed, recycle_output, result);
            continue;
        }

        if (decision.should_delay()) {
            DelayedFrame delayed{
                .frame_address = descriptor.frame_address,
                .frame_length = descriptor.frame_length,
                .rule_id = decision.rule_id,
                .release_tick = 0,
            };

            const std::uint64_t delay_ticks = delay_ns_to_ticks(decision.delay_ns, tick_ns);
            const ScheduleStatus status = timing_wheel.schedule(delayed, delay_ticks);
            if (status == ScheduleStatus::Scheduled) {
                ++result.stats.packets_delayed;
                continue;
            }

            ++result.stats.schedule_failures;
            ++result.stats.packets_dropped;
            (void)append_recycle(processed, recycle_output, result);

            continue;
        }

        if (decision.should_corrupt() && corrupt_packet_payload(frame, packet, decision.hash)) {
            ++result.stats.packets_corrupted;
        }

        if (frame != nullptr) {
            (void)swap_ethernet_macs(std::span<Byte>{frame, descriptor.frame_length});
        }

        if (result.tx_count < tx_output.size()) {
            tx_output[result.tx_count] = processed;
            ++result.tx_count;
            ++result.stats.tx_frames;
        } else {
            ++result.stats.output_overflows;
            ++result.stats.packets_dropped;
            (void)append_recycle(processed, recycle_output, result);
        }
    }

    return result;
}

template <typename TimingWheelT>
[[nodiscard]] inline DataPlaneBatchResult process_packet_batch(Byte* umem_base,
                                                               std::span<const PacketDescriptor> rx_descriptors,
                                                               std::span<const ChaosRule> rules,
                                                               TimingWheelT& timing_wheel,
                                                               std::span<ProcessedFrame> tx_output,
                                                               std::span<ProcessedFrame> recycle_output,
                                                               const Nanoseconds tick_ns =
                                                                   kDefaultTimingWheelTickNs) noexcept {
    const std::size_t unbounded_umem = std::numeric_limits<std::size_t>::max();
    return process_packet_batch(umem_base,
                                unbounded_umem,
                                rx_descriptors,
                                rules,
                                timing_wheel,
                                tx_output,
                                recycle_output,
                                tick_ns);
}

}  // namespace chronosx
