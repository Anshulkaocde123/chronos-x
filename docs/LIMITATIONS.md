# Known Limitations & Roadmap

Chronos-X is a **portfolio project focused on depth over breadth.** This document lists known limitations, explains why they exist, and suggests paths forward.

---

## By Category

### Architecture & Scaling

#### ❌ Single AF_XDP Queue (No Multi-Queue Sharding)

**What it means:** Chronos-X processes packets from one AF_XDP RX queue per instance. Multi-core throughput requires running multiple instances.

**Impact:** Throughput capped at ~5-10 Mbps per core per instance. For 40 Gbps NIC, would need 4+ instances.

**Why:** Multi-queue sharding requires:
- One data plane thread per queue
- SPSC queues between queues and control plane (combinatorial explosion)
- Load balancing logic to distribute rules across queues consistently
- More complex failure handling

**Workaround:** Run multiple instances with RSS (Receive-Side Scaling) to distribute traffic by flow hash.

**Next Steps:**
- [ ] Implement multi-queue architecture with flow-affinity sharding
- [ ] Add per-queue data plane threads with shared control plane
- [ ] Benchmark 40 Gbps NIC with 4-core sharding

---

#### ❌ IPv4 Only (No IPv6)

**What it means:** Packet parser only handles IPv4 headers. IPv6 packets are dropped or parsed incorrectly.

**Impact:** Cannot inject chaos into IPv6 traffic.

**Why:** IPv4 covers the core chaos logic. IPv6 is an extension with same semantics:
- L4 parsing logic (TCP/UDP) is identical
- Packet header parsing is similar
- Chaos decisions (drop/delay/corrupt) apply the same way

**Workaround:** Pre-process IPv6 traffic through tunnel (IPv6 → IPv4) if needed for testing.

**Next Steps:**
- [ ] Add IPv6 header parsing (similar to IPv4)
- [ ] Dual-stack support in chaos engine
- [ ] Test IPv6 + IPv4 mixed workloads

---

#### ❌ Single-Node Only (No Distributed Coordination)

**What it means:** Chronos-X runs on one machine. Cannot coordinate rules across multiple instances for distributed chaos injection.

**Impact:** Testing distributed systems requires per-node rule management (manual synchronization).

**Why:** Distributed consensus is hard and orthogonal to the core problem (lock-free data plane). Single-node avoids:
- Network RPC latency jitter
- Consensus protocol complexity
- Determinism across machines (wall-clock variation)

**By Design:** This is a single-node chaos tool, not a distributed systems framework.

**Workaround:** Script rule management across multiple instances or use external orchestration.

**Future (Lower Priority):**
- [ ] gRPC for rule sync across instances
- [ ] Accept determinism hit from network RPC
- [ ] Use hash(flow, seed, machine_id) for cross-machine determinism

---

### Permissions & Privileges

#### ❌ Root / CAP_NET_ADMIN Required (AF_XDP Mode)

**What it means:** Live packet interception requires elevated privileges. Demo mode works unprivileged.

**Impact:** Cannot run AF_XDP mode in unprivileged containers or CI environments.

**Why:** AF_XDP is a kernel resource requiring:
- Capability: `CAP_NET_ADMIN` (network administration)
- Capability: `CAP_SYS_RESOURCE` (bump rlimits)
- Cannot be delegated via namespace isolation

**Workaround:** Run demo mode (simulates chaos, no root required). Test AF_XDP on lab machine with root.

**Next Steps:**
- [ ] Explore `BPF_MAP_TYPE_RINGBUF` for unprivileged observability
- [ ] Investigate newer BPF LSM hooks (may relax requirements)
- [ ] Container image with `--cap-add NET_ADMIN` for local testing

---

### Platform Support

#### ❌ Linux-Only (No macOS, Windows, BSD)

**What it means:** Chronos-X requires Linux 5.8+ (for AF_XDP). No Windows/macOS support.

