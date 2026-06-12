#!/usr/bin/env bash
# =============================================================================
# build.sh — Build Chronos-X with flexible configuration
#
# Usage:
#   ./scripts/build.sh              # Build (Debug)
#   ./scripts/build.sh --release    # Build (Release)
#   ./scripts/build.sh --sanitizers # Build with ASan/UBSan
#   ./scripts/build.sh --all        # Build all variants
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"

# Defaults
BUILD_TYPE="Debug"
ENABLE_SANITIZERS=OFF
ENABLE_LIBXDP=OFF

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --sanitizers)
            ENABLE_SANITIZERS=ON
            shift
            ;;
        --libxdp)
            ENABLE_LIBXDP=ON
            shift
            ;;
        --all)
            echo "Building all variants..."
            "$SCRIPT_DIR/build.sh"
            "$SCRIPT_DIR/build.sh" --release
            "$SCRIPT_DIR/build.sh" --sanitizers
            echo "✓ All builds complete"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--release|--sanitizers|--libxdp|--all]"
            exit 1
            ;;
    esac
done

echo "=========================================="
echo "Building Chronos-X"
echo "=========================================="
echo "Build Type: $BUILD_TYPE"
echo "Sanitizers: $ENABLE_SANITIZERS"
echo "libxdp: $ENABLE_LIBXDP"
echo ""

# Clean old build (if any)
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning old build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake
echo "Configuring with CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCHRONOSX_ENABLE_SANITIZERS="$ENABLE_SANITIZERS" \
    -DCHRONOSX_ENABLE_LIBXDP="$ENABLE_LIBXDP" \
    -DCHRONOSX_WARNINGS_AS_ERRORS=ON

# Build
echo "Building..."
cmake --build . --verbose

# Summary
echo ""
echo "=========================================="
echo "✓ Build complete!"
echo "=========================================="
echo "Build directory: $BUILD_DIR"
echo "Binaries:"
find "$BUILD_DIR" -maxdepth 1 -type f -executable ! -name "CMakeFiles" | sed 's|.*|  - &|'
echo ""
echo "Next: Run tests with ./scripts/run_tests.sh"
