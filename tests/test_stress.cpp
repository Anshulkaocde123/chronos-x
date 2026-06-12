// =============================================================================
// test_stress.cpp — HFT-level stress tests for Chronos-X
//
// Validates:
//   1. SPSC queue under sustained pressure (10M items, 1 producer × 1 consumer)
//   2. SPSC backpressure (full queue, verify no silent drops)
//   3. Concurrent rule swap (writer thread + reader thread, no crash/torn read)
//   4. Batch storm (10K batches of 64 packets, verify stats integrity)
//   5. TimingWheel flood (fill to capacity, tick, verify all delivered)
//   6. Edge cases (empty batch, zero-length packets, all-drop rules)
//   7. Determinism (same seed + same packets = same outcomes, 100 trials)
//
// These tests are designed to survive conditions found in HFT systems:
// tight loops, cache-line contention, memory ordering stress, and
// statistical verification of correctness under high throughput.
// =============================================================================

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

#include "chronosx/chaos_engine.hpp"
#include "chronosx/control_plane.hpp"
#include "chronosx/data_plane.hpp"
#include "chronosx/frame_allocator.hpp"
#include "chronosx/packet_parser.hpp"
#include "chronosx/protocol.hpp"
#include "chronosx/spsc_queue.hpp"
#include "chronosx/timing_wheel.hpp"
#include "chronosx/tui.hpp"
#include "chronosx/types.hpp"

namespace {

using namespace chronosx;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void build_tcp_packet(Byte* frame, const std::uint16_t dst_port,
                      const std::size_t payload_size) noexcept {
    const std::size_t total = kEthernetHeaderBytes + kIPv4BaseHeaderBytes +
                               kTCPBaseHeaderBytes + payload_size;
    std::memset(frame, 0, total);

    frame[12] = 0x08;
    frame[13] = 0x00;

    Byte* ip = frame + kEthernetHeaderBytes;
    ip[0] = 0x45;
    const auto len = static_cast<std::uint16_t>(
        kIPv4BaseHeaderBytes + kTCPBaseHeaderBytes + payload_size);
    ip[2] = static_cast<Byte>(len >> 8);
    ip[3] = static_cast<Byte>(len & 0xFF);
    ip[8] = 64;
    ip[9] = kIPv4ProtocolTCP;
    // src_ip = 10.0.0.1
    ip[12] = 10; ip[13] = 0; ip[14] = 0; ip[15] = 1;
    // dst_ip = 10.0.0.2
    ip[16] = 10; ip[17] = 0; ip[18] = 0; ip[19] = 2;

    Byte* tcp = ip + kIPv4BaseHeaderBytes;
    tcp[0] = static_cast<Byte>(12345 >> 8);
    tcp[1] = static_cast<Byte>(12345 & 0xFF);
    tcp[2] = static_cast<Byte>(dst_port >> 8);
    tcp[3] = static_cast<Byte>(dst_port & 0xFF);
    tcp[12] = 0x50;

    Byte* payload = tcp + kTCPBaseHeaderBytes;
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<Byte>(i & 0xFF);
    }
}

// ===== TEST 1: SPSC Queue — 10M Items Under Pressure ========================
//
// Why this matters: In HFT, the SPSC queue carries stat updates from the
// data plane to the UI at line rate. If memory ordering is wrong, the
// consumer sees stale data (torn reads). This test has one producer pushing
// 10M sequential integers and one consumer verifying every item arrives
// in order with no gaps.
// =============================================================================

