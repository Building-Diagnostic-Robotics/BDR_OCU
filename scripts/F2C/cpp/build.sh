#!/bin/bash
# Build script for BDR Coverage Planner (C++)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="${1:-Release}"

echo "=== BDR Coverage Planner C++ Build ==="
echo "Build type: ${BUILD_TYPE}"
echo "Build directory: ${BUILD_DIR}"
echo ""

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure
echo ">>> Configuring..."
cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} ..

# Build
echo ""
echo ">>> Building..."
make -j$(nproc)

echo ""
echo "=== Build complete ==="
echo "Executable: ${BUILD_DIR}/bdr_coverage_planner"
echo ""
echo "To run: ${BUILD_DIR}/bdr_coverage_planner"

