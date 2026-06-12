# Design Decisions

This document records the major architectural decisions, their rationale, and trade-offs. Each section follows the Architecture Decision Record (ADR) format: **Context → Decision → Consequences**.

---

## ADR-001: Three-Plane Architecture (Lock-Free Data Plane)

### Context
Building a network chaos injection tool faces a fundamental tension:
- **Data plane** must process packets with **minimal, predictable latency** (critical for HFT)
- **Control plane** must be **responsive to user rule changes** (requires locks, allocations, I/O)
- **UI** must provide **real-time dashboards** (bursty rendering, can starve packet processing)

Mixing these concerns on one thread or using shared locks guarantees jitter on the data plane.

### Decision
**Separate into three independent threads on dedicated cores:**
1. **Data Plane** — Process packets lock-free (SPSC queues only)
2. **Control Plane** — Manage rules via epoll TCP server
3. **UI Plane** — Render dashboards via FTXUI

Planes communicate via lock-free queues (data ↔ control) and atomic snapshots (control → data).

### Consequences
✅ **Advantages:**
- **Deterministic latency** on data plane (no locks, allocations, or preemption)
- **UI hangs never stall packet processing** (independent threads)
- **Clear thread safety** (single producer/consumer per queue)
- **Testable in isolation** (mock queues for unit tests)

❌ **Trade-offs:**
- **Complexity:** Three threads instead of one; requires careful synchronization
- **Debugging:** Thread interactions harder to trace
- **CPU overhead:** Three cores required for full utilization

**Rationale:** Deterministic latency is non-negotiable for HFT systems. The complexity cost is acceptable.

---

## ADR-002: Lock-Free SPSC Queue Instead of Mutex-Protected Queue

### Context
Data plane and control plane must exchange statistics (packets seen, dropped, latency). Options:
1. **Mutex-protected queue** — Simple, standard (stdlib queue + mutex)
2. **Lock-free SPSC queue** — No contention, but single producer/consumer only
3. **Atomic variables only** — No ordering guarantees; error-prone
4. **Memory-mapped shared memory** — Over-engineered for inter-thread communication

### Decision
**Implement custom lock-free SPSC queue with `acquire/release` memory ordering.**

```cpp
template <typename T, std::size_t Capacity>
class SPSCQueue {
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        
        if (tail - head >= Capacity) return false;
        
        buffer_[tail & kIndexMask] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }
    
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        
        if (head == tail) return std::nullopt;
        
        T item = buffer_[head & kIndexMask];
        head_.store(head + 1, std::memory_order_release);
        return item;
    }
};
```

### Consequences
✅ **Advantages:**
- **Zero contention** between data and control planes on stats
- **Predictable latency** (no lock waits, no syscalls)
- **Cache-friendly** (head/tail on separate cache lines)
- **Memory-safe** (sized for fixed capacity, no dynamic allocation)

❌ **Trade-offs:**
- **Single producer/consumer only** — Can't have multiple data threads
- **Manual memory ordering** — Risk of subtle bugs if ordering wrong
- **Bounded capacity** — Must size queue ahead of time (acceptable: stats are low-volume)

**Rationale:** Data plane latency trumps flexibility. SPSC is the perfect fit because data plane is sole producer of stats, control plane is sole consumer.

---

## ADR-003: RCU (Read-Copy-Update) for Rule Publishing Instead of Global Mutex

### Context
Data plane must read the current set of rules for every packet. Control plane must update rules on command. Options:
1. **Global `std::mutex` over all rules** — Simple, but data plane blocks on every rule update
2. **Per-rule atomic<bool>** — No coordination; rules must be independent
3. **RCU (Read-Copy-Update)** — Readers never block; writers must build new snapshot offline
4. **Read-Write lock** — Readers block on writer; better than mutex but still contention

### Decision
**Use RCU with `atomic<shared_ptr<const RuleSnapshot>>`:**

```cpp
// Control plane: build new snapshot offline (no lock on data plane)
auto new_snapshot = std::make_shared<RuleSnapshot>(rules_);
config_.store(new_snapshot, std::memory_order_release);

// Data plane: never blocks, just loads
auto snapshot = config_.load(std::memory_order_acquire);
```

### Consequences
✅ **Advantages:**
- **Data plane never blocks** — No lock on critical path
- **Rule updates atomic** — No torn reads; snapshot always consistent
- **Writer builds offline** — No time-critical work on update thread
- **Natural for snapshots** — Rules are naturally immutable once published

❌ **Trade-offs:**
- **Memory overhead** — Old snapshots kept alive via `shared_ptr` until all readers finish
- **Complexity** — Must understand memory ordering semantics
- **Not suitable for frequent updates** — Each update requires building new snapshot (acceptable: rule changes are rare)

**Rationale:** RCU is the canonical solution for "frequent reads, infrequent writes" with determinism requirements. Used throughout the Linux kernel for this reason.

---

