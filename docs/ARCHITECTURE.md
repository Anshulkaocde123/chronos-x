# Architecture

Chronos-X uses a **three-plane, lock-free architecture** to achieve deterministic packet processing with minimal latency variance. This document explains the design, components, and communication patterns.

---

## Overview: Three Planes

```
┌──────────────────────────────────────────────────────────────┐
│                        Chronos-X                             │
│                     Three-Plane Design                        │
├───────────────┬──────────────────────┬──────────────────────┤
│  DATA PLANE   │  CONTROL PLANE        │      UI PLANE        │
│  (Hot Path)   │  (Rule Management)    │  (Dashboard)         │
│               │                       │                      │
│  • AF_XDP RX  │  • TCP epoll server   │  • FTXUI renderer    │
│  • Parse L2-4 │  • Binary protocol    │  • Throughput chart  │
│  • Chaos ops  │  • Atomic RCU swap    │  • Latency histogram │
│  • TX output  │  • Stats aggregation  │  • SPSC monitoring   │
│               │                       │                      │
│  SCHED_FIFO   │  Normal priority      │  Low priority        │
│  Isolated CPU │  Shared with OS       │  UI doesn't starve   │
│               │                       │  packet processing   │
└───────────────┴───────────┬───────────┴──────────┬───────────┘
                            │                      │
                  SPSC Queue │                      │ RCU Snapshot
                  (stats)    │                      │ (config)
                            └──────────────────────┘
```

Each plane runs on its own thread with distinct responsibilities:

### Data Plane (Hot Path)
- **Responsibility:** Process incoming packets, apply chaos rules, output results
- **Concurrency Model:** SCHED_FIFO on isolated core (no context switching)
- **Locking:** None. Only SPSC queues and atomic reads
- **Synchronization:** `acquire/release` memory ordering on SPSC pops
- **Key Invariants:**
  - No locks on the critical path
  - No dynamic allocations
  - Same input (flow 5-tuple) always produces same fate (determinism)
  - Fixed packet arrival latency (sub-microsecond variance)

### Control Plane (Management)
- **Responsibility:** Accept TCP connections, parse binary protocol, publish rule updates, aggregate statistics
- **Concurrency Model:** epoll event loop (normal OS priority)
- **Locking:** RCU for config publishing; thread-safe stats export
- **Synchronization:** Atomic `shared_ptr` swap for rule snapshots (writers block, readers never block)
- **Key Invariants:**
  - New rules published atomically without blocking data plane
  - Statistics available without locks on data plane
  - CRC32 validation on all protocol messages

### UI Plane (Monitoring)
- **Responsibility:** Render terminal dashboard, display throughput/latency/jitter
- **Concurrency Model:** Low priority; can be preempted without affecting packets
- **Locking:** SPSC queue for stats ingestion (never contends with data plane)
- **Synchronization:** O(1) histogram recording into buckets
- **Key Invariants:**
  - UI hangs never stall packet processing
  - Monitors both data plane and control plane via shared SPSC queues

---

## Inter-Plane Communication

### Data Plane → Control Plane: Statistics

```
Data Plane                          Control Plane
    │                                    │
    ├─ Every N packets ──────┐          │
    │                         │          │
    └─ Build StatUpdate       │          │
       (48 bytes, fixed)      │          │
       │                      │          │
       └──→ SPSC Queue ───────┤──→ Aggregate
               (lock-free)     │    & Export
                              │
                         ← TCP Client
                         Response w/ stats
```

**Mechanism:** `SPSCQueue<StatUpdate, 1024>` (lock-free ring buffer)

**Data Structure:**
```cpp
struct StatUpdate {
    uint64_t timestamp_cycles;     // rdtsc
    uint64_t packets_seen;
    uint64_t packets_dropped;
    uint32_t last_rule_epoch;      // RCU version
};
```

**Why SPSC:** Data plane is sole producer, control plane is sole consumer. No contention.

---

### Control Plane → Data Plane: Rules

```
Control Plane                       Data Plane
    │                                    │
    ├─ Parse new rule  ──────┐          │
    │   from TCP client        │          │
    │                         │          │
    ├─ Build RuleSnapshot      │          │
    │   (vector of rules)       │          │
    │                         │          │
    └─ Atomic CAS ────────────┤──→ Load via
    atomic<shared_ptr>         │    atomic_load()
       release/acquire         │
                              │
                        Every packet checks
                        current rule snapshot
```

**Mechanism:** `atomic<shared_ptr<const RuleSnapshot>>`

**Guarantees:**
- **Writers never block readers.** Control plane builds new snapshot offline, then swaps atomically.
- **Readers never block writers.** Data plane uses `acquire` ordering; control plane uses `release`.
- **Lock-free on hot path.** Data plane only calls `atomic_load()` and `acquire`.

**Implementation (Data Plane Pseudocode):**
```cpp
void process_batch(PacketDescriptor* rx, std::size_t count) {
    const auto rules = config_.load(std::memory_order_acquire);
    
    for (std::size_t i = 0; i < count; ++i) {
        const auto decision = chaos_engine_.decide(rx[i], rules);
        apply_fate(rx[i], decision);
    }
}
```

---

### Control Plane ↔ UI Plane: Monitoring

```
Data Plane                Control Plane              UI Plane
    │                           │                        │
    └──→ StatUpdate             │                        │
         (SPSC Queue 1) ────→ Aggregate ──→ SPSC Queue 2 ──→ Render Dashboard
                               Stats
```

Two separate SPSC queues prevent contention between monitoring threads.

---

