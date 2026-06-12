// =============================================================================
// tui.hpp — Terminal Dashboard Components
//
// What it does:
//   Provides the data structures and rendering logic for the monitoring
//   dashboard: latency histogram, throughput sparkline, and a text-mode
//   fallback renderer. In FTXUI mode, these feed into FTXUI Elements.
//
// Why a separate TUI layer (not just printf):
//   The data plane produces stats at line rate (~10M updates/sec).
//   The TUI consumes them at 30 FPS. The SPSC queue bridges the gap.
//   drain_stats_queue() pulls all pending updates in a single burst,
//   then the dashboard renders once. This is non-blocking and never
//   stalls the data plane.
//
// Key design decisions:
//   - LatencyHistogram uses 8 fixed buckets (log-scale: <1us to 1s+).
//     Fixed buckets = O(1) recording, no dynamic allocation, cache-friendly.
//   - Sparkline uses a fixed-size circular buffer (60 samples = 60s at 1/sec).
//     No heap allocation, no deque overhead.
//   - TextDashboard uses raw ANSI escape codes for portability.
//     Works over SSH, in CI, without FTXUI installed.
// =============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>

#include "chronosx/spsc_queue.hpp"
#include "chronosx/types.hpp"

namespace chronosx {

inline constexpr std::size_t kHistogramBucketCount = 16;
inline constexpr std::size_t kSparklineWindowSize = 60;
inline constexpr std::size_t kTuiTargetFps = 30;
inline constexpr Nanoseconds kTuiFrameIntervalNs = 1'000'000'000ULL / kTuiTargetFps;

// ---------------------------------------------------------------------------
// LatencyHistogram — fixed-bucket histogram for latency distribution.
//
// Buckets: [0, 1μs), [1μs, 10μs), [10μs, 100μs), [100μs, 1ms),
//          [1ms, 10ms), [10ms, 100ms), [100ms, 1s), [1s+)
// ---------------------------------------------------------------------------

class LatencyHistogram {
public:
    LatencyHistogram() = default;

    void record(const Nanoseconds latency_ns) noexcept {
        const std::size_t bucket = bucket_for(latency_ns);
        ++counts_[bucket];
        ++total_count_;
        total_sum_ns_ += latency_ns;

        if (latency_ns < min_ns_ || total_count_ == 1) {
            min_ns_ = latency_ns;
        }
        if (latency_ns > max_ns_) {
            max_ns_ = latency_ns;
        }
    }

    [[nodiscard]] std::uint64_t count(const std::size_t bucket) const noexcept {
        return bucket < kHistogramBucketCount ? counts_[bucket] : 0;
    }

    [[nodiscard]] std::uint64_t total() const noexcept {
        return total_count_;
    }

    [[nodiscard]] Nanoseconds average_ns() const noexcept {
        return total_count_ > 0 ? total_sum_ns_ / total_count_ : 0;
    }

    [[nodiscard]] Nanoseconds min_ns() const noexcept {
        return min_ns_;
    }

    [[nodiscard]] Nanoseconds max_ns() const noexcept {
        return max_ns_;
    }

    [[nodiscard]] double bucket_fraction(const std::size_t bucket) const noexcept {
        if (total_count_ == 0 || bucket >= kHistogramBucketCount) {
            return 0.0;
        }
        return static_cast<double>(counts_[bucket]) / static_cast<double>(total_count_);
    }

    [[nodiscard]] static constexpr const char* bucket_label(const std::size_t bucket) noexcept {
        constexpr const char* labels[] = {
            "<1us", "<10us", "<100us", "<1ms",
            "<10ms", "<100ms", "<1s", "1s+",
            "", "", "", "", "", "", "", "",
        };
        return bucket < kHistogramBucketCount ? labels[bucket] : "";
    }

