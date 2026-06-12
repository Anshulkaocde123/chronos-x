#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "chronosx/packet_parser.hpp"
#include "chronosx/types.hpp"

namespace chronosx {

inline constexpr std::uint16_t kProbabilityScale = 10'000;

struct ChaosRule {
    std::uint64_t seed{};
    Nanoseconds delay_ns{};
    std::uint32_t id{};
    std::uint32_t src_ip{};
    std::uint32_t dst_ip{};
    std::uint16_t src_port{};
    std::uint16_t dst_port{};
    std::uint16_t probability{};
    std::uint8_t protocol{};
    PacketAction action{PacketAction::Pass};
    std::uint8_t enabled{1};
    std::uint8_t reserved[3]{};
};

static_assert(sizeof(ChaosRule) == 40);
static_assert(alignof(ChaosRule) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<ChaosRule>);
static_assert(std::is_standard_layout_v<ChaosRule>);

struct ChaosDecision {
    std::uint64_t hash{};
    Nanoseconds delay_ns{};
    std::uint32_t rule_id{};
    std::uint16_t roll{};
    PacketAction action{PacketAction::Pass};
    std::uint8_t matched_rule{};

    [[nodiscard]] constexpr bool matched() const noexcept {
        return matched_rule != 0;
    }

    [[nodiscard]] constexpr bool should_forward() const noexcept {
        return action != PacketAction::Drop;
    }

    [[nodiscard]] constexpr bool should_drop() const noexcept {
        return action == PacketAction::Drop;
    }

    [[nodiscard]] constexpr bool should_delay() const noexcept {
        return action == PacketAction::Delay;
    }

