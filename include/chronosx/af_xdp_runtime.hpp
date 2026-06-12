#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "chronosx/data_plane.hpp"
#include "chronosx/frame_allocator.hpp"
#include "chronosx/socket_manager.hpp"
#include "chronosx/timing_wheel.hpp"
#include "chronosx/types.hpp"

namespace chronosx {

enum class RuntimeStatus : std::uint8_t {
    Ok = 0,
    AlreadyInitialized = 1,
    NotInitialized = 2,
    FrameAllocationFailed = 3,
    InvalidFrame = 4,
    InvalidOwner = 5,
    RingFull = 6,
};

struct RuntimePollStats {
    PacketCount rx_frames{};
    PacketCount tx_submitted{};
    PacketCount tx_completed{};
    PacketCount fill_refilled{};
    PacketCount fill_failures{};
    PacketCount owner_failures{};
    DataPlaneBatchStats data_plane{};
};

struct RuntimePollResult {
    RuntimeStatus status{RuntimeStatus::Ok};
    RuntimePollStats stats{};
};

struct RuntimeConfig {
    std::size_t initial_fill_frames{kDefaultFillRingSize};
};

template <std::size_t FrameCount = kDefaultFrameCount,
          std::size_t FrameSize = kDefaultFrameSizeBytes,
          typename SocketT = DefaultSimulatedSocket,
          typename TimingWheelT = DefaultTimingWheel>
    requires(FrameCount > 0 && is_power_of_two(FrameSize))
class AfXdpRuntimeAdapter {
public:
    AfXdpRuntimeAdapter() = default;

    AfXdpRuntimeAdapter(const AfXdpRuntimeAdapter&) = delete;
    AfXdpRuntimeAdapter& operator=(const AfXdpRuntimeAdapter&) = delete;
    AfXdpRuntimeAdapter(AfXdpRuntimeAdapter&&) = delete;
    AfXdpRuntimeAdapter& operator=(AfXdpRuntimeAdapter&&) = delete;

    [[nodiscard]] RuntimeStatus init(const RuntimeConfig& config = {}) noexcept {
        if (initialized_) {
            return RuntimeStatus::AlreadyInitialized;
        }

        const std::size_t fill_target = config.initial_fill_frames < FrameCount
                                            ? config.initial_fill_frames
                                            : FrameCount;

        for (std::size_t index = 0; index < fill_target; ++index) {
            const FrameAllocation allocation = allocator_.allocate(FrameOwner::FillRing);
            if (!allocation.ok()) {
                return RuntimeStatus::FrameAllocationFailed;
            }

            const std::array<std::uint64_t, 1> address{allocation.frame.address};
            const FillResult fill = socket_.refill(std::span<const std::uint64_t>{address});
            if (fill.count != 1) {
                (void)allocator_.transition(allocation.frame, FrameOwner::FillRing, FrameOwner::Free);
                return RuntimeStatus::RingFull;
            }
        }

        initialized_ = true;
        return RuntimeStatus::Ok;
    }

    [[nodiscard]] RuntimePollResult poll_once(std::span<const ChaosRule> rules) noexcept {
        RuntimePollResult result{};
        if (!initialized_) {
            result.status = RuntimeStatus::NotInitialized;
            return result;
        }

        drain_completions(result.stats);
        release_delayed(result.stats);

        std::array<RxDescriptor, kDataPlaneBatchSize> rx{};
        const RxBatchResult rx_result = socket_.consume_rx(std::span<RxDescriptor>{rx});
        (void)rx_result.status;
        result.stats.rx_frames = rx_result.count;

        std::array<PacketDescriptor, kDataPlaneBatchSize> descriptors{};
        std::size_t descriptor_count = 0;
        for (std::size_t index = 0; index < rx_result.count; ++index) {
            const FrameAllocation frame = allocator_.handle_from_address(rx[index].address);
            if (!frame.ok()) {
                ++result.stats.owner_failures;
                continue;
            }

            const FrameOwner owner = allocator_.owner_of(frame.frame.index);
            const FrameStatus transition = allocator_.transition(frame.frame, owner, FrameOwner::DataPlane);
            if (transition != FrameStatus::Ok) {
                ++result.stats.owner_failures;
                continue;
            }

            descriptors[descriptor_count] = PacketDescriptor{
                .frame_address = rx[index].address,
                .frame_length = rx[index].length,
                .options = rx[index].options,
            };
            ++descriptor_count;
        }

        std::array<ProcessedFrame, kDataPlaneBatchSize> tx{};
        std::array<ProcessedFrame, kDataPlaneBatchSize> recycle{};
        const DataPlaneBatchResult batch = process_packet_batch(
            umem_.data(),
            umem_.size(),
            std::span<const PacketDescriptor>{descriptors.data(), descriptor_count},
            rules,
            timing_wheel_,
            std::span<ProcessedFrame>{tx},
            std::span<ProcessedFrame>{recycle});
        result.stats.data_plane = batch.stats;

        submit_tx(std::span<const ProcessedFrame>{tx.data(), batch.tx_count}, result.stats);
        refill_recycled(std::span<const ProcessedFrame>{recycle.data(), batch.recycle_count}, result.stats);
        return result;
    }

