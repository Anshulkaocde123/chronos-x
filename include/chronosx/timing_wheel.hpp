#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "chronosx/types.hpp"

namespace chronosx {

struct DelayedFrame {
    std::uint64_t frame_address{};
    PacketLength frame_length{};
    std::uint32_t rule_id{};
    std::uint64_t release_tick{};
};

static_assert(sizeof(DelayedFrame) == 24);
static_assert(alignof(DelayedFrame) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<DelayedFrame>);
static_assert(std::is_standard_layout_v<DelayedFrame>);

enum class ScheduleStatus : std::uint8_t {
    Scheduled = 0,
    DelayTooLarge = 1,
    SlotFull = 2,
};

[[nodiscard]] constexpr const char* schedule_status_name(const ScheduleStatus status) noexcept {
    switch (status) {
        case ScheduleStatus::Scheduled:
            return "Scheduled";
        case ScheduleStatus::DelayTooLarge:
            return "DelayTooLarge";
        case ScheduleStatus::SlotFull:
            return "SlotFull";
    }

    return "Unknown";
}

template <std::size_t SlotCount, std::size_t MaxFramesPerSlot>
    requires(is_power_of_two(SlotCount) && MaxFramesPerSlot > 0)
class TimingWheel {
public:
    TimingWheel() = default;

    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;
    TimingWheel(TimingWheel&&) = delete;
    TimingWheel& operator=(TimingWheel&&) = delete;

    [[nodiscard]] ScheduleStatus schedule(DelayedFrame frame, std::uint64_t delay_ticks) noexcept {
        if (delay_ticks > SlotCount) {
            ++schedule_failures_;
            return ScheduleStatus::DelayTooLarge;
        }

        if (delay_ticks == 0) {
            delay_ticks = 1;
        }

        frame.release_tick = current_tick_ + delay_ticks;
        Slot& slot = slots_[frame.release_tick & kSlotMask];

        if (slot.count >= MaxFramesPerSlot) {
            ++schedule_failures_;
            return ScheduleStatus::SlotFull;
        }

        slot.frames[slot.count] = frame;
        ++slot.count;
        ++pending_count_;
        return ScheduleStatus::Scheduled;
    }

    [[nodiscard]] std::span<const DelayedFrame> tick() noexcept {
        ++current_tick_;
        expired_count_ = 0;

        Slot& slot = slots_[current_tick_ & kSlotMask];
        std::size_t kept_count = 0;

        for (std::size_t index = 0; index < slot.count; ++index) {
            const DelayedFrame frame = slot.frames[index];

            if (frame.release_tick <= current_tick_) {
                expired_[expired_count_] = frame;
                ++expired_count_;
                --pending_count_;
                continue;
            }

            slot.frames[kept_count] = frame;
            ++kept_count;
        }

        slot.count = kept_count;
        return std::span<const DelayedFrame>{expired_.data(), expired_count_};
    }

    [[nodiscard]] std::uint64_t current_tick() const noexcept {
        return current_tick_;
    }

    [[nodiscard]] std::size_t pending_count() const noexcept {
        return pending_count_;
    }

    [[nodiscard]] std::uint64_t schedule_failures() const noexcept {
        return schedule_failures_;
    }

    [[nodiscard]] std::size_t slot_pending_count(const std::size_t slot_index) const noexcept {
        return slot_index < SlotCount ? slots_[slot_index].count : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return pending_count_ == 0;
    }

    [[nodiscard]] static consteval std::size_t slot_count() noexcept {
        return SlotCount;
    }

    [[nodiscard]] static consteval std::size_t max_frames_per_slot() noexcept {
        return MaxFramesPerSlot;
    }

    [[nodiscard]] static consteval std::uint64_t max_delay_ticks() noexcept {
        return SlotCount;
    }

private:
    static constexpr std::size_t kSlotMask = SlotCount - 1;

    struct Slot {
        std::array<DelayedFrame, MaxFramesPerSlot> frames{};
        std::size_t count{};
    };

    std::array<Slot, SlotCount> slots_{};
    std::array<DelayedFrame, MaxFramesPerSlot> expired_{};
    std::uint64_t current_tick_{};
    std::size_t expired_count_{};
    std::size_t pending_count_{};
    std::uint64_t schedule_failures_{};
};

using DefaultTimingWheel = TimingWheel<1024, 64>;

}  // namespace chronosx
