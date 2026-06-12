#!/usr/bin/env bash
# =============================================================================
# run_tests.sh — Run the full test suite
#
# Usage:
#   ./scripts/run_tests.sh              # Run tests (debug build)
#   ./scripts/run_tests.sh --release    # Run tests (release build)
#   ./scripts/run_tests.sh --sanitizers # Run tests with ASan/UBSan
#   ./scripts/run_tests.sh --verbose    # Verbose output
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"

CTEST_ARGS=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)
            BUILD_DIR="${PROJECT_ROOT}/build-release"
            shift
            ;;
        --sanitizers)
            BUILD_DIR="${PROJECT_ROOT}/build-san"
            shift
            ;;
        --verbose|-V)
            CTEST_ARGS="--output-on-failure -V"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--release|--sanitizers|--verbose]"
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

echo "=========================================="
echo "Running Test Suite"
echo "=========================================="
echo "Build directory: $BUILD_DIR"
echo ""

cd "$BUILD_DIR"

# Run tests
echo "Executing tests..."
if [ -z "$CTEST_ARGS" ]; then
    ctest --output-on-failure
else
    ctest $CTEST_ARGS
fi

# Summary
echo ""
echo "=========================================="
echo "✓ Test suite complete!"
echo "=========================================="
