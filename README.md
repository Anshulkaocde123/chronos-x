# Chronos-X

**High-Performance Network Chaos Engineering Tool**

A C++20 system for injecting deterministic chaos into live network traffic at wire speed. Uses AF_XDP for zero-copy packet processing and a lock-free three-plane architecture designed for HFT-level reliability.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Chronos-X                              │
│                                                             │
│  ┌──────────────┐  ┌─────────────────┐  ┌───────────────┐  │
│  │  Data Plane   │  │  Control Plane   │  │   UI Plane    │  │
│  │  (SCHED_FIFO) │  │  (epoll TCP)     │  │  (TUI)        │  │
│  │               │  │                  │  │               │  │
│  │  AF_XDP RX    │  │  Binary protocol │  │  Throughput   │  │
│  │  → Parse      │  │  Rule management │  │  Latency hist │  │
│  │  → Chaos      │  │  Stats export    │  │  Sparkline    │  │
│  │  → TX         │  │  atomic<sptr>    │  │  SPSC stats   │  │
│  └──────┬───────┘  └────────┬─────────┘  └───────┬───────┘  │
│         │   SPSC Queue      │  RCU swap           │          │
│         └───────────────────┴──────────────────────┘          │
└─────────────────────────────────────────────────────────────┘
```

### Why three planes?
- **Isolation**: Packet processing latency must be deterministic. Config and visualization are bursty. Mixing them injects jitter.
- **Failure containment**: UI hang ≠ data plane stall. The SPSC queue is lock-free and bounded.
- **CPU pinning**: Data plane runs on an isolated core with `SCHED_FIFO`.

## Components

| Module | Header | Purpose | Key Invariant |
|--------|--------|---------|---------------|
| Types | `types.hpp` | Vocabulary types, `StatUpdate` | 48-byte fixed layout |
| SPSC Queue | `spsc_queue.hpp` | Lock-free producer/consumer | `release/acquire` ordering, `alignas(64)` |
| Packet Parser | `packet_parser.hpp` | Zero-copy L2/L3/L4 parser | Length-checked before every cast |
| Chaos Engine | `chaos_engine.hpp` | Deterministic FNV1a decisions | Same seed + same packet = same fate |
| Timing Wheel | `timing_wheel.hpp` | O(1) delay scheduling | Power-of-two slots, bitmask indexing |
| Frame Allocator | `frame_allocator.hpp` | UMEM frame lifecycle | Each frame in exactly ONE state |
| Data Plane | `data_plane.hpp` | Batch processing pipeline | No allocations on hot path |
| Protocol | `protocol.hpp` | Binary control protocol + CRC32 | Partial-read tolerant, CRC-validated |
| Socket Manager | `socket_manager.hpp` | AF_XDP abstraction + simulation | Ring arithmetic testable in userspace |
| Control Plane | `control_plane.hpp` | epoll TCP + RCU rule publish | No locks on data-plane read path |
| TUI | `tui.hpp` | Dashboard state + text renderer | O(1) histogram recording |

## Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
ctest --output-on-failure
```

**Requirements**: GCC 13+ (C++20), CMake 3.20+, Linux.

### Build with sanitizers

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCHRONOSX_ENABLE_SANITIZERS=ON
cmake --build .
ctest --output-on-failure
```

## Quick Start

### Demo mode (no root / no AF_XDP required)
```bash
./build/chronosx
```
Runs a complete end-to-end pipeline: installs chaos rules, processes test packets, renders the text dashboard.

### Control plane server
```bash
./build/chronosx --server 9090
```

### Python control client
```bash
python3 tools/chronos_client.py ping
python3 tools/chronos_client.py add-rule --port 8080 --action drop --prob 10000
python3 tools/chronos_client.py list-rules
python3 tools/chronos_client.py stats
python3 tools/chronos_client.py clear-rules
```

## Design Invariants

| Invariant | Why |
|-----------|-----|
| **Determinism** | FNV1a hash of flow 5-tuple + seed. Same input = same outcome. Never includes timestamps or PIDs in hash. |
| **Lock-Free Hot Path** | Data plane uses SPSC queues and atomic loads only. No mutexes, no syscalls on the packet path. |
| **Zero-Copy** | AF_XDP UMEM frames managed via `FrameAllocator` with strict ownership transitions (FREE → POSTED → RECEIVED → TX → COMPLETED). |
| **RCU Config** | `atomic<shared_ptr<const RuleSnapshot>>`. Readers never block. Writers build new snapshot, swap atomically. |
| **Bounded Memory** | All data structures are fixed-size. No heap allocation on the hot path. |

## Tests

```
$ ctest --output-on-failure
 1/11 spsc_queue         Passed  0.14s
 2/11 types_layout       Passed  0.00s
 3/11 packet_parser      Passed  0.00s
 4/11 chaos_engine       Passed  0.00s
 5/11 timing_wheel       Passed  0.00s
 6/11 frame_allocator    Passed  0.00s
 7/11 data_plane         Passed  0.00s
 8/11 protocol           Passed  0.00s
 9/11 socket_manager     Passed  0.00s
10/11 control_plane      Passed  0.00s
11/11 stress             Passed  1.05s   ← HFT-level: 10M SPSC, concurrent RCU, 640K-pkt batch

100% tests passed, 0 tests failed out of 11
```

## Benchmark

```
$ ./build/bench_data_plane 10000
throughput_mpps = 6.84
throughput_gbps = 3.50
```

## Project Structure

```
Chronos-X/
├── include/chronosx/          # Header-only library (11 modules)
│   ├── types.hpp              # Shared vocabulary types
│   ├── spsc_queue.hpp         # Lock-free SPSC ring
│   ├── packet_parser.hpp      # Zero-copy L2/L3/L4 parser
│   ├── chaos_engine.hpp       # Deterministic chaos decisions
│   ├── timing_wheel.hpp       # O(1) delay scheduler
│   ├── frame_allocator.hpp    # UMEM frame lifecycle manager
│   ├── data_plane.hpp         # Batch processing pipeline
│   ├── protocol.hpp           # Binary control protocol + CRC32
│   ├── socket_manager.hpp     # AF_XDP socket abstraction
│   ├── control_plane.hpp      # Epoll TCP server + RCU rules
│   └── tui.hpp                # Dashboard histogram/sparkline/renderer
├── src/main.cpp               # Three-plane integration demo
├── tests/                     # 11 test suites (assert-based, no GoogleTest)
│   ├── test_spsc_queue.cpp
│   ├── test_types_layout.cpp
│   ├── test_packet_parser.cpp
│   ├── test_chaos_engine.cpp
│   ├── test_timing_wheel.cpp
│   ├── test_frame_allocator.cpp
│   ├── test_data_plane.cpp
│   ├── test_protocol.cpp
│   ├── test_socket_manager.cpp
│   ├── test_control_plane.cpp
│   └── test_stress.cpp        # HFT stress: 10M SPSC, RCU, batch storm
├── benchmarks/                # Data plane throughput benchmark
├── tools/chronos_client.py    # Python control client
├── bpf/                       # XDP BPF program (production use)
├── CMakeLists.txt             # Build system
├── .clang-format              # Code style
└── .gitignore
```

## Known Limitations

| Limitation | Severity | Notes |
|------------|----------|-------|
| Single NIC queue (queue 0) | Medium | Multi-queue + RSS is future work |
| IPv4 only | Medium | IPv6 parsing not implemented |
| No authentication on control port | Medium | TCP:9090 plaintext, local-only |
| No rule persistence | Low | Rules live in memory, restart loses them |
| No distributed coordination | By design | Single-node tool |

## License

Academic / Research project.