## ADR-004: AF_XDP (UMEM + Rings) Instead of DPDK or Raw Sockets

### Context
Chronos-X needs to intercept live network traffic at wire speed. Options:
1. **Raw sockets (AF_PACKET)** — Userspace only, copies packets, ~1 Mbps per core
2. **DPDK (Data Plane Development Kit)** — Excellent throughput, but requires extensive setup, massive dependencies
3. **AF_XDP (Address Family XDP)** — Linux 5.8+, zero-copy via eBPF, ~5-10 Mbps per core, minimal dependencies
4. **netmap** — Excellent but requires kernel patching

### Decision
**Use AF_XDP with UMEM frame allocator and kernel eBPF program.**

```
eBPF Program (kernel)          UMEM (userspace)
       │                              │
       ├─ Redirect packet ────→ Frame Ring (RX)
       │                              │
                                 Data plane reads batch,
                                 processes, TX ring
```

### Consequences
✅ **Advantages:**
- **Zero-copy** — Packets in shared UMEM, no data plane boundary crossing
- **Kernel-native** — Built into Linux 5.8+; no external kernel modules or DPDK
- **Minimal dependencies** — Just libbpf + libxdp (optional; can simulate)
- **Deterministic latency** — No packet copies; fixed frame allocation
- **Testable without hardware** — Simulation mode works in userspace

❌ **Trade-offs:**
- **Linux-only** — No Windows/macOS/BSD support
- **Root required** — CAP_NET_ADMIN for live interception (demo mode works unprivileged)
- **Single queue per instance** — Multi-core sharding requires multiple instances
- **NIC-dependent** — Requires NIC driver support for AF_XDP

**Rationale:** AF_XDP offers the best trade-off of zero-copy, determinism, and low complexity. We sacrifice multi-queue sharding (per-instance limitation) for simplicity and testability.

---

## ADR-005: Deterministic Chaos via FNV1a Hash Instead of Random Number Generator

### Context
Chaos injection needs to be **reproducible and deterministic** for testing and replay. The chaos decision (drop, delay, corrupt) must depend only on the packet and a seed, never on time or process state. Options:
1. **Random number generator (e.g., `std::mt19937`)** — Non-deterministic across runs
2. **Timestamp-based hashing** — Deterministic but varies by wall-clock time (breaks reproducibility)
3. **FNV1a hash of 5-tuple + seed** — Deterministic, fast, reproducible
4. **Cryptographic hash (SHA-256)** — Deterministic but overkill; slow

### Decision
**Hash the packet's flow 5-tuple (src IP, dst IP, src port, dst port, protocol) with seed via FNV1a:**

```cpp
uint64_t decision = fnv1a_hash(
    src_ip, dst_ip, src_port, dst_port, protocol, seed
);

bool should_drop = (decision % 10000) < drop_probability;
```

**Key invariant:** Same packet always gets the same fate, regardless of when or how many times it's processed.

### Consequences
✅ **Advantages:**
- **Reproducible** — Replay a trace with same seed = same chaos fate for each packet
- **Fast** — O(1) hash; no RNG state or locks
- **Testable** — Unit tests can verify determinism
- **No side effects** — Pure function of packet contents + seed
- **Cache-friendly** — No global RNG state contention

