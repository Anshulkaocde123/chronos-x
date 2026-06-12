#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

#include "chronosx/af_xdp_runtime.hpp"
#include "chronosx/packet_parser.hpp"

namespace {

using Runtime = chronosx::AfXdpRuntimeAdapter<16,
                                              chronosx::kDefaultFrameSizeBytes,
                                              chronosx::SimulatedXskSocket<16, 16, 16, 16>,
                                              chronosx::TimingWheel<64, 16>>;

void write_be16(chronosx::Byte* data, const std::size_t offset, const std::uint16_t value) {
    data[offset] = static_cast<chronosx::Byte>((value >> 8U) & 0xFFU);
    data[offset + 1] = static_cast<chronosx::Byte>(value & 0xFFU);
}

std::size_t build_tcp_packet(chronosx::Byte* frame, const std::uint16_t dst_port) {
    constexpr std::size_t payload_size = 8;
    constexpr std::size_t frame_size = chronosx::kEthernetHeaderBytes +
                                       chronosx::kIPv4BaseHeaderBytes +
                                       chronosx::kTCPBaseHeaderBytes +
                                       payload_size;
    std::memset(frame, 0, frame_size);
    frame[12] = 0x08;
    frame[13] = 0x00;

    chronosx::Byte* ip = frame + chronosx::kEthernetHeaderBytes;
    ip[0] = 0x45;
    write_be16(ip, 2, static_cast<std::uint16_t>(chronosx::kIPv4BaseHeaderBytes +
                                                chronosx::kTCPBaseHeaderBytes +
                                                payload_size));
    ip[8] = 64;
    ip[9] = chronosx::kIPv4ProtocolTCP;

    chronosx::Byte* tcp = ip + chronosx::kIPv4BaseHeaderBytes;
    write_be16(tcp, 0, 12345);
    write_be16(tcp, 2, dst_port);
    tcp[12] = 0x50;
    return frame_size;
}

void test_pass_tx_completion_refill_cycle() {
    Runtime runtime;
    assert(runtime.init(chronosx::RuntimeConfig{.initial_fill_frames = 2}) ==
           chronosx::RuntimeStatus::Ok);

    std::array<std::uint64_t, 1> fill{};
    assert(runtime.socket().consume_fill(std::span<std::uint64_t>{fill}) == 1);

    const std::size_t length = build_tcp_packet(runtime.umem_data() + fill[0], 443);
    const std::array<chronosx::RxDescriptor, 1> rx{{
        {.address = fill[0], .length = static_cast<std::uint32_t>(length), .options = 0},
    }};
    assert(runtime.socket().inject_rx(std::span<const chronosx::RxDescriptor>{rx}) == 1);

    const auto first = runtime.poll_once(std::span<const chronosx::ChaosRule>{});
    assert(first.status == chronosx::RuntimeStatus::Ok);
    assert(first.stats.rx_frames == 1);
    assert(first.stats.tx_submitted == 1);
    assert(runtime.socket().tx_ring_pending() == 1);

    const std::array<std::uint64_t, 1> completed{fill[0]};
    assert(runtime.socket().complete_tx(std::span<const std::uint64_t>{completed}) == 1);

    const auto second = runtime.poll_once(std::span<const chronosx::ChaosRule>{});
    assert(second.stats.tx_completed == 1);
    assert(second.stats.fill_refilled == 1);
    assert(runtime.allocator().owner_of(0) == chronosx::FrameOwner::FillRing);
}

void test_drop_recycles_to_fill() {
    Runtime runtime;
    assert(runtime.init(chronosx::RuntimeConfig{.initial_fill_frames = 1}) ==
           chronosx::RuntimeStatus::Ok);

    std::array<std::uint64_t, 1> fill{};
    assert(runtime.socket().consume_fill(std::span<std::uint64_t>{fill}) == 1);

    const std::size_t length = build_tcp_packet(runtime.umem_data() + fill[0], 8080);
    const std::array<chronosx::RxDescriptor, 1> rx{{
        {.address = fill[0], .length = static_cast<std::uint32_t>(length), .options = 0},
    }};
    assert(runtime.socket().inject_rx(std::span<const chronosx::RxDescriptor>{rx}) == 1);

    const std::array<chronosx::ChaosRule, 1> rules{{
        {.id = 7,
         .dst_port = 8080,
         .probability = chronosx::kProbabilityScale,
         .protocol = chronosx::kIPv4ProtocolTCP,
         .action = chronosx::PacketAction::Drop,
         .enabled = 1},
    }};

    const auto result = runtime.poll_once(std::span<const chronosx::ChaosRule>{rules});
    assert(result.stats.data_plane.packets_dropped == 1);
    assert(result.stats.tx_submitted == 0);
    assert(result.stats.fill_refilled == 1);
    assert(runtime.socket().fill_ring_pending() == 1);
}

}  // namespace

int main() {
    test_pass_tx_completion_refill_cycle();
    test_drop_recycles_to_fill();

    std::cout << "af_xdp runtime adapter tests passed\n";
    return 0;
}
