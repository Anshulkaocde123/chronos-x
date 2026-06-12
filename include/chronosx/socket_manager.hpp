// =============================================================================
// socket_manager.hpp — AF_XDP Socket Abstraction Layer
//
// What it does:
//   Wraps the four AF_XDP rings (Fill, Completion, RX, TX) and UMEM
//   configuration into a clean C++ API. Provides a SimulatedXskSocket
//   that mimics real AF_XDP ring arithmetic for userspace-only testing.
//
// Why it exists:
//   AF_XDP requires root privileges and a real NIC to test. By simulating
//   the ring Producer/Consumer pattern (same index masking, same flow),
//   we can run the ENTIRE data-plane pipeline in a unit test. The real
//   AF_XDP implementation would swap SimulatedRing for libxdp's ring API
//   with zero changes to the data-plane logic.
//
// Key design decision:
//   All ring sizes MUST be power-of-two. This allows index masking
//   (idx & (size-1)) instead of modulo, which is a single AND instruction
//   vs. an expensive DIV. At 10 Mpps, this saves ~30 million DIVs/sec.
//
// Interview question (D.E. Shaw):
//   "Why four separate rings instead of one shared ring?"
//   Answer: "Each ring has a different producer/consumer pair:
//   Fill (user→kernel), RX (kernel→user), TX (user→kernel),
//   Completion (kernel→user). Separating them eliminates false sharing
//   and allows lockless progress — the kernel and userspace never
//   contend on the same cache line."
// =============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "chronosx/frame_allocator.hpp"
#include "chronosx/types.hpp"

namespace chronosx {

inline constexpr std::size_t kDefaultUmemFrameSize = 4096;
inline constexpr std::size_t kDefaultUmemFrameCount = 4096;
inline constexpr std::size_t kDefaultFillRingSize = 2048;
inline constexpr std::size_t kDefaultCompletionRingSize = 2048;
inline constexpr std::size_t kDefaultRxRingSize = 2048;
inline constexpr std::size_t kDefaultTxRingSize = 2048;
inline constexpr std::uint32_t kDefaultQueueId = 0;
inline constexpr std::uint32_t kDefaultXdpFlags = 0;

static_assert(is_power_of_two(kDefaultFillRingSize));
static_assert(is_power_of_two(kDefaultCompletionRingSize));
static_assert(is_power_of_two(kDefaultRxRingSize));
static_assert(is_power_of_two(kDefaultTxRingSize));

struct UmemConfig {
    std::size_t frame_size{kDefaultUmemFrameSize};
    std::size_t frame_count{kDefaultUmemFrameCount};
    std::size_t fill_ring_size{kDefaultFillRingSize};
    std::size_t completion_ring_size{kDefaultCompletionRingSize};

