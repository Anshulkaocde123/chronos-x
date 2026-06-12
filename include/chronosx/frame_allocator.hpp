#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "chronosx/types.hpp"

namespace chronosx {

inline constexpr std::size_t kDefaultFrameSizeBytes = 4096;
inline constexpr std::size_t kDefaultFrameCount = 4096;
inline constexpr std::size_t kMinXdpFrameSizeBytes = 2048;

enum class FrameOwner : std::uint8_t {
    Free = 0,
    FillRing = 1,
    RxRing = 2,
    DataPlane = 3,
    TimingWheel = 4,
    TxRing = 5,
    CompletionRing = 6,
};

[[nodiscard]] constexpr const char* frame_owner_name(const FrameOwner owner) noexcept {
    switch (owner) {
        case FrameOwner::Free:
            return "Free";
        case FrameOwner::FillRing:
            return "FillRing";
        case FrameOwner::RxRing:
            return "RxRing";
        case FrameOwner::DataPlane:
            return "DataPlane";
        case FrameOwner::TimingWheel:
            return "TimingWheel";
        case FrameOwner::TxRing:
            return "TxRing";
        case FrameOwner::CompletionRing:
            return "CompletionRing";
    }

    return "Unknown";
}

enum class FrameStatus : std::uint8_t {
    Ok = 0,
    PoolEmpty = 1,
    PoolFull = 2,
    InvalidFrameIndex = 3,
    InvalidFrameAddress = 4,
    InvalidOwner = 5,
};

[[nodiscard]] constexpr const char* frame_status_name(const FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::Ok:
            return "Ok";
        case FrameStatus::PoolEmpty:
            return "PoolEmpty";
        case FrameStatus::PoolFull:
            return "PoolFull";
        case FrameStatus::InvalidFrameIndex:
            return "InvalidFrameIndex";
        case FrameStatus::InvalidFrameAddress:
            return "InvalidFrameAddress";
        case FrameStatus::InvalidOwner:
            return "InvalidOwner";
    }

    return "Unknown";
}

struct FrameHandle {
    std::uint64_t address{};
    std::uint32_t index{};
    std::uint32_t reserved{};
};

static_assert(sizeof(FrameHandle) == 16);
static_assert(alignof(FrameHandle) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<FrameHandle>);
static_assert(std::is_standard_layout_v<FrameHandle>);

struct FrameAllocation {
    FrameHandle frame{};
    FrameStatus status{FrameStatus::PoolEmpty};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == FrameStatus::Ok;
    }
};

static_assert(sizeof(FrameAllocation) == 24);
static_assert(alignof(FrameAllocation) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<FrameAllocation>);
static_assert(std::is_standard_layout_v<FrameAllocation>);

template <std::size_t FrameCount = kDefaultFrameCount, std::size_t FrameSize = kDefaultFrameSizeBytes>
    requires(FrameCount > 0 && is_power_of_two(FrameSize) && FrameSize >= kMinXdpFrameSizeBytes)
class FrameAllocator {
public:
    FrameAllocator() noexcept {
        for (std::size_t index = 0; index < FrameCount; ++index) {
            free_stack_[index] = static_cast<std::uint32_t>(FrameCount - index - 1);
            owners_[index] = FrameOwner::Free;
        }

        free_count_ = FrameCount;
    }

    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;
    FrameAllocator(FrameAllocator&&) = delete;
    FrameAllocator& operator=(FrameAllocator&&) = delete;

    [[nodiscard]] FrameAllocation allocate(const FrameOwner owner = FrameOwner::DataPlane) noexcept {
        if (free_count_ == 0) {
            ++allocation_failures_;
            return FrameAllocation{.status = FrameStatus::PoolEmpty};
        }

        const std::uint32_t index = free_stack_[free_count_ - 1];
        --free_count_;
        owners_[index] = owner;

        return FrameAllocation{
            .frame = make_handle(index),
            .status = FrameStatus::Ok,
        };
    }

    [[nodiscard]] std::size_t allocate_batch(std::span<FrameHandle> output,
                                             const FrameOwner owner = FrameOwner::DataPlane) noexcept {
        std::size_t count = 0;

        while (count < output.size() && free_count_ > 0) {
            const FrameAllocation allocation = allocate(owner);
            output[count] = allocation.frame;
            ++count;
        }

        if (count < output.size()) {
            ++allocation_failures_;
        }

        return count;
    }

    [[nodiscard]] FrameStatus transition(const FrameHandle frame,
                                         const FrameOwner expected_owner,
                                         const FrameOwner next_owner) noexcept {
        const FrameStatus status = validate_frame(frame);
        if (status != FrameStatus::Ok) {
            ++transition_failures_;
            return status;
        }

        if (owners_[frame.index] != expected_owner) {
            ++transition_failures_;
            return FrameStatus::InvalidOwner;
        }

        if (next_owner == FrameOwner::Free) {
            return release(frame, expected_owner);
        }

        owners_[frame.index] = next_owner;
        return FrameStatus::Ok;
    }

