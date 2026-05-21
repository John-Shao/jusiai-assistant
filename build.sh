#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Cross-compile JuSi AI Assistant for the Rockchip RV1126B board.
#
#   ./build.sh [--sdk <livekit-sdk-cpp dir>] [--deps <dir>] [--clean]
#
# Sources the SDK's scripts/env-rv1126b.sh (toolchain, sysroot, webrtc/MPP
# env) and configures with its RV1126B CMake toolchain file. LVGL and
# nlohmann-json are auto-discovered from a deps/ directory when present, so
# the build never has to reach GitHub.
# ---------------------------------------------------------------------------
# Note: no `-u` — the SDK's env-rv1126b.sh probes optional unset variables.
set -eo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="${SDK_DIR:-$HOME/livekit/livekit-sdk-cpp-0.3.3}"
DEPS_DIR=""
DO_CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sdk)   SDK_DIR="$2";   shift 2 ;;
    --deps)  DEPS_DIR="$2";  shift 2 ;;
    --clean) DO_CLEAN=1;     shift ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

SDK_DIR="$(cd "$SDK_DIR" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-rv1126b"

TOOLCHAIN="$SDK_DIR/cmake/toolchains/rv1126b-aarch64-linux-gnu.cmake"
ENV_SCRIPT="$SDK_DIR/scripts/env-rv1126b.sh"
for f in "$TOOLCHAIN" "$ENV_SCRIPT"; do
  [[ -f "$f" ]] || { echo "ERROR: missing $f" >&2; exit 1; }
done

echo "==> LiveKit SDK : $SDK_DIR"
echo "==> Build dir   : $BUILD_DIR"

# Cross-compile environment: toolchain, sysroot, pkg-config, webrtc, MPP.
# shellcheck disable=SC1090
source "$ENV_SCRIPT"

# Auto-discover staged dependency sources so FetchContent never downloads
# (the build VM cannot reliably reach GitHub). Each staged directory is
# classified by content and mapped to the matching FETCHCONTENT_SOURCE_DIR_*.
#   LVGL / NLOHMANN_JSON          — this project
#   LIVEKIT_{ABSEIL,PROTOBUF,SPDLOG} — the LiveKit SDK sub-project
declare -A FC
classify_dep() {
  local d="${1%/}"
  [[ -f "$d/lvgl.h" ]] && FC[LVGL]="$d"
  [[ -f "$d/CMakeLists.txt" && -d "$d/include/nlohmann" ]] && FC[NLOHMANN_JSON]="$d"
  [[ -d "$d/absl" && -f "$d/CMakeLists.txt" ]] && FC[LIVEKIT_ABSEIL]="$d"
  [[ -d "$d/src/google/protobuf" ]] && FC[LIVEKIT_PROTOBUF]="$d"
  [[ -d "$d/include/spdlog" ]] && FC[LIVEKIT_SPDLOG]="$d"
  return 0  # never let a failed test trip `set -e`
}
SCAN_DIRS=("$PROJECT_ROOT/deps" "$HOME/lk-deps" "$PROJECT_ROOT/../lk-deps")
if [[ -n "$DEPS_DIR" ]]; then
  SCAN_DIRS=("$DEPS_DIR" "${SCAN_DIRS[@]}")
fi
for parent in "${SCAN_DIRS[@]}"; do
  [[ -d "$parent" ]] || continue
  for d in "$parent"/*/; do classify_dep "$d"; done
done
CMAKE_DEP_ARGS=()
for k in "${!FC[@]}"; do
  echo "==> dep $k -> ${FC[$k]}"
  CMAKE_DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_$k=${FC[$k]}")
done

if [[ "$DO_CLEAN" == "1" ]]; then
  echo "==> Removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIVEKIT_SDK_DIR="$SDK_DIR" \
  "${CMAKE_DEP_ARGS[@]}"

cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "==> Done. Executable: $BUILD_DIR/bin/jusiai-assistant"
echo "    Deploy bin/ to the board and run there."
