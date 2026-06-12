#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>

#include "chronosx/spsc_queue.hpp"
#include "chronosx/types.hpp"

namespace {

void test_basic_push_pop() {
    chronosx::SPSCQueue<std::uint64_t, 8> queue;

    assert(queue.empty_approx());
    assert(queue.try_push(42));
    assert(!queue.empty_approx());

    const auto value = queue.try_pop();
    assert(value.has_value());
    assert(*value == 42);
    assert(!queue.try_pop().has_value());
}

void test_full_queue() {
    chronosx::SPSCQueue<std::uint64_t, 4> queue;

    assert(queue.try_push(1));
    assert(queue.try_push(2));
    assert(queue.try_push(3));
    assert(queue.try_push(4));
    assert(!queue.try_push(5));

    for (std::uint64_t expected = 1; expected <= 4; ++expected) {
        const auto value = queue.try_pop();
        assert(value.has_value());
        assert(*value == expected);
    }

    assert(!queue.try_pop().has_value());
}

void test_wraparound_ordering() {
    chronosx::SPSCQueue<std::uint64_t, 8> queue;

    for (std::uint64_t value = 0; value < 10'000; ++value) {
        assert(queue.try_push(value));
        const auto popped = queue.try_pop();
        assert(popped.has_value());
        assert(*popped == value);
    }
}

void test_stat_update_payload() {
    chronosx::SPSCQueue<chronosx::StatUpdate, 16> queue;

    const chronosx::StatUpdate update{
        .timestamp_cycles = 123,
        .packets_seen = 456,
        .packets_dropped = 7,
        .bytes_seen = 456 * 64,
        .average_latency_ns = 89,
        .last_action = chronosx::PacketAction::Corrupt,
        .reserved0 = 0,
        .reserved1 = 0,
    };

    assert(queue.try_push(update));
    const auto popped = queue.try_pop();
    assert(popped.has_value());
    assert(popped->timestamp_cycles == update.timestamp_cycles);
    assert(popped->packets_seen == update.packets_seen);
    assert(popped->packets_dropped == update.packets_dropped);
    assert(popped->bytes_seen == update.bytes_seen);
    assert(popped->average_latency_ns == update.average_latency_ns);
    assert(popped->last_action == update.last_action);
}

void test_threaded_spsc_stress() {
    chronosx::SPSCQueue<std::uint64_t, 1024> queue;
    constexpr std::uint64_t kItemCount = 1'000'000;

    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> consumed_count{0};
    std::atomic<std::uint64_t> consumed_sum{0};

    std::thread producer([&queue, &producer_done] {
        for (std::uint64_t value = 1; value <= kItemCount; ++value) {
            while (!queue.try_push(value)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&queue, &producer_done, &consumed_count, &consumed_sum] {
        std::uint64_t expected = 1;

        while (!producer_done.load(std::memory_order_acquire) || !queue.empty_approx()) {
            const auto value = queue.try_pop();
            if (!value.has_value()) {
                std::this_thread::yield();
                continue;
            }

            assert(*value == expected);
            consumed_sum.fetch_add(*value, std::memory_order_relaxed);
            consumed_count.fetch_add(1, std::memory_order_relaxed);
            ++expected;
        }
    });

    producer.join();
    consumer.join();

    constexpr std::uint64_t expected_sum = kItemCount * (kItemCount + 1) / 2;
    assert(consumed_count.load(std::memory_order_relaxed) == kItemCount);
    assert(consumed_sum.load(std::memory_order_relaxed) == expected_sum);
}

}  // namespace

int main() {
    test_basic_push_pop();
    test_full_queue();
    test_wraparound_ordering();
    test_stat_update_payload();
    test_threaded_spsc_stress();

    std::cout << "all SPSC queue tests passed\n";
    return 0;
}