**Impact:** Can't run on macOS laptops for local development. Must use WSL2 or Linux VM.

**Why:** Core dependencies:
- AF_XDP (Linux-specific)
- eBPF (Linux-specific, some support on Windows 11 but limited)
- epoll (Linux-specific; could use kqueue on BSD or IOCP on Windows)

**Workaround:** WSL2 on Windows; Linux containers on macOS.

**Next Steps (Very Low Priority):**
- [ ] Abstraction layer for packet I/O (AF_XDP on Linux, pcap elsewhere)
- [ ] Port to BSD (uses kqueue instead of epoll)
- [ ] Windows 11 BPF support (experimental)

---

### Terminal & Display

#### ❌ FTXUI Requires Interactive Terminal

**What it means:** Dashboard rendering requires a real terminal (TTY). Fails on headless/piped output.

**Impact:** Can't stream dashboard over SSH or into CI/CD logs.

**Workaround:** Use `--output json` for JSON stats export (future), or log text summary.

**Next Steps:**
- [ ] Add JSON stats output mode
- [ ] Grafana/Prometheus integration
- [ ] Headless mode: print stats periodically to stdout

---

### Protocol & Security

#### ⚠️ No Authentication on Control Port (Local-Only)

**What it means:** TCP:9090 control port accepts plaintext commands with no authentication. Requires firewall isolation.

**Impact:** Cannot expose control port across untrusted networks. Risk of accidental or malicious rule injection.

**Design Decision:** Chronos-X is a lab tool for HFT systems. Authentication is out of scope. Assume trusted network.

**Workaround:** Firewall rule to restrict TCP:9090 to `localhost` and specific IPs.

**Next Steps (If Needed):**
- [ ] HMAC-SHA256 authentication on protocol messages
- [ ] mTLS for remote control
- [ ] Role-based access control (list-only vs. modify)

---

#### ❌ No Rule Persistence (Memory-Only)

**What it means:** Rules live in memory. Restarting Chronos-X loses all active rules.

**Impact:** Cannot resume chaos injection after crash or planned restart.

**Why:** Persistence adds complexity (state serialization, recovery, consistency). For lab testing, usually acceptable.

**Workaround:** Script rule injection via Python client on startup.

**Next Steps (If Needed):**
- [ ] Snapshot rules to JSON file
- [ ] Restore on startup
- [ ] Periodic sync to disk

---

### Observation & Instrumentation

#### ⚠️ Limited Observability into eBPF/Kernel

**What it means:** Cannot easily observe eBPF program execution or kernel-side stats. Debug prints don't work in XDP context.

**Impact:** Harder to diagnose kernel-side issues (dropped packets, NIC overload, etc.).

**Workaround:** Use `bpftrace` or `bpf perf` for kernel inspection.

**Next Steps:**
- [ ] Add BPF_MAP_TYPE_ARRAY for kernel-side counters
- [ ] Export counters to userspace stats
- [ ] Integration with existing BPF tooling (bpftrace)

---

#### ❌ No Distributed Tracing

**What it means:** Cannot trace a single packet's fate across data/control/UI planes end-to-end.

**Impact:** Harder to debug why specific packets were dropped or delayed.

**Why:** Would require adding trace context to every packet (overhead + complexity).

**Workaround:** Use debug mode with limited packet count or replay test trace.

**Next Steps (If Needed):**
- [ ] Per-packet trace context (optional feature)
- [ ] OpenTelemetry integration
- [ ] Trace export to Jaeger/Honeycomb

---

## Comparison: Chronos-X vs. Alternatives

