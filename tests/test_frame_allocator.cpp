#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <unordered_set>

#include "chronosx/frame_allocator.hpp"
#include "chronosx/timing_wheel.hpp"

namespace {

void test_layout_and_constants() {
    static_assert(sizeof(chronosx::FrameHandle) == 16);
    static_assert(sizeof(chronosx::FrameAllocation) == 24);
    static_assert(chronosx::FrameAllocator<16, 4096>::capacity() == 16);
    static_assert(chronosx::FrameAllocator<16, 4096>::frame_size_bytes() == 4096);
    static_assert(chronosx::FrameAllocator<16, 4096>::umem_size_bytes() == 16 * 4096);

    assert(chronosx::frame_owner_name(chronosx::FrameOwner::TimingWheel) != nullptr);
    assert(chronosx::frame_status_name(chronosx::FrameStatus::InvalidOwner) != nullptr);
}

void test_initial_allocation_order_and_counts() {
    chronosx::FrameAllocator<8, 4096> allocator;

    assert(allocator.available() == 8);
    assert(allocator.in_use() == 0);
    assert(allocator.full());

    const auto first = allocator.allocate();
    assert(first.ok());
    assert(first.frame.index == 0);
    assert(first.frame.address == 0);
    assert(allocator.available() == 7);
    assert(allocator.in_use() == 1);
    assert(allocator.owner_of(first.frame.index) == chronosx::FrameOwner::DataPlane);
}

void test_allocate_all_frames_are_unique() {
    chronosx::FrameAllocator<16, 4096> allocator;
    std::unordered_set<std::uint64_t> addresses;

    for (std::size_t count = 0; count < allocator.capacity(); ++count) {
        const auto allocation = allocator.allocate(chronosx::FrameOwner::FillRing);
        assert(allocation.ok());
        assert(allocation.frame.address % allocator.frame_size_bytes() == 0);
        assert(addresses.insert(allocation.frame.address).second);
        assert(allocator.owner_of(allocation.frame.index) == chronosx::FrameOwner::FillRing);
    }

    assert(allocator.empty());
    assert(allocator.available() == 0);

    const auto exhausted = allocator.allocate();
    assert(!exhausted.ok());
    assert(exhausted.status == chronosx::FrameStatus::PoolEmpty);
    assert(allocator.allocation_failures() == 1);
}

void test_release_reuses_frame_lifo() {
    chronosx::FrameAllocator<8, 4096> allocator;

    const auto first = allocator.allocate();
    const auto second = allocator.allocate();
    assert(first.ok());
    assert(second.ok());
    assert(first.frame.index == 0);
    assert(second.frame.index == 1);

    assert(allocator.release(second.frame) == chronosx::FrameStatus::Ok);
    assert(allocator.owner_of(second.frame.index) == chronosx::FrameOwner::Free);

    const auto reused = allocator.allocate();
    assert(reused.ok());
    assert(reused.frame.index == second.frame.index);
    assert(reused.frame.address == second.frame.address);
}

void test_double_free_is_rejected() {
    chronosx::FrameAllocator<8, 4096> allocator;

    const auto allocation = allocator.allocate();
    assert(allocation.ok());
    assert(allocator.release(allocation.frame) == chronosx::FrameStatus::Ok);

    const auto second_release = allocator.release(allocation.frame);
    assert(second_release == chronosx::FrameStatus::InvalidOwner);
    assert(allocator.release_failures() == 1);
    assert(allocator.available() == 8);
}

void test_invalid_frame_handle_is_rejected() {
    chronosx::FrameAllocator<8, 4096> allocator;

    const auto allocation = allocator.allocate();
    assert(allocation.ok());

    chronosx::FrameHandle wrong_address = allocation.frame;
    wrong_address.address += 1;
    assert(allocator.release(wrong_address) == chronosx::FrameStatus::InvalidFrameAddress);

    chronosx::FrameHandle wrong_index = allocation.frame;
    wrong_index.index = 99;
    assert(allocator.release(wrong_index) == chronosx::FrameStatus::InvalidFrameIndex);

    assert(allocator.release_failures() == 2);
    assert(allocator.release(allocation.frame) == chronosx::FrameStatus::Ok);
}

void test_owner_transition_state_machine() {
    chronosx::FrameAllocator<8, 4096> allocator;
    const auto allocation = allocator.allocate(chronosx::FrameOwner::FillRing);
    assert(allocation.ok());

    assert(allocator.transition(allocation.frame,
                                chronosx::FrameOwner::FillRing,
                                chronosx::FrameOwner::RxRing) == chronosx::FrameStatus::Ok);
    assert(allocator.transition(allocation.frame,
                                chronosx::FrameOwner::RxRing,
                                chronosx::FrameOwner::DataPlane) == chronosx::FrameStatus::Ok);
    assert(allocator.transition(allocation.frame,
                                chronosx::FrameOwner::DataPlane,
                                chronosx::FrameOwner::TxRing) == chronosx::FrameStatus::Ok);
    assert(allocator.transition(allocation.frame,
                                chronosx::FrameOwner::TxRing,
                                chronosx::FrameOwner::CompletionRing) == chronosx::FrameStatus::Ok);
    assert(allocator.release(allocation.frame,
                             chronosx::FrameOwner::CompletionRing) == chronosx::FrameStatus::Ok);
    assert(allocator.full());
}

void test_wrong_owner_transition_is_rejected() {
    chronosx::FrameAllocator<8, 4096> allocator;
    const auto allocation = allocator.allocate(chronosx::FrameOwner::DataPlane);
    assert(allocation.ok());

    const auto status = allocator.transition(allocation.frame,
                                             chronosx::FrameOwner::RxRing,
                                             chronosx::FrameOwner::TxRing);
    assert(status == chronosx::FrameStatus::InvalidOwner);
    assert(allocator.transition_failures() == 1);
    assert(allocator.owner_of(allocation.frame.index) == chronosx::FrameOwner::DataPlane);
}

void test_batch_allocate_and_release() {
    chronosx::FrameAllocator<8, 4096> allocator;
    std::array<chronosx::FrameHandle, 5> frames{};

    const std::size_t allocated = allocator.allocate_batch(frames, chronosx::FrameOwner::DataPlane);
    assert(allocated == frames.size());
    assert(allocator.available() == 3);

    const auto release_status = allocator.release_batch(std::span<const chronosx::FrameHandle>{frames.data(),
                                                                                               allocated});
    assert(release_status == chronosx::FrameStatus::Ok);
    assert(allocator.full());
}

void test_partial_batch_allocation() {
    chronosx::FrameAllocator<4, 4096> allocator;
    std::array<chronosx::FrameHandle, 6> frames{};

    const std::size_t allocated = allocator.allocate_batch(frames);
    assert(allocated == 4);
    assert(allocator.empty());
    assert(allocator.allocation_failures() == 1);

    assert(allocator.release_batch(std::span<const chronosx::FrameHandle>{frames.data(), allocated}) ==
           chronosx::FrameStatus::Ok);
}

void test_address_to_handle_conversion() {
    chronosx::FrameAllocator<8, 4096> allocator;

    const auto valid = allocator.handle_from_address(4096 * 3);
    assert(valid.ok());
    assert(valid.frame.index == 3);
    assert(valid.frame.address == 4096 * 3);

    const auto misaligned = allocator.handle_from_address(4097);
    assert(!misaligned.ok());
    assert(misaligned.status == chronosx::FrameStatus::InvalidFrameAddress);

    const auto out_of_range = allocator.handle_from_address(4096 * 8);
    assert(!out_of_range.ok());
    assert(out_of_range.status == chronosx::FrameStatus::InvalidFrameAddress);
}

void test_timing_wheel_ownership_handoff() {
    chronosx::FrameAllocator<8, 4096> allocator;
    chronosx::TimingWheel<8, 4> wheel;

    const auto allocation = allocator.allocate(chronosx::FrameOwner::DataPlane);
    assert(allocation.ok());

    assert(allocator.transition(allocation.frame,
                                chronosx::FrameOwner::DataPlane,
                                chronosx::FrameOwner::TimingWheel) == chronosx::FrameStatus::Ok);

    const chronosx::DelayedFrame delayed{
        .frame_address = allocation.frame.address,
        .frame_length = 512,
        .rule_id = 42,
        .release_tick = 0,
    };

    assert(wheel.schedule(delayed, 2) == chronosx::ScheduleStatus::Scheduled);
    assert(wheel.tick().empty());

    const auto expired = wheel.tick();
    assert(expired.size() == 1);
    assert(expired[0].frame_address == allocation.frame.address);

    const auto handle = allocator.handle_from_address(expired[0].frame_address);
    assert(handle.ok());
    assert(allocator.transition(handle.frame,
                                chronosx::FrameOwner::TimingWheel,
                                chronosx::FrameOwner::TxRing) == chronosx::FrameStatus::Ok);
    assert(allocator.transition(handle.frame,
                                chronosx::FrameOwner::TxRing,
                                chronosx::FrameOwner::CompletionRing) == chronosx::FrameStatus::Ok);
    assert(allocator.release(handle.frame,
                             chronosx::FrameOwner::CompletionRing) == chronosx::FrameStatus::Ok);
    assert(allocator.full());
}

}  // namespace

int main() {
    test_layout_and_constants();
    test_initial_allocation_order_and_counts();
    test_allocate_all_frames_are_unique();
    test_release_reuses_frame_lifo();
    test_double_free_is_rejected();
    test_invalid_frame_handle_is_rejected();
    test_owner_transition_state_machine();
    test_wrong_owner_transition_is_rejected();
    test_batch_allocate_and_release();
    test_partial_batch_allocation();
    test_address_to_handle_conversion();
    test_timing_wheel_ownership_handoff();

    std::cout << "frame allocator tests passed\n";
    return 0;
}
