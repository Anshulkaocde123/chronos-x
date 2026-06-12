# Testing

Chronos-X uses a layered testing strategy: **unit tests** verify correctness, **stress tests** prove thread safety, and **sanitizers** catch memory/concurrency bugs. This document explains what's tested, how, and why.

---

## Testing Pyramid

```
        ▲
       /│\
      / │ \  ⚙️ ASan / TSan / UBSan
     /  │  \  (Instrumentation: Memory Safety, Data Races, UB)
    /   │   \
   /    │    \  🔨 Stress Tests
  /     │     \ (Concurrency: million-packet runs, thread safety)
 /      │      \
/───────│───────\  🧪 Unit Tests
                  (Correctness: each component in isolation)
```

Each tier provides **different confidence:**
- **Unit tests** → Component correctness
- **Stress tests** → Thread safety under load
- **Sanitizers** → Memory safety, data races
- **Benchmarks** (not shown) → Performance characterization

---

## Unit Tests (Component Isolation)

### Philosophy
Every header file has a corresponding `test_*.cpp` that exercises its contract in isolation. Tests use **simple assert-based checking** (no GoogleTest framework) for clarity and minimal overhead.

### Test Files & Coverage

| File | Component | What's Tested | Scale |
|------|-----------|---------------|-------|
| `test_types_layout.cpp` | `types.hpp` | Struct sizes, alignment, padding | Static checks |
| `test_spsc_queue.cpp` | `spsc_queue.hpp` | Push/pop, wraparound, full queue, capacity | 1K-10K ops |
| `test_packet_parser.cpp` | `packet_parser.hpp` | Ethernet, IPv4, TCP parsing; malformed packets | 100 packets |
| `test_chaos_engine.cpp` | `chaos_engine.hpp` | Determinism (same input = same output), seeding | 10K hashes |
| `test_timing_wheel.cpp` | `timing_wheel.hpp` | Insert/expire, slot wraparound, ordering | 10K delays |
| `test_frame_allocator.cpp` | `frame_allocator.hpp` | State transitions, no double-frees, no leaks | 1K frames |
| `test_data_plane.cpp` | `data_plane.hpp` | Full batch processing: RX → parse → chaos → TX | 100 packets |
| `test_protocol.cpp` | `protocol.hpp` | Binary protocol parsing, CRC32 validation, invalid messages | 1K messages |
| `test_socket_manager.cpp` | `socket_manager.hpp` | Ring arithmetic, simulation mode without AF_XDP | 1K operations |
| `test_control_plane.cpp` | `control_plane.hpp` | TCP message parsing, rule management | 10 rules |
| `test_af_xdp_runtime.cpp` | `af_xdp_runtime.hpp` | UMEM setup, ring initialization (simulation) | 1K frames |
| `test_libxdp_socket.cpp` | `libxdp_socket.hpp` | Real AF_XDP backend (if `CHRONOSX_ENABLE_LIBXDP=ON`) | Requires NIC |

### Example: `test_spsc_queue.cpp`

```cpp
void test_wraparound_ordering() {
    chronosx::SPSCQueue<uint64_t, 8> queue;
    
    // Push/pop across multiple wraps (capacity is 8, so wrapping at 8, 16, 24, ...)
    for (uint64_t value = 0; value < 10'000; ++value) {
        assert(queue.try_push(value));
        
        const auto popped = queue.try_pop();
        assert(popped.has_value());
        assert(*popped == value);  // FIFO order maintained
    }
}

void test_concurrent_producer_consumer() {
    chronosx::SPSCQueue<chronosx::StatUpdate, 16> queue;
    
    std::thread producer([&]() {
        for (int i = 0; i < 10'000; ++i) {
            const chronosx::StatUpdate update{...};
            while (!queue.try_push(update)) { /* spin */ }
        }
    });
    
    std::thread consumer([&]() {
        for (int i = 0; i < 10'000; ++i) {
            std::optional<chronosx::StatUpdate> item;
            while (!(item = queue.try_pop())) { /* spin */ }
        }
    });
    
    producer.join();
    consumer.join();
}
```

