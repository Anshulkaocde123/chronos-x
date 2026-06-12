// =============================================================================
// control_plane.hpp — Epoll-Based TCP Control Server + RCU Rule Publishing
//
// What it does:
//   Accepts TCP connections on port 9090, speaks the binary protocol
//   (protocol.hpp), and manages chaos rules. When rules change, publishes
//   a new immutable snapshot via atomic<shared_ptr> so the data plane
//   can read without locks.
//
// Why RCU (Read-Copy-Update) style:
//   The data plane reads rules on EVERY packet (~10M reads/sec).
//   The control plane writes rules maybe once per minute.
//   A mutex would force the data plane to syscall on every batch.
//   With atomic<shared_ptr>, reads are a single atomic load (~5ns).
//   Writes build a new snapshot and swap it in. Old snapshots are freed
//   automatically when the last reader drops its shared_ptr.
//
// Why epoll:
//   We may have multiple admin clients connected simultaneously.
//   epoll gives us O(1) event notification per ready fd, vs O(N) for
//   select/poll. For 16 connections this doesn't matter much, but it's
//   the idiomatically correct Linux approach and scales.
//
// Interview question (D.E. Shaw):
//   "What happens if the control plane crashes mid-rule-update?"
//   Answer: "Nothing bad. We build the complete new RuleSnapshot first,
//   then do a single atomic store. If we crash before the store, the old
//   rules remain. If we crash after, the new rules are in place. There's
//   no intermediate ‘half-updated’ state — this is the key advantage of
//   the copy-on-write / RCU pattern."
// =============================================================================

#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include "chronosx/chaos_engine.hpp"
#include "chronosx/protocol.hpp"
#include "chronosx/types.hpp"

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace chronosx {

inline constexpr std::uint16_t kDefaultControlPort = 9090;
inline constexpr std::size_t kMaxControlConnections = 16;
inline constexpr std::size_t kControlReadBufferBytes = 8192;
inline constexpr std::size_t kControlResponseBufferBytes = 8192;
inline constexpr std::size_t kMaxEpollEvents = 32;

enum class ControlPlaneStatus : std::uint8_t {
    Ok = 0,
    BindFailed = 1,
    ListenFailed = 2,
    EpollCreateFailed = 3,
    AcceptFailed = 4,
    ReadFailed = 5,
    WriteFailed = 6,
    ConnectionClosed = 7,
    TooManyConnections = 8,
    InvalidMessage = 9,
    ShutdownRequested = 10,
    EventFdFailed = 11,
    NotInitialized = 12,
    AlreadyInitialized = 13,
};

[[nodiscard]] constexpr const char* control_plane_status_name(const ControlPlaneStatus status) noexcept {
    switch (status) {
        case ControlPlaneStatus::Ok:
            return "Ok";
        case ControlPlaneStatus::BindFailed:
            return "BindFailed";
        case ControlPlaneStatus::ListenFailed:
            return "ListenFailed";
        case ControlPlaneStatus::EpollCreateFailed:
            return "EpollCreateFailed";
        case ControlPlaneStatus::AcceptFailed:
            return "AcceptFailed";
        case ControlPlaneStatus::ReadFailed:
            return "ReadFailed";
        case ControlPlaneStatus::WriteFailed:
            return "WriteFailed";
        case ControlPlaneStatus::ConnectionClosed:
            return "ConnectionClosed";
        case ControlPlaneStatus::TooManyConnections:
            return "TooManyConnections";
        case ControlPlaneStatus::InvalidMessage:
            return "InvalidMessage";
        case ControlPlaneStatus::ShutdownRequested:
            return "ShutdownRequested";
        case ControlPlaneStatus::EventFdFailed:
            return "EventFdFailed";
        case ControlPlaneStatus::NotInitialized:
            return "NotInitialized";
        case ControlPlaneStatus::AlreadyInitialized:
            return "AlreadyInitialized";
    }

    return "Unknown";
}

// ---------------------------------------------------------------------------
// ConnectionBuffer — per-connection read buffer for partial TCP message
//                    reassembly.
//
// TCP does not preserve message boundaries. A single recv() may return:
//   (a) a partial message
//   (b) exactly one message
//   (c) one-and-a-half messages
//
// This buffer accumulates bytes until a complete protocol message
// (header + payload) is available, then dispatches it to ControlCore.
// ---------------------------------------------------------------------------

class ConnectionBuffer {
public:
    ConnectionBuffer() = default;

    [[nodiscard]] std::size_t append(std::span<const Byte> data) noexcept {
        const std::size_t space = buffer_.size() - write_pos_;
        const std::size_t copy_bytes = data.size() < space ? data.size() : space;
        std::memcpy(buffer_.data() + write_pos_, data.data(), copy_bytes);
        write_pos_ += copy_bytes;
        overflowed_ = overflowed_ || copy_bytes != data.size();
        return copy_bytes;
    }

    [[nodiscard]] std::span<const Byte> readable() const noexcept {
        return std::span<const Byte>{buffer_.data(), write_pos_};
    }

    [[nodiscard]] std::size_t readable_bytes() const noexcept {
        return write_pos_;
    }

    void consume(const std::size_t count) noexcept {
        if (count >= write_pos_) {
            write_pos_ = 0;
            return;
        }

        std::memmove(buffer_.data(), buffer_.data() + count, write_pos_ - count);
        write_pos_ -= count;
    }

    void reset() noexcept {
        write_pos_ = 0;
        overflowed_ = false;
    }

    [[nodiscard]] bool overflowed() const noexcept {
        return overflowed_;
    }

    [[nodiscard]] bool has_complete_message() const noexcept {
        if (write_pos_ < kProtocolHeaderBytes) {
            return false;
        }

        const std::uint32_t payload_length = read_u32_be(
            std::span<const Byte>{buffer_.data(), write_pos_}, 8);
        if (payload_length > kMaxControlPayloadBytes) {
            return true;
        }

        const std::size_t total = kProtocolHeaderBytes + static_cast<std::size_t>(payload_length);
        return write_pos_ >= total;
    }

    [[nodiscard]] std::size_t next_message_length() const noexcept {
        if (write_pos_ < kProtocolHeaderBytes) {
            return 0;
        }

        const std::uint32_t payload_length = read_u32_be(
            std::span<const Byte>{buffer_.data(), write_pos_}, 8);
        if (payload_length > kMaxControlPayloadBytes) {
            return write_pos_;
        }

        return kProtocolHeaderBytes + static_cast<std::size_t>(payload_length);
    }

private:
    std::array<Byte, kControlReadBufferBytes> buffer_{};
    std::size_t write_pos_{};
    bool overflowed_{};
};

// ---------------------------------------------------------------------------
// ControlPlaneConfig
// ---------------------------------------------------------------------------

struct ControlPlaneConfig {
    std::uint16_t port{kDefaultControlPort};
    std::size_t max_connections{kMaxControlConnections};
    int listen_backlog{8};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return port > 0 && max_connections > 0 &&
               max_connections <= kMaxControlConnections &&
               listen_backlog > 0;
    }
};

