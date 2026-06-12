#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>

#include "chronosx/socket_manager.hpp"
#include "chronosx/types.hpp"

namespace {

void test_umem_config_validation() {
    const chronosx::UmemConfig valid{};
    assert(valid.valid());
    assert(valid.total_bytes() == 4096ULL * 4096);

    const chronosx::UmemConfig bad_frame_size{.frame_size = 1024};
    assert(!bad_frame_size.valid());

    const chronosx::UmemConfig bad_zero_count{.frame_size = 4096, .frame_count = 0};
    assert(!bad_zero_count.valid());
}

void test_socket_config_validation() {
    const chronosx::SocketConfig valid{};
    assert(valid.valid());

    const chronosx::SocketConfig bad_rx{.rx_ring_size = 100};
    assert(!bad_rx.valid());
}

void test_status_names() {
    assert(chronosx::socket_status_name(chronosx::SocketStatus::Ok)[0] == 'O');
    assert(chronosx::socket_status_name(chronosx::SocketStatus::InvalidConfig)[0] == 'I');
    assert(chronosx::socket_status_name(chronosx::SocketStatus::KickFailed)[0] == 'K');
}

void test_simulated_ring_basic() {
    chronosx::SimulatedRing<std::uint64_t, 8> ring;

    assert(ring.empty());
    assert(!ring.full());
    assert(ring.available_consume() == 0);
    assert(ring.available_produce() == 8);

    const std::array<std::uint64_t, 3> items{10, 20, 30};
    const std::size_t produced = ring.produce(std::span<const std::uint64_t>{items});
    assert(produced == 3);
    assert(ring.available_consume() == 3);
    assert(ring.available_produce() == 5);

    std::array<std::uint64_t, 3> output{};
    const std::size_t consumed = ring.consume(std::span<std::uint64_t>{output});
    assert(consumed == 3);
    assert(output[0] == 10);
    assert(output[1] == 20);
    assert(output[2] == 30);
    assert(ring.empty());
}

void test_simulated_ring_full() {
    chronosx::SimulatedRing<std::uint64_t, 4> ring;

    const std::array<std::uint64_t, 4> items{1, 2, 3, 4};
    assert(ring.produce(std::span<const std::uint64_t>{items}) == 4);
    assert(ring.full());

    const std::array<std::uint64_t, 1> overflow{5};
    assert(ring.produce(std::span<const std::uint64_t>{overflow}) == 0);
}

void test_simulated_xsk_partial_statuses() {
    chronosx::SimulatedXskSocket<2, 2, 2, 2> socket;

    const std::array<std::uint64_t, 3> fill_addrs{0, 4096, 8192};
    const auto fill = socket.refill(std::span<const std::uint64_t>{fill_addrs});
    assert(fill.count == 2);
    assert(fill.status == chronosx::SocketStatus::RingFull);

    std::array<std::uint64_t, 3> drained{};
    const auto completions = socket.drain_completions(std::span<std::uint64_t>{drained});
    assert(completions.count == 0);
    assert(completions.status == chronosx::SocketStatus::RingEmpty);

    const std::array<chronosx::TxDescriptor, 3> tx_descs{{
        {.address = 0, .length = 64, .options = 0},
        {.address = 4096, .length = 64, .options = 0},
        {.address = 8192, .length = 64, .options = 0},
    }};
    const auto tx = socket.submit_tx(std::span<const chronosx::TxDescriptor>{tx_descs});
    assert(tx.count == 2);
    assert(tx.status == chronosx::SocketStatus::RingFull);
}

void test_simulated_ring_wraparound() {
    chronosx::SimulatedRing<std::uint64_t, 4> ring;

    for (std::uint64_t cycle = 0; cycle < 100; ++cycle) {
        const std::array<std::uint64_t, 1> item{cycle};
        assert(ring.produce(std::span<const std::uint64_t>{item}) == 1);

        std::array<std::uint64_t, 1> output{};
        assert(ring.consume(std::span<std::uint64_t>{output}) == 1);
        assert(output[0] == cycle);
    }
}

void test_simulated_xsk_fill_and_rx() {
    chronosx::SimulatedXskSocket<16, 16, 16, 16> socket;

    const std::array<std::uint64_t, 3> addresses{0, 4096, 8192};
    const auto fill_result = socket.refill(std::span<const std::uint64_t>{addresses});
    assert(fill_result.count == 3);
    assert(fill_result.status == chronosx::SocketStatus::Ok);
    assert(socket.fill_ring_pending() == 3);

    std::array<std::uint64_t, 4> drained{};
    const std::size_t fill_consumed = socket.consume_fill(std::span<std::uint64_t>{drained});
    assert(fill_consumed == 3);
    assert(drained[0] == 0);
    assert(drained[1] == 4096);
    assert(drained[2] == 8192);

    const std::array<chronosx::RxDescriptor, 2> rx_descs{
        chronosx::RxDescriptor{.address = 0, .length = 64, .options = 0},
        chronosx::RxDescriptor{.address = 4096, .length = 128, .options = 0},
    };
    assert(socket.inject_rx(std::span<const chronosx::RxDescriptor>{rx_descs}) == 2);
    assert(socket.rx_ring_pending() == 2);

    std::array<chronosx::RxDescriptor, 4> rx_output{};
    const auto rx_result = socket.consume_rx(std::span<chronosx::RxDescriptor>{rx_output});
    assert(rx_result.count == 2);
    assert(rx_output[0].address == 0);
    assert(rx_output[0].length == 64);
    assert(rx_output[1].address == 4096);
    assert(rx_output[1].length == 128);
}

void test_simulated_xsk_tx_and_completion() {
    chronosx::SimulatedXskSocket<16, 16, 16, 16> socket;

    const std::array<chronosx::TxDescriptor, 2> tx_descs{
        chronosx::TxDescriptor{.address = 0, .length = 64, .options = 0},
        chronosx::TxDescriptor{.address = 4096, .length = 128, .options = 0},
    };
    const auto tx_result = socket.submit_tx(std::span<const chronosx::TxDescriptor>{tx_descs});
    assert(tx_result.count == 2);
    assert(socket.tx_ring_pending() == 2);

    assert(socket.kick_tx() == chronosx::SocketStatus::Ok);
    assert(socket.tx_kick_count() == 1);

    const std::array<std::uint64_t, 2> completed{0, 4096};
    assert(socket.complete_tx(std::span<const std::uint64_t>{completed}) == 2);
    assert(socket.completion_ring_pending() == 2);

    std::array<std::uint64_t, 4> comp_output{};
    const auto comp_result = socket.drain_completions(std::span<std::uint64_t>{comp_output});
    assert(comp_result.count == 2);
    assert(comp_output[0] == 0);
    assert(comp_output[1] == 4096);
}

void test_full_rx_tx_cycle() {
    chronosx::SimulatedXskSocket<32, 32, 32, 32> socket;

    std::array<std::uint64_t, 4> fill_addrs{0, 4096, 8192, 12288};
    (void)socket.refill(std::span<const std::uint64_t>{fill_addrs});

    const std::array<chronosx::RxDescriptor, 2> rx_descs{
        chronosx::RxDescriptor{.address = 0, .length = 64, .options = 0},
        chronosx::RxDescriptor{.address = 4096, .length = 128, .options = 0},
    };
    (void)socket.inject_rx(std::span<const chronosx::RxDescriptor>{rx_descs});

    std::array<chronosx::RxDescriptor, 4> rx_output{};
    const auto rx_result = socket.consume_rx(std::span<chronosx::RxDescriptor>{rx_output});
    assert(rx_result.count == 2);

    std::array<chronosx::TxDescriptor, 2> tx_descs{};
    for (std::size_t index = 0; index < rx_result.count; ++index) {
        tx_descs[index] = chronosx::TxDescriptor{
            .address = rx_output[index].address,
            .length = rx_output[index].length,
            .options = 0,
        };
    }

    const auto tx_result = socket.submit_tx(std::span<const chronosx::TxDescriptor>{tx_descs.data(), rx_result.count});
    assert(tx_result.count == 2);
    (void)socket.kick_tx();

    const std::array<std::uint64_t, 2> completed{0, 4096};
    (void)socket.complete_tx(std::span<const std::uint64_t>{completed});

    std::array<std::uint64_t, 4> comp_out{};
    const auto comp = socket.drain_completions(std::span<std::uint64_t>{comp_out});
    assert(comp.count == 2);

    (void)socket.refill(std::span<const std::uint64_t>{comp_out.data(), comp.count});
    assert(socket.fill_ring_pending() == 4 + 2);
}

}  // namespace

int main() {
    test_umem_config_validation();
    test_socket_config_validation();
    test_status_names();
    test_simulated_ring_basic();
    test_simulated_ring_full();
    test_simulated_xsk_partial_statuses();
    test_simulated_ring_wraparound();
    test_simulated_xsk_fill_and_rx();
    test_simulated_xsk_tx_and_completion();
    test_full_rx_tx_cycle();

    std::cout << "all socket manager tests passed\n";
    return 0;
}