    void reset() noexcept {
        counts_ = {};
        total_count_ = 0;
        total_sum_ns_ = 0;
        min_ns_ = 0;
        max_ns_ = 0;
    }

private:
    [[nodiscard]] static constexpr std::size_t bucket_for(const Nanoseconds ns) noexcept {
        if (ns < 1'000) return 0;
        if (ns < 10'000) return 1;
        if (ns < 100'000) return 2;
        if (ns < 1'000'000) return 3;
        if (ns < 10'000'000) return 4;
        if (ns < 100'000'000) return 5;
        if (ns < 1'000'000'000) return 6;
        return 7;
    }

    std::array<std::uint64_t, kHistogramBucketCount> counts_{};
    std::uint64_t total_count_{};
    Nanoseconds total_sum_ns_{};
    Nanoseconds min_ns_{};
    Nanoseconds max_ns_{};
};

// ---------------------------------------------------------------------------
// Sparkline — rolling window of values for sparkline / mini-chart rendering.
// ---------------------------------------------------------------------------

class Sparkline {
public:
    Sparkline() : values_{}, write_pos_{0}, count_{0} {}

    void push(const double value) noexcept {
        values_[write_pos_ % kSparklineWindowSize] = value;
        ++write_pos_;
        if (count_ < kSparklineWindowSize) {
            ++count_;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    [[nodiscard]] double at(const std::size_t index) const noexcept {
        if (index >= count_) return 0.0;

        const std::size_t start = write_pos_ >= count_ ? write_pos_ - count_ : 0;
        return values_[(start + index) % kSparklineWindowSize];
    }

    [[nodiscard]] double latest() const noexcept {
        if (count_ == 0) return 0.0;
        return values_[(write_pos_ - 1) % kSparklineWindowSize];
    }

    [[nodiscard]] double max_value() const noexcept {
        double max = 0.0;
        for (std::size_t index = 0; index < count_; ++index) {
            const double value = at(index);
            if (value > max) max = value;
        }
        return max;
    }

    void reset() noexcept {
        values_ = {};
        write_pos_ = 0;
        count_ = 0;
    }

private:
    std::array<double, kSparklineWindowSize> values_;
    std::size_t write_pos_;
    std::size_t count_;
};

// ---------------------------------------------------------------------------
// DashboardState — aggregated display state consumed by the TUI renderer.
//
// This struct is populated by draining the SPSC queue (try_pop in a loop)
// on the UI thread. It contains everything the dashboard needs to render.
// ---------------------------------------------------------------------------

struct DashboardState {
    PacketCount packets_seen{};
    PacketCount packets_dropped{};
    ByteCount bytes_seen{};
    Nanoseconds average_latency_ns{};
    PacketAction last_action{PacketAction::Pass};

    double throughput_mpps{};
    double throughput_gbps{};
    double drop_rate_percent{};

    LatencyHistogram histogram{};
    Sparkline throughput_sparkline{};
    Sparkline drop_rate_sparkline{};

    std::size_t rule_count{};
    std::size_t update_count{};
};

// ---------------------------------------------------------------------------
// drain_stats_queue — pull all available StatUpdates from the SPSC queue
//                     and merge them into the DashboardState.
// ---------------------------------------------------------------------------

template <std::size_t Capacity>
inline void drain_stats_queue(SPSCQueue<StatUpdate, Capacity>& queue,
                              DashboardState& state) noexcept {
    while (true) {
        const auto update = queue.try_pop();
        if (!update.has_value()) {
            break;
        }

        state.packets_seen = update->packets_seen;
        state.packets_dropped = update->packets_dropped;
        state.bytes_seen = update->bytes_seen;
        state.average_latency_ns = update->average_latency_ns;
        state.last_action = update->last_action;
        ++state.update_count;

        state.histogram.record(update->average_latency_ns);
    }
}

// ---------------------------------------------------------------------------
// TextDashboard — simple text-mode dashboard for environments without FTXUI.
//
// Prints a formatted stats summary to stdout. Useful for:
//   - CI environments
//   - SSH sessions without TERM capability
//   - Debugging/logging mode
// ---------------------------------------------------------------------------

class TextDashboard {
public:
    TextDashboard() = default;

    void render(const DashboardState& state) const noexcept {
        std::cout << "\033[2J\033[H";  // Clear screen + home cursor
        std::cout << "╔══════════════════════════════════════════════════╗\n";
        std::cout << "║         Chronos-X  —  Network Chaos Engine      ║\n";
        std::cout << "╠══════════════════════════════════════════════════╣\n";

        std::cout << "║  Packets Seen:    " << pad_left(state.packets_seen, 12)  << "                 ║\n";
        std::cout << "║  Packets Dropped: " << pad_left(state.packets_dropped, 12) << "                 ║\n";
        std::cout << "║  Bytes Seen:      " << pad_left(state.bytes_seen, 12)    << "                 ║\n";
        std::cout << "║  Avg Latency:     " << pad_left(state.average_latency_ns, 12) << " ns            ║\n";

        std::cout << "╠══════════════════════════════════════════════════╣\n";

        std::cout << "║  Throughput:  ";
        print_gauge(state.throughput_mpps, 10.0, 20);
        std::cout << "  " << fixed2(state.throughput_mpps) << " Mpps   ║\n";

        std::cout << "║  Drop Rate:   ";
        print_gauge(state.drop_rate_percent, 100.0, 20);
        std::cout << "  " << fixed2(state.drop_rate_percent) << " %      ║\n";

        std::cout << "╠══════════════════════════════════════════════════╣\n";
        std::cout << "║  Latency Histogram                              ║\n";

        for (std::size_t bucket = 0; bucket < 8; ++bucket) {
            const double fraction = state.histogram.bucket_fraction(bucket);
            std::cout << "║  " << pad_right(LatencyHistogram::bucket_label(bucket), 8);
            print_bar(fraction, 25);
            std::cout << "  " << fixed1(fraction * 100.0) << "%      ║\n";
        }

        std::cout << "╠══════════════════════════════════════════════════╣\n";
        std::cout << "║  Rules: " << state.rule_count << "  |  Updates: " << state.update_count;
        std::cout << "                        ║\n";
        std::cout << "╚══════════════════════════════════════════════════╝\n";
        std::cout << std::flush;
    }

private:
    static void print_gauge(const double value, const double max_val, const int width) {
        const double fraction = max_val > 0.0 ? value / max_val : 0.0;
        const int filled = static_cast<int>(fraction * static_cast<double>(width));
        std::cout << "[";
        for (int index = 0; index < width; ++index) {
            std::cout << (index < filled ? "█" : "░");
        }
        std::cout << "]";
    }

    static void print_bar(const double fraction, const int width) {
        const int filled = static_cast<int>(fraction * static_cast<double>(width));
        for (int index = 0; index < width; ++index) {
            std::cout << (index < filled ? "▓" : "░");
        }
    }

    static std::string pad_left(const std::uint64_t value, const int width) {
        std::string str = std::to_string(value);
        while (static_cast<int>(str.size()) < width) {
            str = " " + str;
        }
        return str;
    }

    static std::string pad_right(const char* text, const int width) {
        std::string str(text);
        while (static_cast<int>(str.size()) < width) {
            str += " ";
        }
        return str;
    }

    static std::string fixed2(const double value) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%.2f", value);
        return buffer;
    }

    static std::string fixed1(const double value) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        return buffer;
    }
};

}  // namespace chronosx
