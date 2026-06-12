#pragma once

#include "chronosx/socket_manager.hpp"

#ifdef CHRONOSX_ENABLE_LIBXDP

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <xdp/xsk.h>

namespace chronosx {

class LibxdpSocket {
public:
    LibxdpSocket() = default;

    LibxdpSocket(const LibxdpSocket&) = delete;
    LibxdpSocket& operator=(const LibxdpSocket&) = delete;
    LibxdpSocket(LibxdpSocket&&) = delete;
    LibxdpSocket& operator=(LibxdpSocket&&) = delete;

    ~LibxdpSocket() {
        shutdown();
    }

    [[nodiscard]] SocketStatus init(const char* interface_name,
                                    const UmemConfig& umem_config = {},
                                    const SocketConfig& socket_config = {}) noexcept {
        if (initialized_) {
            return SocketStatus::AlreadyInitialized;
        }
        if (interface_name == nullptr || !umem_config.valid() || !socket_config.valid()) {
            return SocketStatus::InvalidConfig;
        }

        umem_size_ = umem_config.total_bytes();
        umem_area_ = ::mmap(nullptr,
                            umem_size_,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                            -1,
                            0);
        if (umem_area_ == MAP_FAILED) {
            umem_area_ = nullptr;
            return SocketStatus::UmemAllocationFailed;
        }

        const xsk_umem_config xumem_config{
            .fill_size = static_cast<__u32>(umem_config.fill_ring_size),
            .comp_size = static_cast<__u32>(umem_config.completion_ring_size),
            .frame_size = static_cast<__u32>(umem_config.frame_size),
            .frame_headroom = 0,
            .flags = 0,
        };

        if (xsk_umem__create(&umem_,
                             umem_area_,
                             umem_size_,
                             &fill_,
                             &completion_,
                             &xumem_config) != 0) {
            shutdown();
            return SocketStatus::UmemCreateFailed;
        }

        const xsk_socket_config xsocket_config{
            .rx_size = static_cast<__u32>(socket_config.rx_ring_size),
            .tx_size = static_cast<__u32>(socket_config.tx_ring_size),
            .libxdp_flags = 0,
            .xdp_flags = socket_config.xdp_flags,
            .bind_flags = static_cast<__u16>(socket_config.bind_flags),
        };

        if (xsk_socket__create(&socket_,
                               interface_name,
                               socket_config.queue_id,
                               umem_,
                               &rx_,
                               &tx_,
                               &xsocket_config) != 0) {
            shutdown();
            return SocketStatus::SocketCreateFailed;
        }

        initialized_ = true;
        return SocketStatus::Ok;
    }

    void shutdown() noexcept {
        if (socket_ != nullptr) {
            xsk_socket__delete(socket_);
            socket_ = nullptr;
        }
        if (umem_ != nullptr) {
            (void)xsk_umem__delete(umem_);
            umem_ = nullptr;
        }
        if (umem_area_ != nullptr) {
            (void)::munmap(umem_area_, umem_size_);
            umem_area_ = nullptr;
            umem_size_ = 0;
        }
        initialized_ = false;
    }

    [[nodiscard]] FillResult refill(std::span<const std::uint64_t> addresses) noexcept {
        __u32 index = 0;
        const __u32 requested = static_cast<__u32>(addresses.size());
        const __u32 reserved = xsk_ring_prod__reserve(&fill_, requested, &index);
        for (__u32 offset = 0; offset < reserved; ++offset) {
            *xsk_ring_prod__fill_addr(&fill_, index + offset) = addresses[offset];
        }
        if (reserved > 0) {
            xsk_ring_prod__submit(&fill_, reserved);
        }
        return FillResult{.count = reserved,
                          .status = reserved == requested ? SocketStatus::Ok : SocketStatus::RingFull};
    }

    [[nodiscard]] CompletionResult drain_completions(std::span<std::uint64_t> output) noexcept {
        __u32 index = 0;
        const __u32 available = xsk_ring_cons__peek(&completion_, static_cast<__u32>(output.size()), &index);
        for (__u32 offset = 0; offset < available; ++offset) {
            output[offset] = *xsk_ring_cons__comp_addr(&completion_, index + offset);
        }
        if (available > 0) {
            xsk_ring_cons__release(&completion_, available);
        }
        return CompletionResult{.count = available,
                                .status = available == output.size() ? SocketStatus::Ok : SocketStatus::RingEmpty};
    }

    [[nodiscard]] RxBatchResult consume_rx(std::span<RxDescriptor> output) noexcept {
        __u32 index = 0;
        const __u32 available = xsk_ring_cons__peek(&rx_, static_cast<__u32>(output.size()), &index);
        for (__u32 offset = 0; offset < available; ++offset) {
            const xdp_desc* desc = xsk_ring_cons__rx_desc(&rx_, index + offset);
            output[offset] = RxDescriptor{
                .address = desc->addr,
                .length = desc->len,
                .options = desc->options,
            };
        }
        if (available > 0) {
            xsk_ring_cons__release(&rx_, available);
        }
        return RxBatchResult{.count = available,
                             .status = available == output.size() ? SocketStatus::Ok : SocketStatus::RingEmpty};
    }

    [[nodiscard]] TxBatchResult submit_tx(std::span<const TxDescriptor> descriptors) noexcept {
        __u32 index = 0;
        const __u32 requested = static_cast<__u32>(descriptors.size());
        const __u32 reserved = xsk_ring_prod__reserve(&tx_, requested, &index);
        for (__u32 offset = 0; offset < reserved; ++offset) {
            xdp_desc* desc = xsk_ring_prod__tx_desc(&tx_, index + offset);
            desc->addr = descriptors[offset].address;
            desc->len = descriptors[offset].length;
            desc->options = descriptors[offset].options;
        }
        if (reserved > 0) {
            xsk_ring_prod__submit(&tx_, reserved);
        }
        return TxBatchResult{.count = reserved,
                             .status = reserved == requested ? SocketStatus::Ok : SocketStatus::RingFull};
    }

    [[nodiscard]] SocketStatus kick_tx() noexcept {
        if (socket_ == nullptr) {
            return SocketStatus::NotInitialized;
        }
        const int fd = xsk_socket__fd(socket_);
        if (::sendto(fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0) < 0 &&
            errno != EAGAIN && errno != EBUSY && errno != ENOBUFS) {
            return SocketStatus::KickFailed;
        }
        ++tx_kick_count_;
        return SocketStatus::Ok;
    }

    [[nodiscard]] void* umem_area() noexcept {
        return umem_area_;
    }

    [[nodiscard]] std::size_t umem_size_bytes() const noexcept {
        return umem_size_;
    }

    [[nodiscard]] std::uint64_t tx_kick_count() const noexcept {
        return tx_kick_count_;
    }

private:
    xsk_ring_prod fill_{};
    xsk_ring_cons completion_{};
    xsk_ring_cons rx_{};
    xsk_ring_prod tx_{};
    xsk_umem* umem_{};
    xsk_socket* socket_{};
    void* umem_area_{};
    std::size_t umem_size_{};
    std::uint64_t tx_kick_count_{};
    bool initialized_{};
};

}  // namespace chronosx

#endif  // CHRONOSX_ENABLE_LIBXDP