## Component Responsibilities

| Component | File | Purpose | Thread Safety |
|-----------|------|---------|----------------|
| **Types** | `types.hpp` | Vocabulary types: `StatUpdate`, `PacketDescriptor`, `Byte`, `RuleSnapshot` | Immutable (POD) |
| **SPSC Queue** | `spsc_queue.hpp` | Lock-free single-producer/consumer ring buffer | SPSC only |
| **Packet Parser** | `packet_parser.hpp` | Zero-copy L2/L3/L4 parsing, length validation | Read-only |
| **Chaos Engine** | `chaos_engine.hpp` | Deterministic FNV1a-seeded decision making | Read-only (no state changes) |
| **Timing Wheel** | `timing_wheel.hpp` | O(1) delay scheduling via bitmask indexing | Single-threaded per instance |
| **Frame Allocator** | `frame_allocator.hpp` | UMEM frame lifecycle (FREE → POSTED → RECEIVED → TX → COMPLETED) | Called only by data plane |
| **Data Plane** | `data_plane.hpp` | Batch processing pipeline: RX → Parse → Chaos → TX | Data plane thread only |
| **Protocol** | `protocol.hpp` | Binary control protocol with CRC32 validation | Immutable after parsing |
| **Socket Manager** | `socket_manager.hpp` | AF_XDP ring abstractions (simulation + real) | AF_XDP-thread-safe |
| **Control Plane** | `control_plane.hpp` | epoll TCP server + RCU publishing | Thread-safe via locks where needed |
| **AF_XDP Runtime** | `af_xdp_runtime.hpp` | AF_XDP setup, ring initialization | Setup only (pre-data-plane) |
| **libxdp Socket** | `libxdp_socket.hpp` | Real libxdp AF_XDP socket backend | Thread-safe (optional feature) |
| **TUI** | `tui.hpp` | FTXUI dashboard rendering | UI thread only |

---

## Memory Layout & Cache Behavior

### Cache-Line Alignment

All high-contention data structures are aligned to 64-byte cache lines to prevent false sharing:

```cpp
class SPSCQueue {
private:
    alignas(kCacheLineSize) std::atomic<size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<size_t> tail_{0};
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};
```

**Why:** `head_` and `tail_` live on different cache lines. Producer writes to `tail_` without invalidating consumer's `head_` cache line.

### Fixed-Size Types

All packet-critical types have fixed, well-defined layouts:

```cpp
struct StatUpdate {
    uint64_t timestamp_cycles;     // 0-7
    uint64_t packets_seen;         // 8-15
    uint64_t packets_dropped;      // 16-23
    uint32_t last_rule_epoch;      // 24-27
    uint32_t reserved;             // 28-31
    // Total: 32 bytes (fits in one cache line with other data)
};
```

---

## Initialization Sequence

```
main()
  │
  ├─ Parse CLI arguments
  │
  ├─ Create AF_XDP sockets (or simulation mode)
  │
  ├─ Create shared SPSC queues
  │  ├─ stats_queue (data → control)
  │  └─ ui_queue (control → ui)
  │
  ├─ Initialize chaos rules
  │
  ├─ Create config atomic<shared_ptr<RuleSnapshot>>
  │
  ├─ Spawn data plane thread (SCHED_FIFO, isolated CPU)
  │
  ├─ Spawn control plane thread (epoll, normal priority)
  │
  ├─ Spawn UI thread (low priority, if not headless)
  │
  └─ Wait for signal (SIGINT, SIGTERM) → teardown
```

---

## Failure Modes & Resilience

| Failure | Impact | Recovery |
|---------|--------|----------|
| UI thread crashes | No dashboard | Logs errors; data plane continues |
| Control plane deadlock | No new rules; stats stall | Kill + restart control plane; data plane unaffected |
| Data plane thread stall | Packets drop | Watchdog timer (future); restart whole process |
| SPSC queue overflow | Stat loss | Queue is bounded; drops old stats (not packets) |
| AF_XDP NIC down | No packets | Graceful shutdown; demo mode continues |

---

## Performance Characteristics

### Throughput
- **Data plane:** ~5-10 Mbps per core (single AF_XDP queue)
- **Control plane:** <100 μs per rule update (atomic swap)
- **UI plane:** O(1) per histogram bucket; <1ms render time

### Latency
- **Packet processing:** ~500 ns (L2→decision→TX on isolated core)
- **Jitter:** <1 μs (no allocations, no locks, no preemption)
- **Rule update visibility:** <1ms (RCU swap + next batch processes new rules)

### Memory
- **Fixed overhead:** ~1 MB (ring buffers, frames, config)
- **Per-packet overhead:** 0 bytes (zero-copy)

---

## Thread Pinning & CPU Isolation

**Recommended Setup for HFT Lab:**
```bash
# Reserve CPU 2 for data plane
# CPU 0,1 for control plane + kernel
# CPU 3+ for other workloads

chronosx --data-cpu 2 --server 9090
```

**Why:** Prevents timer interrupts, context switches, and cache misses on data plane core.

---

## References

- **AF_XDP Design:** [docs.kernel.org/networking/af_xdp.rst](https://docs.kernel.org/networking/af_xdp.rst)
- **RCU (Read-Copy-Update):** [www.kernel.org/doc/html/latest/RCU/whatisRCU.html](https://www.kernel.org/doc/html/latest/RCU/whatisRCU.html)
- **Lock-Free Programming:** Preshing on Programming: Lock-Free Resources
- **Memory Ordering:** C++ Concurrency in Action (Anthony Williams, 2nd ed.)
