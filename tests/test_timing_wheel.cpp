#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>

#include "chronosx/chaos_engine.hpp"
#include "chronosx/packet_parser.hpp"
#include "chronosx/timing_wheel.hpp"
#include "chronosx/types.hpp"

namespace {

chronosx::DelayedFrame make_frame(const std::uint32_t id) {
    return chronosx::DelayedFrame{
        .frame_address = static_cast<std::uint64_t>(id) * 4096U,
        .frame_length = 128,
        .rule_id = id,
        .release_tick = 0,
    };
}

void test_basic_schedule_and_release() {
    chronosx::TimingWheel<8, 4> wheel;

    assert(wheel.schedule(make_frame(1), 3) == chronosx::ScheduleStatus::Scheduled);
    assert(wheel.pending_count() == 1);

    assert(wheel.tick().empty());
    assert(wheel.tick().empty());

    const auto expired = wheel.tick();
    assert(expired.size() == 1);
    assert(expired[0].rule_id == 1);
    assert(expired[0].release_tick == 3);
    assert(wheel.pending_count() == 0);
    assert(wheel.empty());
}

void test_zero_delay_releases_on_next_tick() {
    chronosx::TimingWheel<8, 4> wheel;

    assert(wheel.schedule(make_frame(2), 0) == chronosx::ScheduleStatus::Scheduled);

    const auto expired = wheel.tick();
    assert(expired.size() == 1);
    assert(expired[0].rule_id == 2);
    assert(expired[0].release_tick == 1);
}

void test_multiple_frames_same_slot_keep_fifo_order() {
    chronosx::TimingWheel<8, 8> wheel;

    for (std::uint32_t id = 10; id < 15; ++id) {
        assert(wheel.schedule(make_frame(id), 4) == chronosx::ScheduleStatus::Scheduled);
    }

    for (int tick = 0; tick < 3; ++tick) {
        assert(wheel.tick().empty());
    }

    const auto expired = wheel.tick();
    assert(expired.size() == 5);

    for (std::uint32_t index = 0; index < expired.size(); ++index) {
        assert(expired[index].rule_id == 10 + index);
        assert(expired[index].release_tick == 4);
    }
}

void test_full_rotation_delay() {
    chronosx::TimingWheel<8, 4> wheel;

    assert(wheel.schedule(make_frame(3), wheel.max_delay_ticks()) == chronosx::ScheduleStatus::Scheduled);

    for (int tick = 0; tick < 7; ++tick) {
        assert(wheel.tick().empty());
    }

    const auto expired = wheel.tick();
    assert(expired.size() == 1);
    assert(expired[0].rule_id == 3);
    assert(expired[0].release_tick == 8);
}

void test_rejects_delay_beyond_single_rotation() {
    chronosx::TimingWheel<8, 4> wheel;

    assert(wheel.schedule(make_frame(4), 9) == chronosx::ScheduleStatus::DelayTooLarge);
    assert(wheel.pending_count() == 0);
    assert(wheel.schedule_failures() == 1);
}

void test_slot_capacity_is_explicit() {
    chronosx::TimingWheel<8, 2> wheel;

    assert(wheel.schedule(make_frame(5), 2) == chronosx::ScheduleStatus::Scheduled);
    assert(wheel.schedule(make_frame(6), 2) == chronosx::ScheduleStatus::Scheduled);
    assert(wheel.schedule(make_frame(7), 2) == chronosx::ScheduleStatus::SlotFull);
    assert(wheel.pending_count() == 2);
    assert(wheel.schedule_failures() == 1);

    assert(wheel.tick().empty());
    const auto expired = wheel.tick();
    assert(expired.size() == 2);
    assert(expired[0].rule_id == 5);
    assert(expired[1].rule_id == 6);
}

void test_wraparound_after_many_ticks() {
    chronosx::TimingWheel<8, 4> wheel;

    for (int tick = 0; tick < 20; ++tick) {
        assert(wheel.tick().empty());
    }

    assert(wheel.current_tick() == 20);
    assert(wheel.schedule(make_frame(8), 3) == chronosx::ScheduleStatus::Scheduled);

    assert(wheel.tick().empty());
    assert(wheel.tick().empty());

    const auto expired = wheel.tick();
    assert(expired.size() == 1);
    assert(expired[0].rule_id == 8);
    assert(expired[0].release_tick == 23);
}

void test_delay_decision_schedules_frame() {
    const chronosx::PacketView packet{
        .data = nullptr,
        .frame_length = 0,
        .status = chronosx::ParseStatus::Ok,
        .ether_type = chronosx::kEtherTypeIPv4,
        .ipv4_protocol = chronosx::kIPv4ProtocolUDP,
        .ipv4_header_length = chronosx::kIPv4BaseHeaderBytes,
        .ipv4_total_length = chronosx::kIPv4BaseHeaderBytes + chronosx::kUDPHeaderBytes,
        .src_ip = 0x0A000001,
        .dst_ip = 0x0A000002,
        .l3_offset = chronosx::kEthernetHeaderBytes,
        .l4_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes,
        .payload_offset = chronosx::kEthernetHeaderBytes + chronosx::kIPv4BaseHeaderBytes +
                          chronosx::kUDPHeaderBytes,
        .payload_length = 0,
        .tcp_header_length = 0,
        .src_port = 5353,
        .dst_port = 53,
    };

    const chronosx::ChaosRule rule{
        .seed = 1234,
        .delay_ns = 2'000'000,
        .id = 77,
        .src_ip = 0,
        .dst_ip = 0,
        .src_port = 0,
        .dst_port = 53,
        .probability = chronosx::kProbabilityScale,
        .protocol = chronosx::kIPv4ProtocolUDP,
        .action = chronosx::PacketAction::Delay,
        .enabled = 1,
        .reserved = {},
    };

    const chronosx::ChaosRule rules[]{rule};
    const auto decision = chronosx::decide_packet(packet, std::span<const chronosx::ChaosRule>{rules, 1});
    assert(decision.matched());
    assert(decision.should_delay());
    assert(decision.delay_ns == 2'000'000);

    chronosx::TimingWheel<8, 4> wheel;
    auto frame = make_frame(decision.rule_id);
    frame.frame_length = 512;

    const std::uint64_t delay_ticks = decision.delay_ns / 1'000'000U;
    assert(wheel.schedule(frame, delay_ticks) == chronosx::ScheduleStatus::Scheduled);

    assert(wheel.tick().empty());
    const auto expired = wheel.tick();
    assert(expired.size() == 1);
    assert(expired[0].rule_id == 77);
    assert(expired[0].frame_length == 512);
}

}  // namespace

int main() {
    test_basic_schedule_and_release();
    test_zero_delay_releases_on_next_tick();
    test_multiple_frames_same_slot_keep_fifo_order();
    test_full_rotation_delay();
    test_rejects_delay_beyond_single_rotation();
    test_slot_capacity_is_explicit();
    test_wraparound_after_many_ticks();
    test_delay_decision_schedules_frame();

    std::cout << "timing wheel tests passed\n";
    return 0;
}