// ---------------------------------------------------------------------------
// RuleSnapshot — immutable snapshot of rules for lock-free sharing with
//                the data plane via atomic<shared_ptr>.
//
// INTERVIEW (D.E. Shaw): "How do you swap configuration without locking?"
// ANSWER: "RCU-style. Control plane builds a new RuleSnapshot, atomically
//   stores a shared_ptr to it. Data plane loads the ptr once per RX batch
//   (acquire). Old snapshot is freed when the last reader drops its ref."
// ---------------------------------------------------------------------------

class RuleSnapshot {
public:
    RuleSnapshot() = default;

    explicit RuleSnapshot(std::span<const ChaosRule> rules)
        : rules_(rules.begin(), rules.end()) {}

    [[nodiscard]] std::span<const ChaosRule> rules() const noexcept {
        return std::span<const ChaosRule>{rules_.data(), rules_.size()};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return rules_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return rules_.empty();
    }

private:
    std::vector<ChaosRule> rules_{};
};

// Atomic shared_ptr for lock-free rule sharing (C++20 feature)
using AtomicRuleSnapshot = std::atomic<std::shared_ptr<const RuleSnapshot>>;

// ---------------------------------------------------------------------------
// ControlPlaneCore — in-process control plane logic (no sockets).
//
// Wraps ControlCore with the RCU-style atomic rule snapshot publishing.
// This is testable without any network I/O.
// ---------------------------------------------------------------------------

class ControlPlaneCore {
public:
    ControlPlaneCore() {
        rule_snapshot_.store(std::make_shared<const RuleSnapshot>());
    }

    ControlPlaneCore(const ControlPlaneCore&) = delete;
    ControlPlaneCore& operator=(const ControlPlaneCore&) = delete;
    ControlPlaneCore(ControlPlaneCore&&) = delete;
    ControlPlaneCore& operator=(ControlPlaneCore&&) = delete;

    [[nodiscard]] ControlResponse handle_request(std::span<const Byte> request,
                                                  std::span<Byte> response) noexcept {
        // Decode the request opcode BEFORE handling, so we know
        // whether this was a rule-mutating operation.
        const Opcode request_opcode = peek_request_opcode(request);
        const ControlResponse result = control_.handle(request, response);

        // Only publish a new rule snapshot when the request was
        // a rule-mutating operation AND it succeeded.
        if (result.status == ProtocolStatus::Ok &&
            is_rule_mutating_request(request_opcode)) {
            publish_rules();
        }

        return result;
    }

