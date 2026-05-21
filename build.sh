#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Build helper for JuSi AI Assistant (Linux).
#
#   ./build.sh [release|debug] [options]
#
# Options:
#   --sdk <dir>     Path to the livekit-sdk-cpp checkout (dev branch).
#                   Default: ../livekit-sdk-cpp
#   --deps <dir>    Directory of pre-staged dependencies, for hosts whose
#                   GitHub access is unreliable. Recognised sub-directories:
#                     spdlog-*/           -> spdlog source
#                     SDL-*/ | SDL3-*/    -> SDL3 source
#                     lvgl-*/             -> LVGL v8.4 source
#                     json/               -> nlohmann-json source
#                     *-release/          -> extracted prebuilt libwebrtc
#                   Any entry that is absent is downloaded normally.
#   --clean         Remove the build directory before configuring.
#
# The LiveKit SDK is compiled as part of this build, so its prerequisites
# (Rust/Cargo, llvm/clang) must be installed first — see README.md.
# ---------------------------------------------------------------------------
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_TYPE="Release"
SDK_DIR="${PROJECT_ROOT}/../livekit-sdk-cpp"
DEPS_DIR=""
DO_CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    release) BUILD_TYPE="Release"; shift ;;
    debug)   BUILD_TYPE="Debug";   shift ;;
    --sdk)   SDK_DIR="$2";         shift 2 ;;
    --deps)  DEPS_DIR="$2";        shift 2 ;;
    --clean) DO_CLEAN=1;           shift ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

BUILD_DIR="${PROJECT_ROOT}/build-$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
SDK_DIR="$(cd "${SDK_DIR}" && pwd)"

[[ "${DO_CLEAN}" == "1" ]] && { echo "==> Removing ${BUILD_DIR}"; rm -rf "${BUILD_DIR}"; }

# --- Build environment -----------------------------------------------------
# Newer GCC is stricter about WebRTC's legacy headers (pulled in via the SDK).
export CXXFLAGS="${CXXFLAGS:-} -Wno-deprecated-declarations"
export CFLAGS="${CFLAGS:-} -Wno-deprecated-declarations"

# Rust bindgen (used by the SDK's webrtc-sys crate) needs libclang.
if [[ -z "${LIBCLANG_PATH:-}" ]]; then
  _libclang="$(find /usr/lib /usr/lib64 -name 'libclang.so*' 2>/dev/null | head -n1 || true)"
  [[ -n "${_libclang}" ]] && export LIBCLANG_PATH="$(dirname "${_libclang}")"
fi

# --- Pre-staged dependencies (optional) ------------------------------------
# Auto-discover a deps/ directory when --deps was not given, so a plain
# `./build.sh release` still picks up staged dependencies (notably the
# prebuilt libwebrtc — webrtc-sys downloads it from GitHub otherwise, and
# that download is unreliable on some networks).
if [[ -z "${DEPS_DIR}" ]]; then
  for _cand in "${PROJECT_ROOT}/deps" "${PROJECT_ROOT}/../deps"; do
    if [[ -d "${_cand}" ]]; then DEPS_DIR="${_cand}"; break; fi
  done
fi

CMAKE_DEP_ARGS=()
if [[ -n "${DEPS_DIR}" ]]; then
  DEPS_DIR="$(cd "${DEPS_DIR}" && pwd)"
  echo "==> Using pre-staged dependencies from ${DEPS_DIR}"
  _find_dep() { find "${DEPS_DIR}" -maxdepth 1 -type d -name "$1" 2>/dev/null | head -n1; }

  _d="$(_find_dep 'spdlog-*')"
  [[ -n "${_d}" ]] && CMAKE_DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_LIVEKIT_SPDLOG=${_d}")
  _d="$(_find_dep 'SDL-*')"; [[ -z "${_d}" ]] && _d="$(_find_dep 'SDL3-*')"
  [[ -n "${_d}" ]] && CMAKE_DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_SDL3=${_d}")
  _d="$(_find_dep 'lvgl-*')"
  [[ -n "${_d}" ]] && CMAKE_DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_LVGL=${_d}")
  _d="$(_find_dep 'json')"
  [[ -n "${_d}" ]] && CMAKE_DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${_d}")
  _d="$(_find_dep '*-release')"
  if [[ -n "${_d}" ]]; then
    export LK_CUSTOM_WEBRTC="${_d}"
    echo "==> LK_CUSTOM_WEBRTC=${LK_CUSTOM_WEBRTC}"
  fi
fi

echo "==> Build type : ${BUILD_TYPE}"
echo "==> LiveKit SDK: ${SDK_DIR}"
echo "==> Build dir  : ${BUILD_DIR}"
echo "==> LIBCLANG   : ${LIBCLANG_PATH:-<not found>}"

# --- Configure + build -----------------------------------------------------
GEN_ARGS=()
command -v ninja >/dev/null 2>&1 && GEN_ARGS=(-G Ninja)

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
  "${GEN_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DLIVEKIT_SDK_DIR="${SDK_DIR}" \
  "${CMAKE_DEP_ARGS[@]}"

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo
echo "==> Done. Executable: ${BUILD_DIR}/bin/jusiai-assistant"
echo "    Run it with:  cd ${BUILD_DIR}/bin && ./jusiai-assistant"
