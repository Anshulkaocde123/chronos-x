#!/usr/bin/env bash
# =============================================================================
# run_benchmark.sh — Run performance benchmarks
#
# Usage:
#   ./scripts/run_benchmark.sh              # Run benchmark (default: 1M iterations)
#   ./scripts/run_benchmark.sh 10000        # Run with 10K iterations
#   ./scripts/run_benchmark.sh --release    # Run benchmark (release build)
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"

ITERATIONS=1000000

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)
            BUILD_DIR="${PROJECT_ROOT}/build-release"
            shift
            ;;
        [0-9]*)
            ITERATIONS="$1"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--release] [iterations]"
            exit 1
            ;;
    esac
done

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory not found: $BUILD_DIR"
    echo "Run './scripts/build.sh' first to build the project."
    exit 1
fi

# Check if benchmark binary exists
BENCHMARK_BIN="${BUILD_DIR}/bench_data_plane"
if [ ! -f "$BENCHMARK_BIN" ]; then
    echo "Error: Benchmark binary not found: $BENCHMARK_BIN"
    echo "Run './scripts/build.sh' first to build the project."
    exit 1
fi

echo "=========================================="
echo "Running Data Plane Benchmark"
echo "=========================================="
echo "Iterations: $ITERATIONS"
echo "Binary: $BENCHMARK_BIN"
echo ""

# Run benchmark
echo "Executing benchmark..."
"$BENCHMARK_BIN" "$ITERATIONS"

echo ""
echo "=========================================="
echo "✓ Benchmark complete!"
echo "=========================================="
echo ""
echo "Interpretation:"
echo "  throughput_mpps: Million packets per second"
echo "  throughput_gbps: Gigabits per second"
echo ""
echo "Example (healthy system):"
echo "  throughput_mpps = 6.84"
echo "  throughput_gbps = 3.50"
echo ""
echo "Performance varies by:"
echo "  - CPU model and frequency"
echo "  - Kernel version"
echo "  - Other system load"