    [[nodiscard]] constexpr std::uint64_t total_bytes() const noexcept {
        return static_cast<std::uint64_t>(frame_size) * frame_count;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return is_power_of_two(frame_size) && frame_size >= kMinXdpFrameSizeBytes &&
               frame_count > 0 && is_power_of_two(fill_ring_size) &&
               is_power_of_two(completion_ring_size);
    }
};

static_assert(std::is_trivially_copyable_v<UmemConfig>);

struct SocketConfig {
    std::uint32_t queue_id{kDefaultQueueId};
    std::uint32_t xdp_flags{kDefaultXdpFlags};
    std::size_t rx_ring_size{kDefaultRxRingSize};
    std::size_t tx_ring_size{kDefaultTxRingSize};
    std::uint32_t interface_index{};
    std::uint32_t bind_flags{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return is_power_of_two(rx_ring_size) && is_power_of_two(tx_ring_size);
    }
};

static_assert(std::is_trivially_copyable_v<SocketConfig>);

enum class SocketStatus : std::uint8_t {
    Ok = 0,
    InvalidConfig = 1,
    UmemAllocationFailed = 2,
    UmemCreateFailed = 3,
    SocketCreateFailed = 4,
    BindFailed = 5,
    NotInitialized = 6,
    AlreadyInitialized = 7,
    KickFailed = 8,
    RingFull = 9,
    RingEmpty = 10,
};

[[nodiscard]] constexpr const char* socket_status_name(const SocketStatus status) noexcept {
    switch (status) {
        case SocketStatus::Ok:
            return "Ok";
        case SocketStatus::InvalidConfig:
            return "InvalidConfig";
        case SocketStatus::UmemAllocationFailed:
            return "UmemAllocationFailed";
        case SocketStatus::UmemCreateFailed:
            return "UmemCreateFailed";
        case SocketStatus::SocketCreateFailed:
            return "SocketCreateFailed";
        case SocketStatus::BindFailed:
            return "BindFailed";
        case SocketStatus::NotInitialized:
            return "NotInitialized";
        case SocketStatus::AlreadyInitialized:
            return "AlreadyInitialized";
        case SocketStatus::KickFailed:
            return "KickFailed";
        case SocketStatus::RingFull:
            return "RingFull";
        case SocketStatus::RingEmpty:
            return "RingEmpty";
    }

    return "Unknown";
}

struct RxDescriptor {
    std::uint64_t address{};
    std::uint32_t length{};
    std::uint32_t options{};
};

static_assert(sizeof(RxDescriptor) == 16);
static_assert(alignof(RxDescriptor) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<RxDescriptor>);

struct TxDescriptor {
    std::uint64_t address{};
    std::uint32_t length{};
    std::uint32_t options{};
};

static_assert(sizeof(TxDescriptor) == 16);
static_assert(alignof(TxDescriptor) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<TxDescriptor>);

struct RxBatchResult {
    std::size_t count{};
    SocketStatus status{SocketStatus::Ok};
};

struct TxBatchResult {
    std::size_t count{};
    SocketStatus status{SocketStatus::Ok};
};

struct FillResult {
    std::size_t count{};
    SocketStatus status{SocketStatus::Ok};
};

struct CompletionResult {
    std::size_t count{};
    SocketStatus status{SocketStatus::Ok};
};

// ---------------------------------------------------------------------------
// SimulatedRing — userspace-only ring buffer mimicking AF_XDP ring mechanics.
//
// In production, the kernel owns one end of each ring. Here we simulate
// both sides so tests can inject RX packets and consume TX output.
//
// The ring uses monotonically increasing indices with bitmask wraparound.
// This is the same technique used by the Linux kernel's xsk_ring_cons/prod.
// The key insight: producer_ and consumer_ NEVER wrap — they grow forever.
// Only the array access uses masking: buffer_[idx & (Capacity - 1)].
// This avoids the branch that a "wrap-to-zero" strategy needs.
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
    requires(is_power_of_two(Capacity))
class SimulatedRing {
public:
    SimulatedRing() = default;

    SimulatedRing(const SimulatedRing&) = delete;
    SimulatedRing& operator=(const SimulatedRing&) = delete;
    SimulatedRing(SimulatedRing&&) = delete;
    SimulatedRing& operator=(SimulatedRing&&) = delete;

    [[nodiscard]] std::size_t produce(std::span<const T> items) noexcept {
        std::size_t produced = 0;

        while (produced < items.size() && available_produce() > 0) {
            buffer_[producer_ & kMask] = items[produced];
            ++producer_;
            ++produced;
        }

        return produced;
    }

    [[nodiscard]] std::size_t consume(std::span<T> output) noexcept {
        std::size_t consumed = 0;

        while (consumed < output.size() && available_consume() > 0) {
            output[consumed] = buffer_[consumer_ & kMask];
            ++consumer_;
            ++consumed;
        }

        return consumed;
    }

    [[nodiscard]] std::size_t available_consume() const noexcept {
        return producer_ - consumer_;
    }

    [[nodiscard]] std::size_t available_produce() const noexcept {
        return Capacity - (producer_ - consumer_);
    }

    [[nodiscard]] bool empty() const noexcept {
        return producer_ == consumer_;
    }

    [[nodiscard]] bool full() const noexcept {
        return available_produce() == 0;
    }

    [[nodiscard]] static consteval std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};
    std::size_t producer_{};
    std::size_t consumer_{};
};

// ---------------------------------------------------------------------------
// SimulatedXskSocket — full AF_XDP socket simulation for testing.
//
// In production, this class would use libxdp's xsk_socket__create(),
// xsk_ring_cons__peek(), xsk_ring_prod__submit(), and sendto() to kick TX.
//
// The simulated version provides the SAME API surface and ring arithmetic
// so that all data-plane logic can be tested without root privileges.
// ---------------------------------------------------------------------------

