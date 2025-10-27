#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------
# Usage:
#   ./compile-with-docker.sh [Preset] [CMake options...]
# Examples:
#   ./compile-with-docker.sh Custom
#   ./compile-with-docker.sh Bandscope -DENABLE_SPECTRUM=ON
#   ./compile-with-docker.sh Broadcast -DENABLE_FEAT_F4HWN_GAME=ON -DENABLE_NOAA=ON
# Default preset: "Custom"
# ---------------------------------------------

IMAGE=uvk1-uvk5v3
PRESET=${1:-Custom}
shift || true  # remove preset from arguments if present

# Any remaining args will be treated as CMake cache variables
EXTRA_ARGS=("$@")

# ---------------------------------------------
# Validate preset name
# ---------------------------------------------
if [[ ! "$PRESET" =~ ^(Custom|Bandscope|Broadcast|Basic|RescueOps|Game|all)$ ]]; then
  echo "❌ Unknown preset: '$PRESET'"
  echo "Valid presets are: Custom, Bandscope, Broadcast, Basic, RescueOps, Game, all"
  exit 1
fi

# ---------------------------------------------
# Build the Docker image (only needed once)
# ---------------------------------------------
docker build -t "$IMAGE" .

# ---------------------------------------------
# Clean existing CMake cache to ensure toolchain reload
# ---------------------------------------------
rm -rf build

# ---------------------------------------------
# Function to build one preset
# ---------------------------------------------
build_preset() {
  local preset="$1"
  echo "=== 🚀 Building preset: ${preset} ==="
  docker run --rm -it -v "$PWD":/src -w /src "$IMAGE" \
    bash -c "which arm-none-eabi-gcc && arm-none-eabi-gcc --version && \
             cmake --preset ${preset} ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} && \
             cmake --build --preset ${preset} -j"
}

build_preset "$PRESET"
