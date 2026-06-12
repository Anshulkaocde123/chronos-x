#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "chronosx/chaos_engine.hpp"
#include "chronosx/types.hpp"

namespace chronosx {

inline constexpr std::uint16_t kProtocolMagic = 0x4358;
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kProtocolHeaderBytes = 24;
inline constexpr std::size_t kMaxControlPayloadBytes = 64 * 1024;
inline constexpr std::size_t kDefaultMaxRules = 128;

enum class Opcode : std::uint8_t {
    Ping = 0x01,
    Pong = 0x02,
    GetStats = 0x10,
    StatsResponse = 0x11,
    AddRule = 0x20,
    RemoveRule = 0x21,
    ClearRules = 0x22,
    ListRules = 0x23,
    RulesResponse = 0x24,
    Error = 0xFF,
};

enum class ProtocolStatus : std::uint8_t {
    Ok = 0,
    BufferTooSmall = 1,
    PayloadTooLarge = 2,
    InvalidMagic = 3,
    InvalidVersion = 4,
    TruncatedPayload = 5,
    CrcMismatch = 6,
    UnknownOpcode = 7,
    InvalidPayload = 8,
    RuleTableFull = 9,
    RuleIndexOutOfRange = 10,
};

[[nodiscard]] constexpr const char* protocol_status_name(const ProtocolStatus status) noexcept {
    switch (status) {
        case ProtocolStatus::Ok:
            return "Ok";
        case ProtocolStatus::BufferTooSmall:
            return "BufferTooSmall";
        case ProtocolStatus::PayloadTooLarge:
            return "PayloadTooLarge";
        case ProtocolStatus::InvalidMagic:
            return "InvalidMagic";
        case ProtocolStatus::InvalidVersion:
            return "InvalidVersion";
        case ProtocolStatus::TruncatedPayload:
            return "TruncatedPayload";
        case ProtocolStatus::CrcMismatch:
            return "CrcMismatch";
        case ProtocolStatus::UnknownOpcode:
            return "UnknownOpcode";
        case ProtocolStatus::InvalidPayload:
            return "InvalidPayload";
        case ProtocolStatus::RuleTableFull:
            return "RuleTableFull";
        case ProtocolStatus::RuleIndexOutOfRange:
            return "RuleIndexOutOfRange";
    }

    return "Unknown";
}

struct MessageHeader {
    std::uint16_t magic{kProtocolMagic};
    std::uint8_t version{kProtocolVersion};
    Opcode opcode{Opcode::Ping};
    std::uint32_t sequence{};
    std::uint32_t payload_length{};
    std::uint32_t crc32{};
    std::uint64_t timestamp_ns{};
};

static_assert(sizeof(MessageHeader) == 24);
static_assert(alignof(MessageHeader) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<MessageHeader>);
static_assert(std::is_standard_layout_v<MessageHeader>);

struct DecodedMessage {
    MessageHeader header{};
    std::span<const Byte> payload{};
};

struct EncodeResult {
    ProtocolStatus status{ProtocolStatus::Ok};
    std::size_t bytes_written{};
};

struct DecodeResult {
    ProtocolStatus status{ProtocolStatus::Ok};
    DecodedMessage message{};
};

[[nodiscard]] constexpr std::uint16_t read_u16_be(const std::span<const Byte> bytes,
                                                  const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] constexpr std::uint32_t read_u32_be(const std::span<const Byte> bytes,
                                                  const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] constexpr std::uint64_t read_u64_be(const std::span<const Byte> bytes,
                                                  const std::size_t offset) noexcept {
    return (static_cast<std::uint64_t>(read_u32_be(bytes, offset)) << 32U) |
           static_cast<std::uint64_t>(read_u32_be(bytes, offset + 4));
}

constexpr void write_u16_be(std::span<Byte> bytes, const std::size_t offset, const std::uint16_t value) noexcept {
    bytes[offset] = static_cast<Byte>((value >> 8U) & 0xFFU);
    bytes[offset + 1] = static_cast<Byte>(value & 0xFFU);
}

constexpr void write_u32_be(std::span<Byte> bytes, const std::size_t offset, const std::uint32_t value) noexcept {
    bytes[offset] = static_cast<Byte>((value >> 24U) & 0xFFU);
    bytes[offset + 1] = static_cast<Byte>((value >> 16U) & 0xFFU);
    bytes[offset + 2] = static_cast<Byte>((value >> 8U) & 0xFFU);
    bytes[offset + 3] = static_cast<Byte>(value & 0xFFU);
}

constexpr void write_u64_be(std::span<Byte> bytes, const std::size_t offset, const std::uint64_t value) noexcept {
    write_u32_be(bytes, offset, static_cast<std::uint32_t>((value >> 32U) & 0xFFFF'FFFFULL));
    write_u32_be(bytes, offset + 4, static_cast<std::uint32_t>(value & 0xFFFF'FFFFULL));
}

namespace detail {

inline constexpr std::uint32_t kCrc32Polynomial = 0xEDB88320U;

[[nodiscard]] consteval std::array<std::uint32_t, 256> generate_crc32_table() {
    std::array<std::uint32_t, 256> table{};

    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t crc = index;
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ kCrc32Polynomial : crc >> 1U;
        }
        table[index] = crc;
    }

    return table;
}

inline constexpr auto kCrc32Table = generate_crc32_table();

}  // namespace detail

[[nodiscard]] inline std::uint32_t crc32_update(std::uint32_t crc, std::span<const Byte> bytes) noexcept {
    for (const Byte byte : bytes) {
        const auto index = static_cast<std::uint8_t>((crc ^ byte) & 0xFFU);
        crc = (crc >> 8U) ^ detail::kCrc32Table[index];
    }

    return crc;
}

[[nodiscard]] inline std::uint32_t crc32(std::span<const Byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFF'FFFFU;
    crc = crc32_update(crc, bytes);
    return crc ^ 0xFFFF'FFFFU;
}

inline void write_header_without_crc(const MessageHeader& header, std::span<Byte> output) noexcept {
    write_u16_be(output, 0, header.magic);
    output[2] = header.version;
    output[3] = static_cast<Byte>(header.opcode);
    write_u32_be(output, 4, header.sequence);
    write_u32_be(output, 8, header.payload_length);
    write_u32_be(output, 12, 0);
    write_u64_be(output, 16, header.timestamp_ns);
}

[[nodiscard]] inline EncodeResult encode_message(const MessageHeader& header,
                                                 std::span<const Byte> payload,
                                                 std::span<Byte> output) noexcept {
    if (payload.size() > kMaxControlPayloadBytes) {
        return EncodeResult{.status = ProtocolStatus::PayloadTooLarge};
    }

    const std::size_t total_size = kProtocolHeaderBytes + payload.size();
    if (output.size() < total_size) {
        return EncodeResult{.status = ProtocolStatus::BufferTooSmall};
    }

    MessageHeader normalized = header;
    normalized.magic = kProtocolMagic;
    normalized.version = kProtocolVersion;
    normalized.payload_length = static_cast<std::uint32_t>(payload.size());
    normalized.crc32 = 0;

    std::span<Byte> message = output.first(total_size);
    write_header_without_crc(normalized, message.first(kProtocolHeaderBytes));

    if (!payload.empty()) {
        std::memcpy(message.data() + kProtocolHeaderBytes, payload.data(), payload.size());
    }

    const std::uint32_t computed_crc = crc32(std::span<const Byte>{message.data(), message.size()});
    write_u32_be(message, 12, computed_crc);

    return EncodeResult{.status = ProtocolStatus::Ok, .bytes_written = total_size};
}

[[nodiscard]] inline DecodeResult decode_message(std::span<const Byte> input) noexcept {
    if (input.size() < kProtocolHeaderBytes) {
        return DecodeResult{.status = ProtocolStatus::BufferTooSmall};
    }

    MessageHeader header{};
    header.magic = read_u16_be(input, 0);
    header.version = input[2];
    header.opcode = static_cast<Opcode>(input[3]);
    header.sequence = read_u32_be(input, 4);
    header.payload_length = read_u32_be(input, 8);
    header.crc32 = read_u32_be(input, 12);
    header.timestamp_ns = read_u64_be(input, 16);

    if (header.magic != kProtocolMagic) {
        return DecodeResult{.status = ProtocolStatus::InvalidMagic};
    }

    if (header.version != kProtocolVersion) {
        return DecodeResult{.status = ProtocolStatus::InvalidVersion};
    }

    if (header.payload_length > kMaxControlPayloadBytes) {
        return DecodeResult{.status = ProtocolStatus::PayloadTooLarge};
    }

    const std::size_t total_size = kProtocolHeaderBytes + static_cast<std::size_t>(header.payload_length);
    if (input.size() < total_size) {
        return DecodeResult{.status = ProtocolStatus::TruncatedPayload};
    }

    std::array<Byte, kProtocolHeaderBytes> header_for_crc{};
    std::memcpy(header_for_crc.data(), input.data(), kProtocolHeaderBytes);
    write_u32_be(std::span<Byte>{header_for_crc}, 12, 0);

    std::uint32_t computed_crc = 0xFFFF'FFFFU;
    computed_crc = crc32_update(computed_crc, std::span<const Byte>{header_for_crc.data(), header_for_crc.size()});
    if (header.payload_length > 0) {
        const std::span<const Byte> payload_bytes{input.data() + kProtocolHeaderBytes, header.payload_length};
        computed_crc = crc32_update(computed_crc, payload_bytes);
    }
    computed_crc ^= 0xFFFF'FFFFU;

    if (computed_crc != header.crc32) {
        return DecodeResult{.status = ProtocolStatus::CrcMismatch};
    }

    return DecodeResult{
        .status = ProtocolStatus::Ok,
        .message = DecodedMessage{
            .header = header,
            .payload = std::span<const Byte>{input.data() + kProtocolHeaderBytes, header.payload_length},
        },
    };
}

template <std::size_t MaxRules = kDefaultMaxRules>
    requires(MaxRules > 0)
class RuleTable {
public:
    [[nodiscard]] ProtocolStatus add(const ChaosRule& rule) noexcept {
        if (count_ >= MaxRules) {
            return ProtocolStatus::RuleTableFull;
        }

        rules_[count_] = rule;
        ++count_;
        return ProtocolStatus::Ok;
    }

    [[nodiscard]] ProtocolStatus remove_at(const std::size_t index) noexcept {
        if (index >= count_) {
            return ProtocolStatus::RuleIndexOutOfRange;
        }

        for (std::size_t current = index + 1; current < count_; ++current) {
            rules_[current - 1] = rules_[current];
        }

        --count_;
        rules_[count_] = ChaosRule{};
        return ProtocolStatus::Ok;
    }

    void clear() noexcept {
        for (std::size_t index = 0; index < count_; ++index) {
            rules_[index] = ChaosRule{};
        }
        count_ = 0;
    }

    [[nodiscard]] std::span<const ChaosRule> rules() const noexcept {
        return std::span<const ChaosRule>{rules_.data(), count_};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    [[nodiscard]] static consteval std::size_t capacity() noexcept {
        return MaxRules;
    }

private:
    std::array<ChaosRule, MaxRules> rules_{};
    std::size_t count_{};
};

struct ControlResponse {
    ProtocolStatus status{ProtocolStatus::Ok};
    Opcode opcode{Opcode::Pong};
    std::size_t bytes_written{};
};

template <std::size_t MaxRules = kDefaultMaxRules>
    requires(MaxRules > 0)
class ControlCore {
public:
    [[nodiscard]] ControlResponse handle(std::span<const Byte> request,
                                         std::span<Byte> response) noexcept {
        const DecodeResult decoded = decode_message(request);
        if (decoded.status != ProtocolStatus::Ok) {
            return encode_error(decoded.status, 0, response);
        }

        const DecodedMessage& message = decoded.message;
        switch (message.header.opcode) {
            case Opcode::Ping:
                return encode_empty(Opcode::Pong, message.header.sequence, response);
            case Opcode::GetStats:
                return handle_get_stats(message.header.sequence, response);
            case Opcode::AddRule:
                return handle_add_rule(message, response);
            case Opcode::RemoveRule:
                return handle_remove_rule(message, response);
            case Opcode::ClearRules:
                rule_table_.clear();
                return encode_empty(Opcode::Pong, message.header.sequence, response);
            case Opcode::ListRules:
                return handle_list_rules(message.header.sequence, response);
            default:
                return encode_error(ProtocolStatus::UnknownOpcode, message.header.sequence, response);
        }
    }

    void publish_stats(const StatUpdate& update) noexcept {
        latest_stats_ = update;
        have_stats_ = true;
    }

    [[nodiscard]] std::span<const ChaosRule> rules() const noexcept {
        return rule_table_.rules();
    }

    [[nodiscard]] std::size_t rule_count() const noexcept {
        return rule_table_.size();
    }

private:
    [[nodiscard]] ControlResponse encode_empty(const Opcode opcode,
                                               const std::uint32_t sequence,
                                               std::span<Byte> response) const noexcept {
        const MessageHeader header{.opcode = opcode, .sequence = sequence};
        const EncodeResult encoded = encode_message(header, std::span<const Byte>{}, response);
        return ControlResponse{.status = encoded.status, .opcode = opcode, .bytes_written = encoded.bytes_written};
    }

    [[nodiscard]] ControlResponse encode_payload(const Opcode opcode,
                                                 const std::uint32_t sequence,
                                                 std::span<const Byte> payload,
                                                 std::span<Byte> response) const noexcept {
        const MessageHeader header{.opcode = opcode, .sequence = sequence};
        const EncodeResult encoded = encode_message(header, payload, response);
        return ControlResponse{.status = encoded.status, .opcode = opcode, .bytes_written = encoded.bytes_written};
    }

    [[nodiscard]] ControlResponse encode_error(const ProtocolStatus status,
                                               const std::uint32_t sequence,
                                               std::span<Byte> response) const noexcept {
        const char* message = protocol_status_name(status);
        const std::span<const Byte> payload{reinterpret_cast<const Byte*>(message), std::strlen(message)};
        const EncodeResult encoded = encode_message(MessageHeader{.opcode = Opcode::Error, .sequence = sequence},
                                                    payload,
                                                    response);
        return ControlResponse{.status = status, .opcode = Opcode::Error, .bytes_written = encoded.bytes_written};
    }

    [[nodiscard]] ControlResponse handle_get_stats(const std::uint32_t sequence,
                                                   std::span<Byte> response) const noexcept {
        const StatUpdate stats = have_stats_ ? latest_stats_ : StatUpdate{};
        return encode_payload(Opcode::StatsResponse,
                              sequence,
                              std::span<const Byte>{reinterpret_cast<const Byte*>(&stats), sizeof(stats)},
                              response);
    }

    [[nodiscard]] ControlResponse handle_add_rule(const DecodedMessage& message,
                                                  std::span<Byte> response) noexcept {
        if (message.payload.size() != sizeof(ChaosRule)) {
            return encode_error(ProtocolStatus::InvalidPayload, message.header.sequence, response);
        }

        ChaosRule rule{};
        std::memcpy(&rule, message.payload.data(), sizeof(rule));

        const ProtocolStatus status = rule_table_.add(rule);
        if (status != ProtocolStatus::Ok) {
            return encode_error(status, message.header.sequence, response);
        }

        return encode_empty(Opcode::Pong, message.header.sequence, response);
    }

    [[nodiscard]] ControlResponse handle_remove_rule(const DecodedMessage& message,
                                                     std::span<Byte> response) noexcept {
        if (message.payload.size() != sizeof(std::uint32_t)) {
            return encode_error(ProtocolStatus::InvalidPayload, message.header.sequence, response);
        }

        const std::size_t index = read_u32_be(message.payload, 0);
        const ProtocolStatus status = rule_table_.remove_at(index);
        if (status != ProtocolStatus::Ok) {
            return encode_error(status, message.header.sequence, response);
        }

        return encode_empty(Opcode::Pong, message.header.sequence, response);
    }

    [[nodiscard]] ControlResponse handle_list_rules(const std::uint32_t sequence,
                                                    std::span<Byte> response) const noexcept {
        const std::span<const ChaosRule> active_rules = rule_table_.rules();
        const std::span<const Byte> payload{reinterpret_cast<const Byte*>(active_rules.data()),
                                            active_rules.size_bytes()};
        return encode_payload(Opcode::RulesResponse, sequence, payload, response);
    }

    RuleTable<MaxRules> rule_table_{};
    StatUpdate latest_stats_{};
    bool have_stats_{};
};

}  // namespace chronosx