    [[nodiscard]] FrameStatus release(const FrameHandle frame,
                                      const FrameOwner expected_owner = FrameOwner::DataPlane) noexcept {
        const FrameStatus status = validate_frame(frame);
        if (status != FrameStatus::Ok) {
            ++release_failures_;
            return status;
        }

        if (owners_[frame.index] != expected_owner) {
            ++release_failures_;
            return FrameStatus::InvalidOwner;
        }

        if (free_count_ >= FrameCount) {
            ++release_failures_;
            return FrameStatus::PoolFull;
        }

        owners_[frame.index] = FrameOwner::Free;
        free_stack_[free_count_] = frame.index;
        ++free_count_;
        return FrameStatus::Ok;
    }

    [[nodiscard]] FrameStatus release_batch(std::span<const FrameHandle> frames,
                                            const FrameOwner expected_owner = FrameOwner::DataPlane) noexcept {
        for (const FrameHandle frame : frames) {
            const FrameStatus status = validate_frame(frame);
            if (status != FrameStatus::Ok || owners_[frame.index] != expected_owner) {
                ++release_failures_;
                return status == FrameStatus::Ok ? FrameStatus::InvalidOwner : status;
            }
        }

        if (free_count_ + frames.size() > FrameCount) {
            ++release_failures_;
            return FrameStatus::PoolFull;
        }

        for (const FrameHandle frame : frames) {
            owners_[frame.index] = FrameOwner::Free;
            free_stack_[free_count_] = frame.index;
            ++free_count_;
        }

        return FrameStatus::Ok;
    }

    [[nodiscard]] FrameAllocation handle_from_address(const std::uint64_t address) const noexcept {
        if (address % FrameSize != 0 || address >= umem_size_bytes()) {
            return FrameAllocation{
                .frame = FrameHandle{.address = address},
                .status = FrameStatus::InvalidFrameAddress,
            };
        }

        const auto index = static_cast<std::uint32_t>(address / FrameSize);
        return FrameAllocation{
            .frame = make_handle(index),
            .status = FrameStatus::Ok,
        };
    }

    [[nodiscard]] FrameOwner owner_of(const std::uint32_t index) const noexcept {
        return index < FrameCount ? owners_[index] : FrameOwner::Free;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return free_count_;
    }

    [[nodiscard]] std::size_t in_use() const noexcept {
        return FrameCount - free_count_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return free_count_ == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return free_count_ == FrameCount;
    }

    [[nodiscard]] std::uint64_t allocation_failures() const noexcept {
        return allocation_failures_;
    }

    [[nodiscard]] std::uint64_t release_failures() const noexcept {
        return release_failures_;
    }

    [[nodiscard]] std::uint64_t transition_failures() const noexcept {
        return transition_failures_;
    }

    [[nodiscard]] static consteval std::size_t capacity() noexcept {
        return FrameCount;
    }

    [[nodiscard]] static consteval std::size_t frame_size_bytes() noexcept {
        return FrameSize;
    }

    [[nodiscard]] static consteval std::uint64_t umem_size_bytes() noexcept {
        return static_cast<std::uint64_t>(FrameCount) * FrameSize;
    }

    [[nodiscard]] static constexpr FrameHandle make_handle(const std::uint32_t index) noexcept {
        return FrameHandle{
            .address = static_cast<std::uint64_t>(index) * FrameSize,
            .index = index,
            .reserved = 0,
        };
    }

private:
    [[nodiscard]] static constexpr bool valid_index(const std::uint32_t index) noexcept {
        return index < FrameCount;
    }

    [[nodiscard]] static constexpr bool valid_address_for_index(const FrameHandle frame) noexcept {
        return frame.address == static_cast<std::uint64_t>(frame.index) * FrameSize;
    }

    [[nodiscard]] static constexpr FrameStatus validate_frame(const FrameHandle frame) noexcept {
        if (!valid_index(frame.index)) {
            return FrameStatus::InvalidFrameIndex;
        }

        if (!valid_address_for_index(frame)) {
            return FrameStatus::InvalidFrameAddress;
        }

        return FrameStatus::Ok;
    }

    std::array<std::uint32_t, FrameCount> free_stack_{};
    std::array<FrameOwner, FrameCount> owners_{};
    std::size_t free_count_{};
    std::uint64_t allocation_failures_{};
    std::uint64_t release_failures_{};
    std::uint64_t transition_failures_{};
};

using DefaultFrameAllocator = FrameAllocator<>;

}  // namespace chronosx