    [[nodiscard]] Byte* umem_data() noexcept {
        return umem_.data();
    }

    [[nodiscard]] constexpr std::size_t umem_size_bytes() const noexcept {
        return umem_.size();
    }

    [[nodiscard]] SocketT& socket() noexcept {
        return socket_;
    }

    [[nodiscard]] FrameAllocator<FrameCount, FrameSize>& allocator() noexcept {
        return allocator_;
    }

    [[nodiscard]] TimingWheelT& timing_wheel() noexcept {
        return timing_wheel_;
    }

private:
    void drain_completions(RuntimePollStats& stats) noexcept {
        std::array<std::uint64_t, kDataPlaneBatchSize> completed{};
        const CompletionResult result = socket_.drain_completions(std::span<std::uint64_t>{completed});
        stats.tx_completed += result.count;

        for (std::size_t index = 0; index < result.count; ++index) {
            const FrameAllocation frame = allocator_.handle_from_address(completed[index]);
            if (!frame.ok()) {
                ++stats.owner_failures;
                continue;
            }
            refill_one(frame.frame, FrameOwner::TxRing, stats);
        }
    }

    void release_delayed(RuntimePollStats& stats) noexcept {
        const std::span<const DelayedFrame> expired = timing_wheel_.tick();
        for (const DelayedFrame frame : expired) {
            const ProcessedFrame processed{
                .frame_address = frame.frame_address,
                .frame_length = frame.frame_length,
                .rule_id = frame.rule_id,
                .action = PacketAction::Delay,
                .reserved = {},
            };
            submit_tx(std::span<const ProcessedFrame>{&processed, 1}, stats);
        }
    }

    void submit_tx(std::span<const ProcessedFrame> frames, RuntimePollStats& stats) noexcept {
        for (const ProcessedFrame frame : frames) {
            const FrameAllocation handle = allocator_.handle_from_address(frame.frame_address);
            if (!handle.ok()) {
                ++stats.owner_failures;
                continue;
            }

            const FrameOwner owner = allocator_.owner_of(handle.frame.index);
            if (allocator_.transition(handle.frame, owner, FrameOwner::TxRing) != FrameStatus::Ok) {
                ++stats.owner_failures;
                continue;
            }

            const std::array<TxDescriptor, 1> tx{TxDescriptor{
                .address = frame.frame_address,
                .length = frame.frame_length,
                .options = 0,
            }};
            const TxBatchResult result = socket_.submit_tx(std::span<const TxDescriptor>{tx});
            if (result.count == 1) {
                ++stats.tx_submitted;
                (void)socket_.kick_tx();
                continue;
            }

            ++stats.fill_failures;
            (void)allocator_.transition(handle.frame, FrameOwner::TxRing, owner);
        }
    }

    void refill_recycled(std::span<const ProcessedFrame> frames, RuntimePollStats& stats) noexcept {
        for (const ProcessedFrame frame : frames) {
            const FrameAllocation handle = allocator_.handle_from_address(frame.frame_address);
            if (!handle.ok()) {
                ++stats.owner_failures;
                continue;
            }
            refill_one(handle.frame, FrameOwner::DataPlane, stats);
        }
    }

    void refill_one(const FrameHandle frame, const FrameOwner expected_owner, RuntimePollStats& stats) noexcept {
        if (allocator_.transition(frame, expected_owner, FrameOwner::FillRing) != FrameStatus::Ok) {
            ++stats.owner_failures;
            return;
        }

        const std::array<std::uint64_t, 1> address{frame.address};
        const FillResult fill = socket_.refill(std::span<const std::uint64_t>{address});
        if (fill.count == 1) {
            ++stats.fill_refilled;
            return;
        }

        ++stats.fill_failures;
        (void)allocator_.transition(frame, FrameOwner::FillRing, expected_owner);
    }

    std::array<Byte, FrameCount * FrameSize> umem_{};
    FrameAllocator<FrameCount, FrameSize> allocator_{};
    SocketT socket_{};
    TimingWheelT timing_wheel_{};
    bool initialized_{};
};

using DefaultAfXdpRuntimeAdapter = AfXdpRuntimeAdapter<>;

static_assert(!std::is_copy_constructible_v<DefaultAfXdpRuntimeAdapter>);

}  // namespace chronosx