---

## Stress Tests

### `test_stress.cpp`: The HFT Crucible

This test simulates **high-frequency trading workloads:** concurrent producers/consumers, massive batch sizes, RCU rule updates.

```cpp
void stress_test_concurrent_packets() {
    const size_t kIterations = 1'000'000;
    const size_t kBatchSize = 64;
    
    // Simulate data plane + control plane + UI
    std::thread data_plane_thread([&]() {
        for (size_t i = 0; i < kIterations; ++i) {
            auto batch = rx_queue.try_pop_batch(kBatchSize);
            for (const auto& packet : batch) {
                apply_chaos(packet);  // Read current rules (RCU)
                tx_queue.try_push(packet);
            }
        }
    });
    
    std::thread control_plane_thread([&]() {
        for (size_t i = 0; i < 100; ++i) {
            auto rules = build_new_rules();
            config.store(rules, memory_order_release);  // RCU swap
        }
    });
    
    std::thread ui_thread([&]() {
        for (size_t i = 0; i < 100; ++i) {
            auto stats = stats_queue.try_pop_batch(1000);
            render_dashboard(stats);
        }
    });
    
    // All threads must complete without deadlock, use-after-free, or data races
    data_plane_thread.join();
    control_plane_thread.join();
    ui_thread.join();
}
```

**What's tested:**
- No deadlocks under high contention
- No memory corruption (use-after-free, buffer overflows)
- RCU rule updates visible to data plane without stalls
- Queue wraparound under load

**Scale:** 1M packets, 100 rule updates, multiple threads.

---

## Sanitizers

### Build with Instrumentation

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCHRONOSX_ENABLE_SANITIZERS=ON
cmake --build .
ctest --output-on-failure
```

### What Each Sanitizer Catches

| Sanitizer | What It Detects | Example |
|-----------|-----------------|---------|
| **ASan (AddressSanitizer)** | Out-of-bounds access, use-after-free, double-free | Buffer overflow in packet parser |
| **UBSan (UndefinedBehaviorSanitizer)** | Signed overflow, misaligned access, invalid shifts | `size_t - 1` when size is 0 |
| **TSan (ThreadSanitizer)** | Data races, mutex misuse | Two threads writing same variable without locks |

### Running with Sanitizers

```bash
# Build with all sanitizers
./scripts/build.sh --sanitizers

# Run tests
ctest --output-on-failure