    void publish_stats(const StatUpdate& update) noexcept {
        control_.publish_stats(update);
    }

    [[nodiscard]] std::shared_ptr<const RuleSnapshot> current_rules() const noexcept {
        return rule_snapshot_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t rule_count() const noexcept {
        return control_.rule_count();
    }

    [[nodiscard]] std::span<const ChaosRule> rules() const noexcept {
        return control_.rules();
    }

private:
    // Peek at the opcode of an incoming request without fully decoding.
    // Used to determine whether rule publishing is needed after handling.
    [[nodiscard]] static Opcode peek_request_opcode(std::span<const Byte> request) noexcept {
        // Opcode is at byte offset 3 in the protocol header
        // [magic:2 | version:1 | opcode:1 | ...]
        if (request.size() < 4) {
            return Opcode::Ping;  // Safe fallback — won't trigger publish
        }
        return static_cast<Opcode>(request[3]);
    }

    // Returns true if the REQUEST opcode is one that mutates
    // the rule table and therefore requires a new atomic snapshot.
    [[nodiscard]] static constexpr bool is_rule_mutating_request(const Opcode opcode) noexcept {
        return opcode == Opcode::AddRule ||
               opcode == Opcode::RemoveRule ||
               opcode == Opcode::ClearRules;
    }

    void publish_rules() noexcept {
        auto snapshot = std::make_shared<const RuleSnapshot>(control_.rules());
        rule_snapshot_.store(snapshot, std::memory_order_release);
    }

    ControlCore<> control_{};
    AtomicRuleSnapshot rule_snapshot_{};
};

// ---------------------------------------------------------------------------
// ControlPlaneServer — epoll-based TCP server.
//
// Manages:
//   - Listener socket (non-blocking, SO_REUSEADDR)
//   - Per-connection read buffers (partial message reassembly)
//   - eventfd for shutdown signaling from another thread
//   - Dispatches complete messages to ControlPlaneCore
//
// Call run_once() in a loop, or run() for the blocking event loop.
// ---------------------------------------------------------------------------

#ifdef __linux__

class ControlPlaneServer {
public:
    ControlPlaneServer() = default;

    ControlPlaneServer(const ControlPlaneServer&) = delete;
    ControlPlaneServer& operator=(const ControlPlaneServer&) = delete;
    ControlPlaneServer(ControlPlaneServer&&) = delete;
    ControlPlaneServer& operator=(ControlPlaneServer&&) = delete;

    ~ControlPlaneServer() {
        shutdown();
    }

    [[nodiscard]] ControlPlaneStatus init(const ControlPlaneConfig& config,
                                           ControlPlaneCore& core) noexcept {
        if (initialized_) {
            return ControlPlaneStatus::AlreadyInitialized;
        }

        if (!config.valid()) {
            return ControlPlaneStatus::InvalidMessage;
        }

        config_ = config;
        core_ = &core;

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            return ControlPlaneStatus::BindFailed;
        }

        int opt = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_fd(listen_fd_);
            return ControlPlaneStatus::BindFailed;
        }

        if (::listen(listen_fd_, config.listen_backlog) < 0) {
            close_fd(listen_fd_);
            return ControlPlaneStatus::ListenFailed;
        }

        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            close_fd(listen_fd_);
            return ControlPlaneStatus::EpollCreateFailed;
        }

        event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd_ < 0) {
            close_fd(epoll_fd_);
            close_fd(listen_fd_);
            return ControlPlaneStatus::EventFdFailed;
        }

        if (!add_to_epoll(listen_fd_, EPOLLIN) || !add_to_epoll(event_fd_, EPOLLIN)) {
            close_fd(event_fd_);
            close_fd(epoll_fd_);
            close_fd(listen_fd_);
            return ControlPlaneStatus::EpollCreateFailed;
        }

        initialized_ = true;
        return ControlPlaneStatus::Ok;
    }

    [[nodiscard]] ControlPlaneStatus run_once(const int timeout_ms = -1) noexcept {
        if (!initialized_) {
            return ControlPlaneStatus::NotInitialized;
        }

        std::array<struct epoll_event, kMaxEpollEvents> events{};
        const int nfds = ::epoll_wait(epoll_fd_, events.data(),
                                       static_cast<int>(events.size()), timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR) {
                return ControlPlaneStatus::Ok;
            }
            return ControlPlaneStatus::EpollCreateFailed;
        }

        for (int index = 0; index < nfds; ++index) {
            const int fd = events[static_cast<std::size_t>(index)].data.fd;

            if (fd == event_fd_) {
                return ControlPlaneStatus::ShutdownRequested;
            }

            if (fd == listen_fd_) {
                handle_accept();
                continue;
            }

            handle_client(fd);
        }

        return ControlPlaneStatus::Ok;
    }

    void run(std::atomic<bool>& running) noexcept {
        while (running.load(std::memory_order_relaxed)) {
            const ControlPlaneStatus status = run_once(100);
            if (status == ControlPlaneStatus::ShutdownRequested) {
                break;
            }
        }
    }

    void request_shutdown() noexcept {
        if (event_fd_ >= 0) {
            const std::uint64_t value = 1;
            (void)::write(event_fd_, &value, sizeof(value));
        }
    }

    void shutdown() noexcept {
        for (std::size_t index = 0; index < connection_count_; ++index) {
            close_fd(client_fds_[index]);
        }
        connection_count_ = 0;

        close_fd(event_fd_);
        close_fd(epoll_fd_);
        close_fd(listen_fd_);
        initialized_ = false;
    }

    [[nodiscard]] std::size_t connection_count() const noexcept {
        return connection_count_;
    }

    [[nodiscard]] std::uint64_t messages_handled() const noexcept {
        return messages_handled_;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return config_.port;
    }