❌ **Trade-offs:**
- **Limited distribution** — Not cryptographically random (acceptable: chaos doesn't require cryptographic strength)
- **Seed matters** — Different seed = different chaos pattern (feature, not bug)
- **Predictable** — Observant attacker could reverse-engineer seed (acceptable: not a security tool)

**Rationale:** Determinism is critical for:
- **Reproducibility** — Running same test twice must chaos the same packets
- **Debugging** — Deterministic failures are easier to reproduce and fix
- **Proof** — Can prove "packet X with seed Y gets dropped" via hash alone

---

## ADR-006: Timing Wheel for O(1) Delay Scheduling Instead of Priority Queue

### Context
Packets may be delayed (not dropped immediately). Must track which packets are delayed and when they can be released. Options:
1. **Priority queue (std::priority_queue)** — O(log n) insert/extract, easy to understand
2. **Timing wheel (bitmask indexing)** — O(1) insert/extract, requires fixed delay bucket count
3. **Sorted list** — O(n) insert/extract, cache-unfriendly
4. **Calendar queue** — O(1) amortized, complex implementation

### Decision
**Implement timing wheel with power-of-two slots and bitmask indexing:**

```cpp
template <std::size_t NumSlots, std::size_t MaxFramesPerSlot>
class TimingWheel {
    std::array<std::vector<Frame>, NumSlots> slots_;  // Power of 2
    
    void insert(const Frame& frame, std::size_t delay) {
        const std::size_t slot = (current_slot_ + delay) & kSlotMask;
        slots_[slot].push_back(frame);
    }
    
    std::vector<Frame> expire() {
        std::vector<Frame> ready;
        ready.swap(slots_[current_slot_]);
        current_slot_ = (current_slot_ + 1) & kSlotMask;
        return ready;
    }
};
```

### Consequences
✅ **Advantages:**
- **O(1) insert and extract** — No sorting, no comparisons
- **Cache-friendly** — Linear scan of one slot per tick
- **Bounded memory** — Fixed slot count; predictable allocation
- **Matches packet batch semantics** — Process in batches; expire in batches

❌ **Trade-offs:**
- **Fixed slot count** — Must know max delay ahead of time (acceptable: bounded by frame buffer)
- **Delayed packets held in memory** — Can't be released earlier than slot allows (acceptable: precision is ~batch latency anyway)
- **Less flexible** — Priorit queue allows arbitrary delays; timing wheel rounds to nearest slot

**Rationale:** Data plane operates on batches; timing wheel matches this granularity. O(1) guarantee is essential for deterministic latency.

---

## ADR-007: CRC32 Validation on Control Protocol Instead of No Validation

### Context
Control plane receives TCP messages from remote clients. Must validate protocol integrity. Options:
1. **No validation** — Fast, but corrupted messages silently apply wrong rules
2. **CRC32** — Fast, catches accidental corruption
3. **HMAC-SHA256** — Strong, but slow (not needed: not a security tool, local-only network)
4. **TLV (Type-Length-Value)** — Extensible, but more complex

### Decision
**Every protocol message includes CRC32 checksum (32 bits) appended:**

```cpp
struct ProtocolMessage {
    uint16_t type;       // MSG_ADD_RULE, MSG_DELETE_RULE, etc.
    uint16_t length;     // Payload length
    Byte payload[...];   // Variable-length payload
    uint32_t crc32;      // Checksum of (type, length, payload)
};
```

### Consequences
✅ **Advantages:**
- **Catches bit flips** — Invalid rules are rejected, not silently applied
- **Fast** — CRC32 is O(n) in message size (small)
- **Standard** — Well-understood, widely used

❌ **Trade-offs:**
- **Not cryptographic** — Doesn't protect against intentional tampering
- **Overhead** — 4 bytes per message (negligible)

**Rationale:** Control plane is local-only (TCP:9090), so HMAC is overkill. CRC32 is the right balance of simplicity and protection.

---

## ADR-008: FTXUI for TUI Dashboard Instead of ncurses

### Context
UI plane must render real-time dashboards (throughput, latency, rules). Options:
1. **ncurses** — Standard, minimal, but low-level API
2. **FTXUI** — Modern C++, compositional, rich widgets, easier to maintain
3. **Dear ImGui** — Excellent but overkill for terminal UI
4. **Print to stdout** — Simple but non-interactive

### Decision
**Use FTXUI for composable, maintainable TUI:**

```cpp
auto ui = Container::Vertical({
    Text("Chronos-X Dashboard"),
    Separator(),
    Renderer([](){ 
        return text("Throughput: " + std::to_string(throughput) + " pps");
    }),
});

Screen::Loop(ui);
```

### Consequences
✅ **Advantages:**
- **C++ API** — Type-safe, composable widgets
- **Maintainable** — Easy to add new gauges/charts
- **Rich widgets** — Histograms, sparklines, progress bars built-in
- **Active project** — Good community, regular updates

❌ **Trade-offs:**
- **Dependency** — Need to build/install FTXUI
- **Headless terminals** — Requires interactive terminal (acceptable: demo mode has JSON output)

**Rationale:** FTXUI improves code maintainability without sacrificing performance. TUI thread is not on critical path.

---

## Summary: Design Principles

| Principle | Where Applied | Justification |
|-----------|---------------|----------------|
| **Determinism** | FNV1a hashing, fixed layouts | Reproducible testing, HFT predictability |
| **Lock-free hot path** | Data plane (SPSC only) | Sub-microsecond jitter requirement |
| **Zero-copy** | AF_XDP UMEM frames | Deterministic latency, throughput |
| **Single producer/consumer** | SPSC queues | No contention between planes |
| **RCU for config** | Rule publishing | Readers never block |
| **Fixed-size structures** | All packet types | Predictable allocation, no surprises |
| **Immutability** | Rule snapshots, packet types | Thread-safe without locks |
| **Clear ownership** | Frame states (FREE → POSTED → RX → TX → COMPLETED) | No double-free, no use-after-free |

---

## Future Considerations

### Multi-Queue Sharding
- **Current:** Single AF_XDP queue per instance
- **Future:** Multiple queues with RSS (Receive-Side Scaling)
- **Challenge:** Would require per-queue data plane threads + aggregation logic

### Distributed Rule Coordination
- **Current:** Single-node only
- **Future:** gRPC for rule sync across multiple instances
- **Challenge:** Maintaining determinism across machines (network delays)

### Kernel-Bypass for Full Control
- **Current:** Hybrid (kernel eBPF + userspace data plane)
- **Future:** Full userspace eBPF JIT + polling instead of interrupts
- **Challenge:** Trade-off between control and Linux integration
