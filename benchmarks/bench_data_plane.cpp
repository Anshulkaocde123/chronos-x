#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>

#include "chronosx/data_plane.hpp"
#include "chronosx/packet_parser.hpp"
#include "chronosx/types.hpp"

namespace {

constexpr std::size_t kFrameCount = chronosx::kDataPlaneBatchSize;
constexpr std::size_t kFrameStride = 256;
constexpr std::uint64_t kDefaultIterations = 1'000'000;

void write_be16(chronosx::Byte* data, const std::size_t offset, const std::uint16_t value) {
    data[offset] = static_cast<chronosx::Byte>((value >> 8U) & 0xFFU);
    data[offset + 1] = static_cast<chronosx::Byte>(value & 0xFFU);
}

void write_be32(chronosx::Byte* data, const std::size_t offset, const std::uint32_t value) {
    data[offset] = static_cast<chronosx::Byte>((value >> 24U) & 0xFFU);
    data[offset + 1] = static_cast<chronosx::Byte>((value >> 16U) & 0xFFU);
    data[offset + 2] = static_cast<chronosx::Byte>((value >> 8U) & 0xFFU);
    data[offset + 3] = static_cast<chronosx::Byte>(value & 0xFFU);
}

void write_ethernet_header(chronosx::Byte* frame) {
    const chronosx::Byte dst_mac[6]{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const chronosx::Byte src_mac[6]{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    std::memcpy(frame, dst_mac, sizeof(dst_mac));
    std::memcpy(frame + 6, src_mac, sizeof(src_mac));
    write_be16(frame, 12, chronosx::kEtherTypeIPv4);
}

std::size_t build_min_tcp_frame(chronosx::Byte* frame, const std::uint16_t dst_port) {
    constexpr std::size_t payload_length = 10;
    constexpr std::size_t payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                                           chronosx::kTCPBaseHeaderBytes;
    constexpr auto ipv4_total_length = static_cast<std::uint16_t>(chronosx::kIPv4BaseHeaderBytes +
                                                                  chronosx::kTCPBaseHeaderBytes + payload_length);

    std::memset(frame, 0, kFrameStride);
    write_ethernet_header(frame);

    chronosx::Byte* ip = frame + chronosx::kEthernetHeaderBytes;
    ip[0] = static_cast<chronosx::Byte>((4U << 4U) | 5U);
    write_be16(ip, 2, ipv4_total_length);
    ip[8] = 64;
    ip[9] = chronosx::kIPv4ProtocolTCP;
    write_be32(ip, 12, 0x0A000001);
    write_be32(ip, 16, 0x0A000002);

    chronosx::Byte* tcp = ip + chronosx::kIPv4BaseHeaderBytes;
    write_be16(tcp, 0, 12345);
    write_be16(tcp, 2, dst_port);
    tcp[12] = static_cast<chronosx::Byte>(5U << 4U);
    tcp[13] = 0x18;
    write_be16(tcp, 14, 4096);

    for (std::size_t index = 0; index < payload_length; ++index) {
        frame[payload_offset + index] = static_cast<chronosx::Byte>('A' + index);
    }

    return payload_offset + payload_length;
}

std::uint64_t parse_iterations(const int argc, char** argv) {
    if (argc < 2) {
        return kDefaultIterations;
    }

    const auto parsed = std::strtoull(argv[1], nullptr, 10);
    return parsed == 0 ? kDefaultIterations : parsed;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t iterations = parse_iterations(argc, argv);

    std::array<chronosx::Byte, kFrameCount * kFrameStride> umem{};
    std::array<chronosx::PacketDescriptor, kFrameCount> rx{};
    std::array<chronosx::ProcessedFrame, kFrameCount> tx{};
    std::array<chronosx::ProcessedFrame, kFrameCount> recycle{};
    chronosx::TimingWheel<1024, 64> timing_wheel;

    for (std::size_t index = 0; index < kFrameCount; ++index) {
        const std::size_t address = index * kFrameStride;
        const std::size_t length = build_min_tcp_frame(umem.data() + address, 9000);
        rx[index] = chronosx::PacketDescriptor{
            .frame_address = address,
            .frame_length = static_cast<chronosx::PacketLength>(length),
            .options = 0,
        };
    }

    const auto started = std::chrono::steady_clock::now();
    chronosx::PacketCount packets = 0;
    chronosx::ByteCount bytes = 0;
    chronosx::PacketCount tx_frames = 0;

    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        const auto result = chronosx::process_packet_batch(umem.data(),
                                                           std::span<const chronosx::PacketDescriptor>{rx},
                                                           std::span<const chronosx::ChaosRule>{},
                                                           timing_wheel,
                                                           std::span<chronosx::ProcessedFrame>{tx},
                                                           std::span<chronosx::ProcessedFrame>{recycle});
        packets += result.stats.packets_seen;
        bytes += result.stats.bytes_seen;
        tx_frames += result.stats.tx_frames;
    }

    const auto stopped = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = stopped - started;
    const double seconds = elapsed.count();
    const double pps = static_cast<double>(packets) / seconds;
    const double mpps = pps / 1'000'000.0;
    const double gbps = (static_cast<double>(bytes) * 8.0) / seconds / 1'000'000'000.0;

    std::cout << "Chronos-X data-plane core benchmark\n";
    std::cout << "iterations=" << iterations << '\n';
    std::cout << "packets=" << packets << '\n';
    std::cout << "tx_frames=" << tx_frames << '\n';
    std::cout << "seconds=" << seconds << '\n';
    std::cout << "throughput_mpps=" << mpps << '\n';
    std::cout << "throughput_gbps_at_test_frame_size=" << gbps << '\n';

    return tx_frames == packets ? 0 : 1;
}