private:
    void handle_accept() noexcept {
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int client = ::accept4(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                                      &len, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (client < 0) {
            return;
        }

        if (connection_count_ >= config_.max_connections) {
            ::close(client);
            return;
        }

        if (!add_to_epoll(client, EPOLLIN)) {
            ::close(client);
            return;
        }

        client_fds_[connection_count_] = client;
        buffers_[connection_count_].reset();
        ++connection_count_;
    }

    void handle_client(const int fd) noexcept {
        const std::size_t slot = find_slot(fd);
        if (slot == connection_count_) {
            return;
        }

        std::array<Byte, 4096> read_buf{};
        const ssize_t bytes_read = ::recv(fd, read_buf.data(), read_buf.size(), 0);

        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        if (bytes_read <= 0) {
            remove_connection(slot);
            return;
        }

        const std::size_t appended = buffers_[slot].append(
            std::span<const Byte>{read_buf.data(), static_cast<std::size_t>(bytes_read)});
        if (appended != static_cast<std::size_t>(bytes_read) || buffers_[slot].overflowed()) {
            remove_connection(slot);
            return;
        }

        while (buffers_[slot].has_complete_message()) {
            const std::size_t msg_len = buffers_[slot].next_message_length();
            if (msg_len == 0 || msg_len > buffers_[slot].readable_bytes()) {
                remove_connection(slot);
                return;
            }

            const std::span<const Byte> msg_data = buffers_[slot].readable().first(msg_len);

            std::array<Byte, kControlResponseBufferBytes> response{};
            const ControlResponse result = core_->handle_request(
                msg_data, std::span<Byte>{response});

            buffers_[slot].consume(msg_len);

            if (result.bytes_written > 0) {
                if (!send_all(fd, std::span<const Byte>{response.data(), result.bytes_written})) {
                    remove_connection(slot);
                    return;
                }
            }

            ++messages_handled_;
        }
    }

    void remove_connection(const std::size_t slot) noexcept {
        if (slot >= connection_count_) {
            return;
        }

        close_fd(client_fds_[slot]);

        if (slot < connection_count_ - 1) {
            client_fds_[slot] = client_fds_[connection_count_ - 1];
            buffers_[slot] = buffers_[connection_count_ - 1];
        }

        --connection_count_;
    }

    [[nodiscard]] std::size_t find_slot(const int fd) const noexcept {
        for (std::size_t index = 0; index < connection_count_; ++index) {
            if (client_fds_[index] == fd) {
                return index;
            }
        }
        return connection_count_;
    }

    [[nodiscard]] bool add_to_epoll(const int fd, const std::uint32_t events) noexcept {
        struct epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    [[nodiscard]] static bool send_all(const int fd, std::span<const Byte> bytes) noexcept {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t sent = ::send(fd,
                                        bytes.data() + offset,
                                        bytes.size() - offset,
                                        MSG_NOSIGNAL);
            if (sent < 0 && (errno == EINTR)) {
                continue;
            }
            if (sent <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }

        return true;
    }

    static void close_fd(int& fd) noexcept {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    ControlPlaneConfig config_{};
    ControlPlaneCore* core_{};
    int listen_fd_{-1};
    int epoll_fd_{-1};
    int event_fd_{-1};
    bool initialized_{};

    std::array<int, kMaxControlConnections> client_fds_{};
    std::array<ConnectionBuffer, kMaxControlConnections> buffers_{};
    std::size_t connection_count_{};
    std::uint64_t messages_handled_{};
};

#endif  // __linux__

}  // namespace chronosx