# Expected output: "0 data races, 0 memory errors"
```

---

## Benchmark: `bench_data_plane.cpp`

Measures **throughput and latency** on the hot path under realistic conditions.

### What's Measured

```cpp
int main() {
    const size_t kFrameCount = chronosx::kDataPlaneBatchSize;
    const size_t kIterations = 1'000'000;
    
    // Build packets in UMEM
    std::array<Byte, kFrameCount * kFrameStride> umem{};
    std::array<PacketDescriptor, kFrameCount> rx{};
    
    for (size_t i = 0; i < kFrameCount; ++i) {
        const size_t length = build_min_tcp_frame(umem.data() + i * kFrameStride, 9000);
        rx[i] = {.frame_address = i * kFrameStride, .frame_length = length};
    }
    
    // Benchmark: process kIterations batches
    auto start = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < kIterations; ++i) {
        std::array<ProcessedFrame, kFrameCount> tx;
        data_plane.process_batch(rx.data(), rx.size(), tx.data());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    
    double elapsed_us = (end - start).count() / 1000.0;
    double throughput_mpps = (kIterations * kFrameCount) / elapsed_us;
    double throughput_gbps = throughput_mpps * 64 * 8 / 1000.0;  // 64-byte min frame
    
    printf("throughput_mpps = %.2f\n", throughput_mpps);
    printf("throughput_gbps = %.2f\n", throughput_gbps);
}
```

### Expected Results
```
$ ./build/bench_data_plane
throughput_mpps = 6.84
throughput_gbps = 3.50
```

*Note: Exact numbers depend on CPU, NIC, and kernel version. On modern Intel CPUs with AF_XDP, expect 5-10 Mbps per core.*

### How to Run
```bash
./build/bench_data_plane [iterations]  # Default: 1,000,000
```

---

## Testing Strategy Summary

| Test Type | What It Proves | Scale | Time |
|-----------|----------------|-------|------|
| **Unit** | Each component works correctly | 1K-10K ops | <0.1s each |
| **Stress** | Concurrent access is safe, no deadlocks | 1M ops, multiple threads | ~1s |
| **ASan** | Memory safety (full test suite under instrumentation) | All tests | ~5s |
| **TSan** | No data races (full test suite under instrumentation) | All tests | ~10s |
| **UBSan** | No undefined behavior | All tests | ~5s |
| **Perf** | Throughput/latency characterized | 1M packets | ~1s |

---

## What's NOT Tested (Yet)

### Live AF_XDP on Hardware
- **Reason:** Requires real NIC + root; can't run in CI/CD
- **Workaround:** `test_af_xdp_runtime.cpp` tests ring arithmetic in simulation
- **Manual validation:** Run `--enable-libxdp` build on lab machine with real NIC

### Real Network Topology
- **Reason:** Would require setting up test bridge + traffic generation
- **Workaround:** Simulation mode (`./chronosx`) demonstrates end-to-end pipeline
- **Lab validation:** Deploy to real network and measure actual jitter

### IPv6 Packet Processing
- **Reason:** IPv4 covers core chaos logic; IPv6 is extension
- **Status:** Future work (see LIMITATIONS.md)

### Distributed Rule Coordination
- **Reason:** Single-node only by design
- **Status:** Future work

---

## Continuous Integration

GitHub Actions runs on every push:

```yaml
name: CI

on: [push, pull_request]

jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build (Debug)
        run: ./scripts/build.sh

      - name: Run Tests
        run: ./scripts/run_tests.sh

      - name: Build with Sanitizers
        run: ./scripts/build.sh --sanitizers

      - name: Run Tests (ASan/UBSan/TSan)
        run: ./scripts/run_tests.sh

      - name: Run Benchmarks
        run: ./scripts/run_benchmark.sh
```

---

## Adding New Tests

### Checklist for New Components

1. **Create `tests/test_<component>.cpp`**
   ```cpp
   #include "chronosx/<component>.hpp"
   
   namespace {
   
   void test_basic_functionality() {
       // Test the happy path
       assert(component_works_as_expected);
   }
   
   void test_edge_cases() {
       // Test boundaries, empty inputs, etc.
   }
   
   }  // namespace
   
   int main() {
       test_basic_functionality();
       test_edge_cases();
       std::cout << "All tests passed!" << std::endl;
       return 0;
   }
   ```

2. **Add to `CMakeLists.txt`**
   ```cmake
   add_executable(test_<component> tests/test_<component>.cpp)
   add_test(NAME <component> COMMAND test_<component>)
   ```

3. **Run locally**
   ```bash
   ./scripts/build.sh
   ./scripts/run_tests.sh
   ```

4. **Verify with sanitizers**
   ```bash
   ./scripts/build.sh --sanitizers
   ./scripts/run_tests.sh
   ```

---

## References

- **AddressSanitizer:** [github.com/google/sanitizers/wiki/AddressSanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizer)
- **ThreadSanitizer:** [github.com/google/sanitizers/wiki/ThreadSanitizer](https://github.com/google/sanitizers/wiki/ThreadSanitizer)
- **Testing at Google:** [abseil.io/resources/swe-book/html/ch11.html](https://abseil.io/resources/swe-book/html/ch11.html)