| Feature | Chronos-X | tc netem | DPDK | Raw Sockets |
|---------|-----------|---------|------|-------------|
| **Throughput** | 5-10 Mbps | 1-2 Mbps | 40+ Gbps | <1 Mbps |
| **Determinism** | ✅ | ❌ | ⚠️ | ❌ |
| **Setup Complexity** | Low | Low | High | Very Low |
| **Dependencies** | libbpf, libxdp | kernel module | DPDK, huge pages | none |
| **Multi-NIC** | ❌ | ✅ | ✅ | ✅ |
| **IPv6** | ❌ | ✅ | ✅ | ✅ |
| **Root Required** | ✅ (AF_XDP) | ✅ | ✅ | ✅ |
| **Testing Mode** | ✅ (demo) | ❌ | ❌ | ❌ |

**Chronos-X sweet spot:** Single-machine, deterministic chaos injection for HFT systems testing.

---

## Roadmap: Next Steps (Priority Order)

### Priority 1: Core Robustness
- [ ] Watchdog timer to detect data plane hangs
- [ ] Graceful shutdown on AF_XDP socket errors
- [ ] Handle NIC hotplug / interface removal
- [ ] Add system tests (full pipeline on real NIC)

### Priority 2: Performance & Scaling
- [ ] Multi-queue sharding (per-queue data plane threads)
- [ ] IPv6 support
- [ ] Benchmark on 40 Gbps NIC
- [ ] Optimize parser cache behavior

### Priority 3: Observability
- [ ] JSON stats export
- [ ] Prometheus metrics endpoint
- [ ] Per-packet trace mode (optional feature flag)
- [ ] BPF program stats export

### Priority 4: Ergonomics
- [ ] Config file format (YAML rules definition)
- [ ] Rule templates (common chaos patterns)
- [ ] Headless dashboard (periodic stdout output)
- [ ] Python API for programmatic rule management

### Priority 5: Advanced Features
- [ ] Distributed coordination (gRPC)
- [ ] Packet replay from pcap file
- [ ] Advanced chaos: reorder, duplicate, corrupt-specific-bytes
- [ ] Hardware timestamping for sub-microsecond latency

---

## Known Issues

### Build Issues

**Q: `fatal error: linux/if_xdp.h: No such file or directory`**  
A: Install `linux-headers` and `libxdp-dev`:
```bash
sudo apt-get install -y linux-headers-$(uname -r) libxdp-dev libbpf-dev
```

**Q: ASan reports memory leak in FTXUI**  
A: FTXUI has known FTXUI allocations that are intentionally never freed (terminal state). Safe to suppress.

### Runtime Issues

**Q: `Permission denied` when running with AF_XDP**  
A: Requires root or `CAP_NET_ADMIN`:
```bash
sudo ./build/chronosx --server 9090
```

**Q: `NIC queue not found` or `No queue available`**  
A: Ensure AF_XDP is supported on your NIC. Run demo mode instead:
```bash
./build/chronosx  # Simulation mode, no NIC required
```

---

## Testing the Limitations

### Test: Demo Mode (No AF_XDP Required)
```bash
./build/chronosx --demo
# Should run packet simulation without root or NIC
```

### Test: Sanitizers (Memory Safety)
```bash
./scripts/build.sh --sanitizers
./scripts/run_tests.sh
# Should report 0 memory errors, 0 data races
```

### Test: High-Frequency Stress
```bash
./build/test_stress
# Should handle 1M packets + concurrent rule updates without deadlock
```

---

## Feedback & Contributions

Found a limitation or issue? Please:
1. Check this document first
2. File a GitHub Issue with reproducible steps
3. Reference relevant docs/tests
4. Consider contributing a fix!

---

## Conclusion

Chronos-X trades **breadth** (multi-NIC, multi-platform) for **depth** (determinism, lock-free, testability). This is a deliberate choice optimizing for HFT systems testing, not general-purpose network chaos.

The limitations are **honest and fixable,** not fundamental flaws. Each exists because the project prioritized:
1. **Correctness** (deterministic latency)
2. **Testability** (simulation mode)
3. **Simplicity** (single-node, lock-free)

Over:
- Breadth (multi-NIC, distributed)
- Ease-of-use (no root, no setup)
- Production readiness (persistence, monitoring)