template <std::size_t FillSize = kDefaultFillRingSize,
          std::size_t CompSize = kDefaultCompletionRingSize,
          std::size_t RxSize = kDefaultRxRingSize,
          std::size_t TxSize = kDefaultTxRingSize>
    requires(is_power_of_two(FillSize) && is_power_of_two(CompSize) &&
             is_power_of_two(RxSize) && is_power_of_two(TxSize))
class SimulatedXskSocket {
public:
    SimulatedXskSocket() = default;

    SimulatedXskSocket(const SimulatedXskSocket&) = delete;
    SimulatedXskSocket& operator=(const SimulatedXskSocket&) = delete;
    SimulatedXskSocket(SimulatedXskSocket&&) = delete;
    SimulatedXskSocket& operator=(SimulatedXskSocket&&) = delete;

    // --- Fill Ring: userspace → kernel (frame addresses for RX) ---

    [[nodiscard]] FillResult refill(std::span<const std::uint64_t> addresses) noexcept {
        const std::size_t count = fill_ring_.produce(
            std::span<const std::uint64_t>{addresses.data(), addresses.size()});

        const SocketStatus status = count == addresses.size() ? SocketStatus::Ok : SocketStatus::RingFull;
        return FillResult{.count = count, .status = status};
    }

    // --- Completion Ring: kernel → userspace (TX-completed frame addresses) ---

    [[nodiscard]] CompletionResult drain_completions(std::span<std::uint64_t> output) noexcept {
        const std::size_t count = completion_ring_.consume(output);
        const SocketStatus status = count == output.size() ? SocketStatus::Ok : SocketStatus::RingEmpty;
        return CompletionResult{.count = count, .status = status};
    }

    // --- RX Ring: kernel → userspace (received packet descriptors) ---

    [[nodiscard]] RxBatchResult consume_rx(std::span<RxDescriptor> output) noexcept {
        const std::size_t count = rx_ring_.consume(output);
        const SocketStatus status = count == output.size() ? SocketStatus::Ok : SocketStatus::RingEmpty;
        return RxBatchResult{.count = count, .status = status};
    }

    // --- TX Ring: userspace → kernel (packets to transmit) ---

    [[nodiscard]] TxBatchResult submit_tx(std::span<const TxDescriptor> descriptors) noexcept {
        const std::size_t count = tx_ring_.produce(descriptors);
        const SocketStatus status = count == descriptors.size() ? SocketStatus::Ok : SocketStatus::RingFull;
        return TxBatchResult{.count = count, .status = status};
    }

    // --- Kick TX: signal kernel to consume TX ring ---

    [[nodiscard]] SocketStatus kick_tx() noexcept {
        tx_kick_count_++;
        return SocketStatus::Ok;
    }

    // --- Simulation helpers (inject packets / complete TX for testing) ---

    [[nodiscard]] std::size_t inject_rx(std::span<const RxDescriptor> descriptors) noexcept {
        return rx_ring_.produce(descriptors);
    }

    [[nodiscard]] std::size_t complete_tx(std::span<const std::uint64_t> addresses) noexcept {
        return completion_ring_.produce(addresses);
    }

    [[nodiscard]] std::size_t consume_fill(std::span<std::uint64_t> output) noexcept {
        return fill_ring_.consume(output);
    }

    // --- Stats ---

    [[nodiscard]] std::uint64_t tx_kick_count() const noexcept {
        return tx_kick_count_;
    }

    [[nodiscard]] std::size_t fill_ring_pending() const noexcept {
        return fill_ring_.available_consume();
    }

    [[nodiscard]] std::size_t completion_ring_pending() const noexcept {
        return completion_ring_.available_consume();
    }

    [[nodiscard]] std::size_t rx_ring_pending() const noexcept {
        return rx_ring_.available_consume();
    }

    [[nodiscard]] std::size_t tx_ring_pending() const noexcept {
        return tx_ring_.available_consume();
    }

private:
    SimulatedRing<std::uint64_t, FillSize> fill_ring_{};
    SimulatedRing<std::uint64_t, CompSize> completion_ring_{};
    SimulatedRing<RxDescriptor, RxSize> rx_ring_{};
    SimulatedRing<TxDescriptor, TxSize> tx_ring_{};
    std::uint64_t tx_kick_count_{};
};

using DefaultSimulatedSocket = SimulatedXskSocket<>;

}  // namespace chronosx