    [[nodiscard]] constexpr bool should_corrupt() const noexcept {
        return action == PacketAction::Corrupt;
    }
};

static_assert(sizeof(ChaosDecision) == 24);
static_assert(alignof(ChaosDecision) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<ChaosDecision>);
static_assert(std::is_standard_layout_v<ChaosDecision>);

[[nodiscard]] constexpr std::uint16_t clamp_probability(const std::uint16_t probability) noexcept {
    return probability > kProbabilityScale ? kProbabilityScale : probability;
}

[[nodiscard]] constexpr bool rule_is_enabled(const ChaosRule& rule) noexcept {
    return rule.enabled != 0;
}

[[nodiscard]] constexpr bool action_forwards(const PacketAction action) noexcept {
    return action != PacketAction::Drop;
}

[[nodiscard]] constexpr bool rule_matches_packet(const ChaosRule& rule, const PacketView& packet) noexcept {
    if (!rule_is_enabled(rule) || !packet.ok()) {
        return false;
    }

    if (rule.src_ip != 0 && rule.src_ip != packet.src_ip) {
        return false;
    }

    if (rule.dst_ip != 0 && rule.dst_ip != packet.dst_ip) {
        return false;
    }

    if (rule.src_port != 0 && rule.src_port != packet.src_port) {
        return false;
    }

    if (rule.dst_port != 0 && rule.dst_port != packet.dst_port) {
        return false;
    }

    if (rule.protocol != 0 && rule.protocol != packet.ipv4_protocol) {
        return false;
    }

    return true;
}

namespace detail {

inline constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
inline constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a_byte(std::uint64_t hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
    return hash;
}

[[nodiscard]] constexpr std::uint64_t fnv1a_u16(std::uint64_t hash, const std::uint16_t value) noexcept {
    hash = fnv1a_byte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    return fnv1a_byte(hash, static_cast<std::uint8_t>(value & 0xFFU));
}

[[nodiscard]] constexpr std::uint64_t fnv1a_u32(std::uint64_t hash, const std::uint32_t value) noexcept {
    hash = fnv1a_byte(hash, static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    hash = fnv1a_byte(hash, static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    hash = fnv1a_byte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    return fnv1a_byte(hash, static_cast<std::uint8_t>(value & 0xFFU));
}

[[nodiscard]] constexpr std::uint64_t fnv1a_u64(std::uint64_t hash, const std::uint64_t value) noexcept {
    hash = fnv1a_u32(hash, static_cast<std::uint32_t>((value >> 32U) & 0xFFFF'FFFFULL));
    return fnv1a_u32(hash, static_cast<std::uint32_t>(value & 0xFFFF'FFFFULL));
}

[[nodiscard]] constexpr std::uint64_t avalanche64(std::uint64_t value) noexcept {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

}

[[nodiscard]] inline std::uint64_t stable_packet_hash(const ChaosRule& rule, const PacketView& packet) noexcept {
    std::uint64_t hash = detail::kFnvOffsetBasis;

    hash = detail::fnv1a_u64(hash, rule.seed);
    hash = detail::fnv1a_u32(hash, packet.src_ip);
    hash = detail::fnv1a_u32(hash, packet.dst_ip);
    hash = detail::fnv1a_u16(hash, packet.src_port);
    hash = detail::fnv1a_u16(hash, packet.dst_port);
    hash = detail::fnv1a_byte(hash, packet.ipv4_protocol);
    hash = detail::fnv1a_u64(hash, static_cast<std::uint64_t>(packet.payload_length));

    const Byte* payload = packet.payload_data();
    if (payload != nullptr) {
        for (std::size_t index = 0; index < packet.payload_length; ++index) {
            hash = detail::fnv1a_byte(hash, payload[index]);
        }
    }

    return detail::avalanche64(hash);
}

[[nodiscard]] inline std::uint64_t stable_flow_hash(const ChaosRule& rule, const PacketView& packet) noexcept {
    std::uint64_t hash = detail::kFnvOffsetBasis;

    hash = detail::fnv1a_u64(hash, rule.seed);
    hash = detail::fnv1a_u32(hash, packet.src_ip);
    hash = detail::fnv1a_u32(hash, packet.dst_ip);
    hash = detail::fnv1a_u16(hash, packet.src_port);
    hash = detail::fnv1a_u16(hash, packet.dst_port);
    hash = detail::fnv1a_byte(hash, packet.ipv4_protocol);
    hash = detail::fnv1a_u64(hash, static_cast<std::uint64_t>(packet.payload_length));

    return detail::avalanche64(hash);
}

[[nodiscard]] constexpr ChaosDecision pass_decision() noexcept {
    return {};
}

[[nodiscard]] inline ChaosDecision pass_from_rule(const ChaosRule& rule,
                                                  const std::uint64_t hash,
                                                  const std::uint16_t roll) noexcept {
    return ChaosDecision{
        .hash = hash,
        .delay_ns = 0,
        .rule_id = rule.id,
        .roll = roll,
        .action = PacketAction::Pass,
        .matched_rule = 1,
    };
}

[[nodiscard]] inline ChaosDecision applied_decision(const ChaosRule& rule,
                                                    const std::uint64_t hash,
                                                    const std::uint16_t roll) noexcept {
    return ChaosDecision{
        .hash = hash,
        .delay_ns = rule.action == PacketAction::Delay ? rule.delay_ns : 0,
        .rule_id = rule.id,
        .roll = roll,
        .action = rule.action,
        .matched_rule = 1,
    };
}

[[nodiscard]] inline ChaosDecision decide_for_rule(const ChaosRule& rule, const PacketView& packet) noexcept {
    const std::uint16_t probability = clamp_probability(rule.probability);

    if (rule.action == PacketAction::Pass || probability == 0) {
        return pass_from_rule(rule, 0, 0);
    }

    if (probability == kProbabilityScale) {
        return applied_decision(rule, 0, 0);
    }

    const std::uint64_t hash = stable_packet_hash(rule, packet);
    const std::uint16_t roll = static_cast<std::uint16_t>(hash % kProbabilityScale);

    if (roll >= probability) {
        return pass_from_rule(rule, hash, roll);
    }

    return applied_decision(rule, hash, roll);
}

[[nodiscard]] inline ChaosDecision decide_for_rule_fast(const ChaosRule& rule, const PacketView& packet) noexcept {
    const std::uint16_t probability = clamp_probability(rule.probability);

    if (rule.action == PacketAction::Pass || probability == 0) {
        return pass_from_rule(rule, 0, 0);
    }

    if (probability == kProbabilityScale) {
        return applied_decision(rule, 0, 0);
    }

    const std::uint64_t hash = stable_flow_hash(rule, packet);
    const std::uint16_t roll = static_cast<std::uint16_t>(hash % kProbabilityScale);

    if (roll >= probability) {
        return pass_from_rule(rule, hash, roll);
    }

    return applied_decision(rule, hash, roll);
}

[[nodiscard]] inline ChaosDecision decide_packet(const PacketView& packet,
                                                 std::span<const ChaosRule> rules) noexcept {
    for (const ChaosRule& rule : rules) {
        if (rule_matches_packet(rule, packet)) {
            return decide_for_rule(rule, packet);
        }
    }

    return pass_decision();
}

[[nodiscard]] inline ChaosDecision decide_packet_fast(const PacketView& packet,
                                                      std::span<const ChaosRule> rules) noexcept {
    for (const ChaosRule& rule : rules) {
        if (rule_matches_packet(rule, packet)) {
            return decide_for_rule_fast(rule, packet);
        }
    }

    return pass_decision();
}

}
