#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

#include "chronosx/types.hpp"

namespace chronosx {

template <std::size_t Capacity>
concept PowerOfTwoCapacity = is_power_of_two(Capacity);

template <typename T>
concept SPSCElement = std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>;

template <SPSCElement T, std::size_t Capacity>
    requires PowerOfTwoCapacity<Capacity>
class SPSCQueue {
public:
    SPSCQueue() = default;

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head >= Capacity) {
            return false;
        }

        buffer_[tail & kIndexMask] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return std::nullopt;
        }

        T item = buffer_[head & kIndexMask];
        head_.store(head + 1, std::memory_order_release);
        return item;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

    [[nodiscard]] bool full_approx() const noexcept {
        return size_approx() >= Capacity;
    }

    [[nodiscard]] static consteval std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kIndexMask = Capacity - 1;

    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

}  // namespace chronosx
