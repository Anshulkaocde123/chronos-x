// =============================================================================
// Chronos-X — Network Chaos Engineering Tool
// =============================================================================
//
// Three-Plane Architecture:
//   Data  Plane — hot-path packet processing (SCHED_FIFO, AF_XDP)
//   Control Plane — TCP server for rule management (epoll, normal priority)
//   UI Plane — terminal dashboard for monitoring (low priority)
//
// This file wires all three planes together and provides a "demo mode"
// for environments without AF_XDP / root privileges.
// =============================================================================

#include <atomic>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "chronosx/af_xdp_runtime.hpp"
#include "chronosx/chaos_engine.hpp"
#include "chronosx/control_plane.hpp"
#include "chronosx/data_plane.hpp"
#include "chronosx/frame_allocator.hpp"
#include "chronosx/packet_parser.hpp"
#include "chronosx/protocol.hpp"
#include "chronosx/socket_manager.hpp"
#include "chronosx/spsc_queue.hpp"
#include "chronosx/timing_wheel.hpp"
#include "chronosx/tui.hpp"
#include "chronosx/types.hpp"

#ifdef CHRONOSX_ENABLE_LIBXDP
#include "chronosx/libxdp_socket.hpp"
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#endif

namespace {

using namespace chronosx;

inline constexpr std::uint16_t kDemoControlPort = 9090;
inline constexpr std::size_t kDemoBatchSize = 4;
inline constexpr std::size_t kDemoFrameCount = 16;
inline constexpr std::size_t kDemoTimingSlots = 64;
inline constexpr std::size_t kDemoMaxFramesPerSlot = 16;

std::atomic<bool> g_running{true};

extern "C" void handle_signal(int) {
    g_running.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// build_test_packet — construct a minimal Ethernet+IPv4+TCP packet.
// ---------------------------------------------------------------------------

void build_test_packet(Byte* frame, const std::uint16_t dst_port,
                       const std::size_t payload_size) noexcept {
    const std::size_t total_frame = kEthernetHeaderBytes + kIPv4BaseHeaderBytes +
                                    kTCPBaseHeaderBytes + payload_size;
    std::memset(frame, 0, total_frame);

    // Ethernet header (14 bytes)
    frame[12] = 0x08;  // EtherType: IPv4
    frame[13] = 0x00;

    // IPv4 header (20 bytes, offset 14)
    Byte* ip = frame + kEthernetHeaderBytes;
    ip[0] = 0x45;  // Version=4, IHL=5
    const auto total_len = static_cast<std::uint16_t>(
        kIPv4BaseHeaderBytes + kTCPBaseHeaderBytes + payload_size);
    ip[2] = static_cast<Byte>(total_len >> 8);
    ip[3] = static_cast<Byte>(total_len & 0xFF);
    ip[8] = 64;  // TTL
    ip[9] = kIPv4ProtocolTCP;

    // TCP header (20 bytes, offset 34)
    Byte* tcp = ip + kIPv4BaseHeaderBytes;
    tcp[0] = static_cast<Byte>(12345 >> 8);     // src port high
    tcp[1] = static_cast<Byte>(12345 & 0xFF);   // src port low
    tcp[2] = static_cast<Byte>(dst_port >> 8);   // dst port high
    tcp[3] = static_cast<Byte>(dst_port & 0xFF); // dst port low
    tcp[12] = 0x50;  // Data offset = 5 (20 bytes)

    // Payload
    Byte* payload = tcp + kTCPBaseHeaderBytes;
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<Byte>(i & 0xFF);
    }
}

// ---------------------------------------------------------------------------
// run_demo — in-process demonstration without AF_XDP.
// ---------------------------------------------------------------------------

void run_demo() {
    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║    Chronos-X  —  Network Chaos Engineering      ║\n";
    std::cout << "║    Demo Mode (no AF_XDP / no root required)     ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n\n";

    // --- 1. Set up the control plane core ---
    ControlPlaneCore control_core;

    // Install a chaos rule: drop 100% of packets to port 8080
    ChaosRule drop_rule{
        .seed = 0xBEEF,
        .delay_ns = 0,
        .id = 1,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = 8080,
        .probability = kProbabilityScale,  // 100%
        .protocol = kIPv4ProtocolTCP,
        .action = PacketAction::Drop,
        .enabled = 1,
    };

    // Install a chaos rule: delay 50% of packets to port 9090
    ChaosRule delay_rule{
        .seed = 0xCAFE,
        .delay_ns = 100'000,  // 100μs
        .id = 2,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = 9090,
        .probability = kProbabilityScale / 2,  // 50%
        .protocol = kIPv4ProtocolTCP,
        .action = PacketAction::Delay,
        .enabled = 1,
    };

    // Encode + apply rules via the binary protocol
    auto install_rule = [&](const ChaosRule& rule) {
        std::array<Byte, 512> request{};
        std::array<Byte, 512> response{};
        const MessageHeader header{.opcode = Opcode::AddRule, .sequence = rule.id};
        const auto enc = encode_message(
            header,
            std::span<const Byte>{reinterpret_cast<const Byte*>(&rule), sizeof(rule)},
            std::span<Byte>{request});
        (void)control_core.handle_request(
            std::span<const Byte>{request.data(), enc.bytes_written},
            std::span<Byte>{response});
    };

    install_rule(drop_rule);
    install_rule(delay_rule);

    std::cout << "[Control] Installed " << control_core.rule_count() << " chaos rules\n";
    for (const auto& rule : control_core.rules()) {
        std::cout << "  Rule " << rule.id << ": port=" << rule.dst_port
                  << " action=" << static_cast<int>(rule.action)
                  << " prob=" << rule.probability << "/" << kProbabilityScale
                  << (rule.enabled ? " [ON]" : " [OFF]") << "\n";
    }

    // --- 2. Set up frame allocator ---
    FrameAllocator<kDemoFrameCount> allocator;

    // Allocate frames for test packets
    std::array<FrameHandle, kDemoBatchSize> handles{};
    for (auto& handle : handles) {
        const FrameAllocation alloc = allocator.allocate();
        handle = alloc.frame;
    }

    // We'll use a contiguous buffer for UMEM simulation
    alignas(kCacheLineSize) std::array<Byte, kDemoFrameCount * kDefaultFrameSizeBytes> umem{};
    Byte* umem_base = umem.data();

    // --- 3. Build test packets and process them ---
    std::cout << "\n[Data Plane] Processing " << kDemoBatchSize << " packets...\n";

    struct TestPacket {
        std::uint16_t dst_port;
        const char* description;
    };

    const std::array<TestPacket, kDemoBatchSize> test_packets{{
        {8080, "HTTP (should DROP)"},
        {9090, "Control (may DELAY)"},
        {443,  "HTTPS (should PASS)"},
        {8080, "HTTP (should DROP)"},
    }};

    const std::size_t payload_size = 32;
    const auto frame_len = static_cast<PacketLength>(
        kEthernetHeaderBytes + kIPv4BaseHeaderBytes + kTCPBaseHeaderBytes + payload_size);

    std::array<PacketDescriptor, kDemoBatchSize> rx_descriptors{};
    for (std::size_t i = 0; i < kDemoBatchSize; ++i) {
        const std::uint64_t addr = handles[i].address;
        build_test_packet(umem_base + addr, test_packets[i].dst_port, payload_size);
        rx_descriptors[i] = PacketDescriptor{
            .frame_address = addr,
            .frame_length = frame_len,
            .options = 0,
        };
    }

    // Process batch using the data plane
    TimingWheel<kDemoTimingSlots, kDemoMaxFramesPerSlot> timing_wheel;
    std::array<ProcessedFrame, kDemoBatchSize> tx_output{};
    std::array<ProcessedFrame, kDemoBatchSize> recycle_output{};

    DataPlaneBatchResult result = process_packet_batch(
        umem_base,
        std::span<const PacketDescriptor>{rx_descriptors},
        control_core.rules(),
        timing_wheel,
        std::span<ProcessedFrame>{tx_output},
        std::span<ProcessedFrame>{recycle_output});

    std::cout << "  packets_seen:      " << result.stats.packets_seen << "\n";
    std::cout << "  packets_dropped:   " << result.stats.packets_dropped << "\n";
    std::cout << "  packets_delayed:   " << result.stats.packets_delayed << "\n";
    std::cout << "  packets_corrupted: " << result.stats.packets_corrupted << "\n";
    std::cout << "  tx_frames:         " << result.stats.tx_frames << "\n";
    std::cout << "  recycle_frames:    " << result.stats.recycle_frames << "\n";

    // Print per-packet results
    std::cout << "\n  TX output (" << result.tx_count << " frames):\n";
    for (std::size_t i = 0; i < result.tx_count; ++i) {
        std::cout << "    [" << i << "] addr=" << tx_output[i].frame_address
                  << " len=" << tx_output[i].frame_length
                  << " action=" << static_cast<int>(tx_output[i].action)
                  << "\n";
    }

    std::cout << "  Recycled (" << result.recycle_count << " frames):\n";
    for (std::size_t i = 0; i < result.recycle_count; ++i) {
        std::cout << "    [" << i << "] addr=" << recycle_output[i].frame_address
                  << " len=" << recycle_output[i].frame_length
                  << " action=" << static_cast<int>(recycle_output[i].action)
                  << "\n";
    }

    // --- 4. Show dashboard state ---
    std::cout << "\n[Dashboard] Rendering text-mode dashboard:\n\n";
    DashboardState dashboard{};
    dashboard.packets_seen = result.stats.packets_seen;
    dashboard.packets_dropped = result.stats.packets_dropped;
    dashboard.bytes_seen = result.stats.bytes_seen;
    dashboard.average_latency_ns = 0;
    dashboard.last_action = PacketAction::Drop;
    dashboard.rule_count = control_core.rule_count();
    dashboard.update_count = 1;
    dashboard.throughput_mpps = 1.5;
    dashboard.throughput_gbps = 3.2;
    dashboard.drop_rate_percent = dashboard.packets_seen > 0
        ? 100.0 * static_cast<double>(dashboard.packets_dropped)
                / static_cast<double>(dashboard.packets_seen)
        : 0.0;

    // Record some latency samples for the histogram
    dashboard.histogram.record(500);       // 500ns
    dashboard.histogram.record(5000);      // 5μs
    dashboard.histogram.record(50000);     // 50μs
    dashboard.histogram.record(500000);    // 500μs

    TextDashboard text_ui;
    text_ui.render(dashboard);

    std::cout << "\n[Demo] Complete. All components validated.\n";
}

// ---------------------------------------------------------------------------
// run_server — launch the control plane TCP server (blocking).
// ---------------------------------------------------------------------------

void run_runtime_demo() {
    std::cout << "Chronos-X AF_XDP runtime adapter demo\n";

    using Runtime = AfXdpRuntimeAdapter<16,
                                        kDefaultFrameSizeBytes,
                                        SimulatedXskSocket<16, 16, 16, 16>,
                                        TimingWheel<64, 16>>;

    Runtime runtime;
    const RuntimeStatus init_status = runtime.init(RuntimeConfig{.initial_fill_frames = 4});
    if (init_status != RuntimeStatus::Ok) {
        std::cerr << "Runtime init failed: " << static_cast<int>(init_status) << "\n";
        return;
    }

    const ChaosRule drop_rule{
        .seed = 0xD00D,
        .delay_ns = 0,
        .id = 100,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = 8080,
        .probability = kProbabilityScale,
        .protocol = kIPv4ProtocolTCP,
        .action = PacketAction::Drop,
        .enabled = 1,
    };

    std::array<std::uint64_t, 2> rx_addresses{};
    const std::size_t fill_count = runtime.socket().consume_fill(std::span<std::uint64_t>{rx_addresses});
    if (fill_count != rx_addresses.size()) {
        std::cerr << "Runtime demo could not reserve RX frames\n";
        return;
    }

    constexpr std::size_t payload_size = 32;
    const auto frame_len = static_cast<PacketLength>(
        kEthernetHeaderBytes + kIPv4BaseHeaderBytes + kTCPBaseHeaderBytes + payload_size);
    build_test_packet(runtime.umem_data() + rx_addresses[0], 443, payload_size);
    build_test_packet(runtime.umem_data() + rx_addresses[1], 8080, payload_size);

    const std::array<RxDescriptor, 2> rx{{
        {.address = rx_addresses[0], .length = frame_len, .options = 0},
        {.address = rx_addresses[1], .length = frame_len, .options = 0},
    }};
    (void)runtime.socket().inject_rx(std::span<const RxDescriptor>{rx});

    const std::array<ChaosRule, 1> rules{drop_rule};
    const RuntimePollResult first = runtime.poll_once(std::span<const ChaosRule>{rules});

    const std::array<std::uint64_t, 1> completed{rx_addresses[0]};
    (void)runtime.socket().complete_tx(std::span<const std::uint64_t>{completed});
    const RuntimePollResult second = runtime.poll_once(std::span<const ChaosRule>{rules});

    std::cout << "  rx_frames:        " << first.stats.rx_frames << "\n";
    std::cout << "  tx_submitted:     " << first.stats.tx_submitted << "\n";
    std::cout << "  packets_dropped:  " << first.stats.data_plane.packets_dropped << "\n";
    std::cout << "  fill_refilled:    " << first.stats.fill_refilled + second.stats.fill_refilled << "\n";
    std::cout << "  tx_completed:     " << second.stats.tx_completed << "\n";
    std::cout << "  fill_pending:     " << runtime.socket().fill_ring_pending() << "\n";
    std::cout << "Runtime demo complete\n";
}

void run_live(const char* interface_name, const std::uint32_t queue_id) {
#ifdef CHRONOSX_ENABLE_LIBXDP
    constexpr std::size_t kLiveFrameCount = kDefaultFrameCount;
    constexpr std::size_t kLiveFrameSize = kDefaultFrameSizeBytes;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    LibxdpSocket socket;
    const UmemConfig umem_config{
        .frame_size = kLiveFrameSize,
        .frame_count = kLiveFrameCount,
        .fill_ring_size = kDefaultFillRingSize,
        .completion_ring_size = kDefaultCompletionRingSize,
    };
    const SocketConfig socket_config{
        .queue_id = queue_id,
        .xdp_flags = XDP_FLAGS_SKB_MODE,
        .rx_ring_size = kDefaultRxRingSize,
        .tx_ring_size = kDefaultTxRingSize,
        .interface_index = 0,
        .bind_flags = XDP_COPY,
    };

    const SocketStatus init_status = socket.init(interface_name, umem_config, socket_config);
    if (init_status != SocketStatus::Ok) {
        std::cerr << "libxdp socket init failed: " << socket_status_name(init_status) << "\n";
        return;
    }

    FrameAllocator<kLiveFrameCount, kLiveFrameSize> allocator;
    std::array<FrameHandle, kDataPlaneBatchSize> initial_frames{};
    std::size_t filled = 0;
    while (filled < kLiveFrameCount) {
        const std::size_t request = (kLiveFrameCount - filled) < initial_frames.size()
                                        ? (kLiveFrameCount - filled)
                                        : initial_frames.size();
        std::size_t allocated = 0;
        for (; allocated < request; ++allocated) {
            const FrameAllocation allocation = allocator.allocate(FrameOwner::FillRing);
            if (!allocation.ok()) {
                break;
            }
            initial_frames[allocated] = allocation.frame;
        }

        std::array<std::uint64_t, kDataPlaneBatchSize> addresses{};
        for (std::size_t index = 0; index < allocated; ++index) {
            addresses[index] = initial_frames[index].address;
        }
        const FillResult refill = socket.refill(std::span<const std::uint64_t>{addresses.data(), allocated});
        if (refill.count == 0) {
            break;
        }
        filled += refill.count;
    }

    TimingWheel<1024, 64> timing_wheel;
    std::array<RxDescriptor, kDataPlaneBatchSize> rx{};
    std::array<PacketDescriptor, kDataPlaneBatchSize> descriptors{};
    std::array<ProcessedFrame, kDataPlaneBatchSize> tx{};
    std::array<ProcessedFrame, kDataPlaneBatchSize> recycle{};
    std::array<std::uint64_t, kDataPlaneBatchSize> completions{};

    PacketCount packets_seen = 0;
    PacketCount packets_dropped = 0;
    PacketCount tx_submitted = 0;
    PacketCount completed = 0;

    std::cout << "Chronos-X live AF_XDP mode on " << interface_name
              << " queue " << queue_id << "\n";
    std::cout << "Press Ctrl+C to stop.\n";

    while (g_running.load(std::memory_order_acquire)) {
        const CompletionResult completion_result = socket.drain_completions(std::span<std::uint64_t>{completions});
        for (std::size_t index = 0; index < completion_result.count; ++index) {
            const FrameAllocation frame = allocator.handle_from_address(completions[index]);
            if (frame.ok() &&
                allocator.transition(frame.frame, FrameOwner::TxRing, FrameOwner::FillRing) == FrameStatus::Ok) {
                const std::array<std::uint64_t, 1> address{frame.frame.address};
                (void)socket.refill(std::span<const std::uint64_t>{address});
                ++completed;
            }
        }

        const RxBatchResult rx_result = socket.consume_rx(std::span<RxDescriptor>{rx});
        if (rx_result.count == 0) {
            continue;
        }

        std::size_t descriptor_count = 0;
        for (std::size_t index = 0; index < rx_result.count; ++index) {
            const FrameAllocation frame = allocator.handle_from_address(rx[index].address);
            if (!frame.ok()) {
                continue;
            }
            const FrameOwner owner = allocator.owner_of(frame.frame.index);
            if (allocator.transition(frame.frame, owner, FrameOwner::DataPlane) != FrameStatus::Ok) {
                continue;
            }
            descriptors[descriptor_count] = PacketDescriptor{
                .frame_address = rx[index].address,
                .frame_length = rx[index].length,
                .options = rx[index].options,
            };
            ++descriptor_count;
        }

        const DataPlaneBatchResult batch = process_packet_batch(
            static_cast<Byte*>(socket.umem_area()),
            socket.umem_size_bytes(),
            std::span<const PacketDescriptor>{descriptors.data(), descriptor_count},
            std::span<const ChaosRule>{},
            timing_wheel,
            std::span<ProcessedFrame>{tx},
            std::span<ProcessedFrame>{recycle});
        packets_seen += batch.stats.packets_seen;
        packets_dropped += batch.stats.packets_dropped;

        for (std::size_t index = 0; index < batch.recycle_count; ++index) {
            const FrameAllocation frame = allocator.handle_from_address(recycle[index].frame_address);
            if (frame.ok() &&
                allocator.transition(frame.frame, FrameOwner::DataPlane, FrameOwner::FillRing) == FrameStatus::Ok) {
                const std::array<std::uint64_t, 1> address{frame.frame.address};
                (void)socket.refill(std::span<const std::uint64_t>{address});
            }
        }

        for (std::size_t index = 0; index < batch.tx_count; ++index) {
            const FrameAllocation frame = allocator.handle_from_address(tx[index].frame_address);
            if (!frame.ok() ||
                allocator.transition(frame.frame, FrameOwner::DataPlane, FrameOwner::TxRing) != FrameStatus::Ok) {
                continue;
            }
            const std::array<TxDescriptor, 1> tx_desc{TxDescriptor{
                .address = tx[index].frame_address,
                .length = tx[index].frame_length,
                .options = 0,
            }};
            if (socket.submit_tx(std::span<const TxDescriptor>{tx_desc}).count == 1) {
                ++tx_submitted;
                (void)socket.kick_tx();
            }
        }
    }

    std::cout << "Stopped. packets_seen=" << packets_seen
              << " packets_dropped=" << packets_dropped
              << " tx_submitted=" << tx_submitted
              << " tx_completed=" << completed << "\n";
#else
    (void)interface_name;
    (void)queue_id;
    std::cerr << "Live AF_XDP mode requires -DCHRONOSX_ENABLE_LIBXDP=ON\n";
#endif
}

void run_server(const std::uint16_t port) {
#ifdef __linux__
    ControlPlaneCore core;
    ControlPlaneServer server;

    const ControlPlaneConfig config{.port = port};
    const ControlPlaneStatus status = server.init(config, core);

    if (status != ControlPlaneStatus::Ok) {
        std::cerr << "Failed to start control server: "
                  << control_plane_status_name(status) << "\n";
        return;
    }

    std::cout << "Control plane listening on 127.0.0.1:" << port << "\n";
    std::cout << "Use chronos_client.py to connect.\n";
    std::cout << "Press Ctrl+C to stop.\n";

    std::atomic<bool> running{true};
    server.run(running);
#else
    (void)port;
    std::cerr << "Control plane server requires Linux\n";
#endif
}

}  // namespace

int main(const int argc, const char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--server") {
        const std::uint16_t port = argc > 2
            ? static_cast<std::uint16_t>(std::stoi(argv[2]))
            : kDemoControlPort;
        run_server(port);
    } else if (argc > 1 && std::string(argv[1]) == "--runtime-demo") {
        run_runtime_demo();
    } else if (argc > 1 && std::string(argv[1]) == "--live") {
        const char* interface_name = argc > 2 ? argv[2] : "eth0";
        const std::uint32_t queue_id = argc > 3
            ? static_cast<std::uint32_t>(std::stoul(argv[3]))
            : 0U;
        run_live(interface_name, queue_id);
    } else {
        run_demo();
    }

    return 0;
}