void test_spsc_high_throughput() {
    std::cout << "  [1/7] SPSC high-throughput...";

    constexpr std::size_t kItemCount = 10'000'000;
    constexpr std::size_t kQueueSize = 4096;

    SPSCQueue<std::uint64_t, kQueueSize> queue;
    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> items_consumed{0};

    // Producer: push 0..N-1. Spin on failure (queue full = backpressure).
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItemCount; ++i) {
            while (!queue.try_push(i)) {
                // Busy-spin — this is what the data plane does.
                // In HFT, a yield here would add microseconds of jitter.
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer: pop and verify monotonic sequence. Any gap = torn read.
    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < kItemCount) {
            const auto item = queue.try_pop();
            if (!item.has_value()) {
                // Check if producer is done and queue is empty
                if (producer_done.load(std::memory_order_acquire) &&
                    queue.empty_approx()) {
                    break;
                }
                continue;
            }
            assert(item.value() == expected &&
                   "SPSC: item received out of order — possible torn read!");
            ++expected;
        }
        items_consumed.store(expected, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    assert(items_consumed.load() == kItemCount &&
           "SPSC: not all items consumed — possible lost items!");
    std::cout << " OK (10M items, zero drops)\n";
}

// ===== TEST 2: SPSC Backpressure =============================================
//
// Verify that try_push correctly returns false when the queue is full,
// and no items are silently lost or corrupted.
// =============================================================================

void test_spsc_backpressure() {
    std::cout << "  [2/7] SPSC backpressure...";

    constexpr std::size_t kQueueSize = 64;
    SPSCQueue<std::uint32_t, kQueueSize> queue;

    // Fill queue to capacity
    for (std::uint32_t i = 0; i < kQueueSize; ++i) {
        assert(queue.try_push(i) && "push should succeed when not full");
    }

    // Next push must fail
    assert(!queue.try_push(999) && "push must fail when queue is full");
    assert(queue.full_approx());

    // Drain and verify all items intact
    for (std::uint32_t i = 0; i < kQueueSize; ++i) {
        const auto item = queue.try_pop();
        assert(item.has_value() && "pop should succeed when not empty");
        assert(item.value() == i && "item corrupted after backpressure");
    }

    assert(queue.empty_approx());
    assert(!queue.try_pop().has_value() && "pop must fail when empty");

    std::cout << " OK (capacity=" << kQueueSize << ", bounded correctly)\n";
}

// ===== TEST 3: Concurrent Rule Swap (RCU Correctness) ========================
//
// Why this matters: The data plane reads rules via atomic<shared_ptr>.
// The control plane swaps rules at any time. If the atomic swap is
// not correct, the reader could see a dangling pointer or torn data.
//
// This test has:
//   - Writer thread: rapidly cycling through 1000 rule sets
//   - Reader thread: reading snapshots and verifying internal consistency
// =============================================================================

void test_concurrent_rule_swap() {
    std::cout << "  [3/7] Concurrent rule swap (RCU)...";

    ControlPlaneCore core;
    constexpr int kSwapRounds = 10'000;
    std::atomic<bool> writer_done{false};
    std::atomic<int> snapshots_read{0};

    // Writer: add a rule, then clear, repeat — this exercises the full
    // atomic publish path.
    std::thread writer([&] {
        for (int round = 0; round < kSwapRounds; ++round) {
            ChaosRule rule{
                .seed = static_cast<std::uint64_t>(round),
                .delay_ns = 0,
                .id = static_cast<std::uint32_t>(round % 100),
                .src_ip = 0,
                .dst_ip = 0,
                .src_port = 0,
                .dst_port = static_cast<std::uint16_t>(8080 + (round % 10)),
                .probability = kProbabilityScale,
                .protocol = kIPv4ProtocolTCP,
                .action = PacketAction::Drop,
                .enabled = 1,
            };

            // Build AddRule request
            std::array<Byte, 512> req{};
            std::array<Byte, 512> resp{};
            const MessageHeader hdr{
                .opcode = Opcode::AddRule,
                .sequence = static_cast<std::uint32_t>(round),
            };
            const auto enc = encode_message(
                hdr,
                std::span<const Byte>{
                    reinterpret_cast<const Byte*>(&rule), sizeof(rule)},
                std::span<Byte>{req});
            (void)core.handle_request(
                std::span<const Byte>{req.data(), enc.bytes_written},
                std::span<Byte>{resp});

            // Every 10th round, clear all rules to test the swap path
            if (round % 10 == 9) {
                std::array<Byte, 512> clear_req{};
                std::array<Byte, 512> clear_resp{};
                const MessageHeader clear_hdr{
                    .opcode = Opcode::ClearRules,
                    .sequence = static_cast<std::uint32_t>(round),
                };
                const auto clear_enc = encode_message(
                    clear_hdr, std::span<const Byte>{},
                    std::span<Byte>{clear_req});
                (void)core.handle_request(
                    std::span<const Byte>{clear_req.data(), clear_enc.bytes_written},
                    std::span<Byte>{clear_resp});
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    // Reader: continuously load the current rule snapshot and verify
    // internal consistency (no null ptr, no garbage, all rules have valid IDs).
    std::thread reader([&] {
        while (!writer_done.load(std::memory_order_acquire)) {
            auto snapshot = core.current_rules();
            assert(snapshot != nullptr && "RCU: null snapshot — atomic swap broken!");

            // Verify every rule in the snapshot has valid fields
            for (const auto& rule : snapshot->rules()) {
                assert(rule.probability <= kProbabilityScale &&
                       "RCU: corrupted probability — torn read!");
                assert(rule.protocol == kIPv4ProtocolTCP &&
                       "RCU: corrupted protocol — torn read!");
                (void)rule.id;  // Just access it — will crash if ptr is dangling
            }

            snapshots_read.fetch_add(1, std::memory_order_relaxed);
        }
    });

    writer.join();
    reader.join();

    assert(snapshots_read.load() > 0 && "Reader thread didn't run!");
    std::cout << " OK (" << kSwapRounds << " swaps, "
              << snapshots_read.load() << " reads, no torn data)\n";
}

// ===== TEST 4: Batch Storm (Data Plane Throughput) ============================
//
// Process 10K batches of 64 packets each (640K total packets).
// Verify that all stats (packets_seen, bytes_seen, drops, tx) add up
// perfectly — any discrepancy = race condition or counter overflow.
// =============================================================================

void test_batch_storm() {
    std::cout << "  [4/7] Batch storm (640K packets)...";

    constexpr std::size_t kBatchCount = 10'000;
    constexpr std::size_t kBatchSize = 64;
    constexpr std::size_t kPayloadSize = 32;
    constexpr std::size_t kFrameSize = kEthernetHeaderBytes + kIPv4BaseHeaderBytes +
                                        kTCPBaseHeaderBytes + kPayloadSize;
    constexpr std::size_t kTotalPackets = kBatchCount * kBatchSize;

    // Drop rule for port 8080 (100% probability)
    const std::array<ChaosRule, 1> rules{{
        ChaosRule{
            .seed = 0xDEAD,
            .delay_ns = 0,
            .id = 1,
            .src_ip = 0,
            .dst_ip = 0,
            .src_port = 0,
            .dst_port = 8080,
            .probability = kProbabilityScale,
            .protocol = kIPv4ProtocolTCP,
            .action = PacketAction::Drop,
            .enabled = 1,
        },
    }};

    // UMEM buffer — enough for one batch at a time
    alignas(kCacheLineSize) std::array<Byte, kBatchSize * 4096> umem{};
    Byte* umem_base = umem.data();

    // Build one batch of descriptors (reused each iteration)
    std::array<PacketDescriptor, kBatchSize> descriptors{};
    for (std::size_t i = 0; i < kBatchSize; ++i) {
        const std::uint64_t addr = i * 4096;
        // Half to port 8080 (will be dropped), half to port 443 (will pass)
        const std::uint16_t port = (i % 2 == 0) ? 8080 : 443;
        build_tcp_packet(umem_base + addr, port, kPayloadSize);
        descriptors[i] = PacketDescriptor{
            .frame_address = addr,
            .frame_length = static_cast<PacketLength>(kFrameSize),
            .options = 0,
        };
    }

    TimingWheel<1024, 64> timing_wheel;
    std::array<ProcessedFrame, kBatchSize> tx_out{};
    std::array<ProcessedFrame, kBatchSize> recycle_out{};

    PacketCount total_seen = 0;
    PacketCount total_dropped = 0;
    PacketCount total_tx = 0;
    ByteCount total_bytes = 0;

    for (std::size_t batch = 0; batch < kBatchCount; ++batch) {
        DataPlaneBatchResult result = process_packet_batch(
            umem_base,
            std::span<const PacketDescriptor>{descriptors},
            std::span<const ChaosRule>{rules},
            timing_wheel,
            std::span<ProcessedFrame>{tx_out},
            std::span<ProcessedFrame>{recycle_out});

        total_seen += result.stats.packets_seen;
        total_dropped += result.stats.packets_dropped;
        total_tx += result.stats.tx_frames;
        total_bytes += result.stats.bytes_seen;
    }

    assert(total_seen == kTotalPackets && "Batch storm: packets_seen mismatch!");
    assert(total_dropped == kTotalPackets / 2 && "Batch storm: drops mismatch!");
    assert(total_tx == kTotalPackets / 2 && "Batch storm: tx mismatch!");
    assert(total_bytes == kTotalPackets * kFrameSize && "Batch storm: bytes mismatch!");

    std::cout << " OK (seen=" << total_seen
              << " dropped=" << total_dropped
              << " tx=" << total_tx << ")\n";
}

// ===== TEST 5: TimingWheel Flood =============================================
//
// Schedule frames until the wheel is at capacity, tick through all of them,
// and verify nothing is lost (total in = total out).
// =============================================================================

void test_timing_wheel_flood() {
    std::cout << "  [5/7] TimingWheel flood...";

    constexpr std::size_t kSlots = 256;
    constexpr std::size_t kFramesPerSlot = 32;
    TimingWheel<kSlots, kFramesPerSlot> wheel;

    std::size_t scheduled = 0;
    std::size_t slot_full_count = 0;

    // Schedule frames with varying delays
    for (std::size_t i = 0; i < kSlots * kFramesPerSlot; ++i) {
        DelayedFrame frame{
            .frame_address = i * 4096,
            .frame_length = 86,
            .rule_id = static_cast<std::uint32_t>(i % 100),
            .release_tick = 0,
        };

        const std::uint64_t delay = (i % kSlots) + 1;
        const ScheduleStatus status = wheel.schedule(frame, delay);
        if (status == ScheduleStatus::Scheduled) {
            ++scheduled;
        } else {
            ++slot_full_count;
        }
    }

    assert(scheduled > 0 && "TimingWheel: nothing was scheduled!");

    // Tick through all slots and count expired frames
    std::size_t expired_total = 0;
    for (std::size_t tick = 0; tick < kSlots + 2; ++tick) {
        const auto expired = wheel.tick();
        expired_total += expired.size();
    }

    assert(expired_total == scheduled &&
           "TimingWheel: scheduled vs expired mismatch — frames lost!");
    assert(wheel.pending_count() == 0 && "TimingWheel: pending != 0 after full drain!");

    std::cout << " OK (scheduled=" << scheduled
              << " expired=" << expired_total
              << " slot_full=" << slot_full_count << ")\n";
}

// ===== TEST 6: Edge Cases ====================================================
//
// Covers the boundary conditions that cause crashes in production:
//   - Empty batch (0 descriptors)
//   - Zero-length packets
//   - Null UMEM base
//   - All rules disabled
//   - Maximum probability (100%)
//   - Minimum probability (0%)
// =============================================================================

void test_edge_cases() {
    std::cout << "  [6/7] Edge cases...";

    TimingWheel<64, 16> wheel;
    std::array<ProcessedFrame, 4> tx_out{};
    std::array<ProcessedFrame, 4> recycle_out{};

    // Edge 1: Empty batch — must not crash, stats = 0
    {
        DataPlaneBatchResult result = process_packet_batch<TimingWheel<64, 16>>(
            nullptr,
            std::span<const PacketDescriptor>{},
            std::span<const ChaosRule>{},
            wheel,
            std::span<ProcessedFrame>{tx_out},
            std::span<ProcessedFrame>{recycle_out});
        assert(result.stats.packets_seen == 0 && "Empty batch: packets_seen != 0");
        assert(result.tx_count == 0 && "Empty batch: tx_count != 0");
    }

    // Edge 2: Null UMEM — packets should be counted but treated as malformed
    {
        std::array<PacketDescriptor, 2> descs{{
            {.frame_address = 0, .frame_length = 86, .options = 0},
            {.frame_address = 4096, .frame_length = 86, .options = 0},
        }};

        DataPlaneBatchResult result = process_packet_batch<TimingWheel<64, 16>>(
            nullptr,
            std::span<const PacketDescriptor>{descs},
            std::span<const ChaosRule>{},
            wheel,
            std::span<ProcessedFrame>{tx_out},
            std::span<ProcessedFrame>{recycle_out});
        assert(result.stats.packets_seen == 2 && "Null UMEM: packets not counted");
    }

    // Edge 3: All rules disabled — all packets pass
    {
        alignas(kCacheLineSize) std::array<Byte, 4096> umem{};
        build_tcp_packet(umem.data(), 8080, 16);

        const std::array<ChaosRule, 1> disabled_rules{{
            ChaosRule{
                .seed = 1, .delay_ns = 0, .id = 1,
                .src_ip = 0, .dst_ip = 0, .src_port = 0, .dst_port = 8080,
                .probability = kProbabilityScale,
                .protocol = kIPv4ProtocolTCP,
                .action = PacketAction::Drop,
                .enabled = 0,  // DISABLED
            },
        }};

        std::array<PacketDescriptor, 1> descs{{
            {.frame_address = 0,
             .frame_length = static_cast<PacketLength>(
                 kEthernetHeaderBytes + kIPv4BaseHeaderBytes + kTCPBaseHeaderBytes + 16),
             .options = 0},
        }};

        DataPlaneBatchResult result = process_packet_batch(
            umem.data(),
            std::span<const PacketDescriptor>{descs},
            std::span<const ChaosRule>{disabled_rules},
            wheel,
            std::span<ProcessedFrame>{tx_out},
            std::span<ProcessedFrame>{recycle_out});
        assert(result.stats.packets_dropped == 0 &&
               "Disabled rule should not drop packets!");
        assert(result.stats.tx_frames == 1 &&
               "Disabled rule: packet should pass to TX!");
    }

    // Edge 4: 0% probability — all packets pass
    {
        alignas(kCacheLineSize) std::array<Byte, 4096> umem{};
        build_tcp_packet(umem.data(), 8080, 16);

        const std::array<ChaosRule, 1> zero_prob{{
            ChaosRule{
                .seed = 1, .delay_ns = 0, .id = 1,
                .src_ip = 0, .dst_ip = 0, .src_port = 0, .dst_port = 8080,
                .probability = 0,
                .protocol = kIPv4ProtocolTCP,
                .action = PacketAction::Drop,
                .enabled = 1,
            },
        }};

        std::array<PacketDescriptor, 1> descs{{
            {.frame_address = 0,
             .frame_length = static_cast<PacketLength>(
                 kEthernetHeaderBytes + kIPv4BaseHeaderBytes + kTCPBaseHeaderBytes + 16),
             .options = 0},
        }};

        DataPlaneBatchResult result = process_packet_batch(
            umem.data(),
            std::span<const PacketDescriptor>{descs},
            std::span<const ChaosRule>{zero_prob},
            wheel,
            std::span<ProcessedFrame>{tx_out},
            std::span<ProcessedFrame>{recycle_out});
        assert(result.stats.packets_dropped == 0 &&
               "0% probability rule should not drop!");
    }

    // Edge 5: ConnectionBuffer — reset and re-use
    {
        ConnectionBuffer buffer;
        std::array<Byte, 128> msg{};
        const MessageHeader hdr{.opcode = Opcode::Ping, .sequence = 1};
        const auto enc = encode_message(hdr, std::span<const Byte>{},
                                         std::span<Byte>{msg});
        (void)buffer.append(std::span<const Byte>{msg.data(), enc.bytes_written});
        assert(buffer.has_complete_message());
        buffer.reset();
        assert(!buffer.has_complete_message());
        assert(buffer.readable_bytes() == 0);
    }

    // Edge 6: LatencyHistogram — boundary values
    {
        LatencyHistogram hist;
        hist.record(0);           // <1us bucket
        hist.record(999);         // <1us bucket
        hist.record(1000);        // <10us bucket
        hist.record(999999999);   // <1s bucket
        hist.record(1000000000);  // 1s+ bucket
        assert(hist.total() == 5);
        assert(hist.count(0) == 2 && "Boundary: <1us should have 2");
        assert(hist.count(1) == 1 && "Boundary: <10us should have 1");
        assert(hist.min_ns() == 0);
    }

    // Edge 7: Sparkline — window wraparound
    {
        Sparkline spark;
        for (int i = 0; i < 200; ++i) {
            spark.push(static_cast<double>(i));
        }
        assert(spark.size() == kSparklineWindowSize);
        assert(spark.latest() == 199.0);
        assert(spark.at(0) == 140.0);  // Oldest visible = 200 - 60
    }

    std::cout << " OK (7 edge cases verified)\n";
}

// ===== TEST 7: Determinism Verification ======================================
//
// THE core invariant: same seed + same packet = same action, every time.
// Run 100 trials and verify identical outcomes.
// =============================================================================

void test_determinism() {
    std::cout << "  [7/7] Determinism (100 trials)...";

    constexpr int kTrials = 100;
    constexpr std::uint16_t kPort = 8080;
    constexpr std::size_t kPayloadSize = 48;

    // A rule with 50% drop probability
    const ChaosRule rule{
        .seed = 0x12345678ABCDEF00ULL,
        .delay_ns = 0,
        .id = 42,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = kPort,
        .probability = kProbabilityScale / 2,  // 50%
        .protocol = kIPv4ProtocolTCP,
        .action = PacketAction::Drop,
        .enabled = 1,
    };

    // Build a packet
    alignas(kCacheLineSize) std::array<Byte, 4096> frame{};
    build_tcp_packet(frame.data(), kPort, kPayloadSize);
    const PacketView packet = parse_packet(
        frame.data(),
        kEthernetHeaderBytes + kIPv4BaseHeaderBytes + kTCPBaseHeaderBytes + kPayloadSize);
    assert(packet.ok() && "test packet should parse successfully");

    // First trial — record the decision
    const ChaosDecision reference = decide_for_rule(rule, packet);

    // All subsequent trials must produce the EXACT same result
    for (int trial = 1; trial < kTrials; ++trial) {
        const ChaosDecision decision = decide_for_rule(rule, packet);
        assert(decision.action == reference.action &&
               "Determinism violated: different action on same input!");
        assert(decision.hash == reference.hash &&
               "Determinism violated: different hash on same input!");
        assert(decision.roll == reference.roll &&
               "Determinism violated: different roll on same input!");
    }

    std::cout << " OK (action=" << static_cast<int>(reference.action)
              << " roll=" << reference.roll
              << " hash=0x" << std::hex << reference.hash << std::dec
              << " — identical across " << kTrials << " trials)\n";
}

}  // namespace

int main() {
    std::cout << "=== Chronos-X Stress Tests ===\n\n";

    const auto start = std::chrono::steady_clock::now();

    test_spsc_high_throughput();
    test_spsc_backpressure();
    test_concurrent_rule_swap();
    test_batch_storm();
    test_timing_wheel_flood();
    test_edge_cases();
    test_determinism();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "\n=== All 7 stress tests passed in " << ms << " ms ===\n";
    return 0;
}
