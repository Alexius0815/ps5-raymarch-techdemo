#!/bin/bash
#
# Builds the Raymarch Tech Demo as a native PS5 executable and, optionally,
# relinks it into a macOS executable for AnyPS5 (Rosetta 2, MoltenVK).
#
# clang compiles for x86_64-sie-ps5, ld.lld links against the stub libraries
# of the open ps5-payload-sdk, and ps5-native-tool (from the GPL-3.0
# ps5-native-app-boilerplate) converts the result into a PS5 executable.
# No proprietary Sony file is used.
#
# Environment:
#   PS5_PAYLOAD_SDK  Path to the ps5-payload-sdk (required).
#   PS5_NATIVE_TOOL  Path to ps5-native-tool (required).
#   LLD              ld.lld to use (default: ld.lld on PATH).
#   ANYPS5_BUILD     AnyPS5 build directory with core/relinker/relinker and
#                    core/libs/libs; when set, also builds run/eboot for
#                    macOS (optional).
#
# Usage: build.sh
# Output: build/eboot.elf (PS5) and, with ANYPS5_BUILD, run/eboot (macOS).

set -o nounset
set -o pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR
readonly BUILD_DIR="${ROOT_DIR}/build"
readonly RUN_DIR="${ROOT_DIR}/run"
readonly SDK_DIR="${PS5_PAYLOAD_SDK:-}"
readonly NATIVE_TOOL="${PS5_NATIVE_TOOL:-}"
readonly LLD="${LLD:-ld.lld}"
readonly RELINKER_BUILD="${ANYPS5_BUILD:-}"
# Loader constants of the boilerplate, validated there on firmware 6.02/12.70.
readonly MODULE_SDK="0x02000009"
readonly COMPANION_SDK="0x08050001"
# Build identity shown by the demo: a version and a content hash of the
# source, so identical sources give identical builds (no timestamps).
readonly DEMO_VERSION="1.3"
DEMO_SOURCE_HASH="$(shasum -a 256 "${ROOT_DIR}/src/showcase.cpp" | cut -c1-8)"
readonly DEMO_SOURCE_HASH
readonly DEMO_DEFINES=(
  "-DSHOWCASE_VERSION=\"${DEMO_VERSION}\""
  "-DSHOWCASE_SOURCE=\"${DEMO_SOURCE_HASH}\""
)

#######################################
# Prints an error message with a timestamp.
# Arguments:
#   Message words.
# Outputs:
#   Writes the message to STDERR.
#######################################
err() {
  echo "[$(date +'%Y-%m-%dT%H:%M:%S%z')]: $*" >&2
}

#######################################
# Compiles one C++ source for the PS5 target.
# Globals:
#   DEMO_DEFINES, SDK_DIR
# Arguments:
#   Source file, object file.
# Returns:
#   The exit status of clang.
#######################################
compile() {
  local source="$1"
  local object="$2"
  clang -target x86_64-sie-ps5 -std=c++20 -O3 -ffast-math \
    -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections \
    -fvisibility-nodllstorageclass=default \
    -fno-stack-protector -fno-plt -femulated-tls \
    -isysroot "${SDK_DIR}" \
    -isystem "${SDK_DIR}/target/include/c++/v1" \
    -isystem "${SDK_DIR}/target/include" \
    "${DEMO_DEFINES[@]}" \
    -c "${source}" -o "${object}"
}

#######################################
# Links all objects into a position-independent PS5 ELF.
# Globals:
#   BUILD_DIR, LLD, ROOT_DIR, SDK_DIR
# Arguments:
#   Object files.
# Returns:
#   The exit status of ld.lld.
#######################################
link_elf() {
  "${LLD}" -m elf_x86_64 -pie --eh-frame-hdr -z max-page-size=0x4000 \
    -mllvm -emulated-tls --hash-style=gnu \
    -T "${ROOT_DIR}/native/ps5-pie.ld" \
    --version-script "${ROOT_DIR}/native/app-symbols.map" \
    -e _start -o "${BUILD_DIR}/llvm-pie.elf" "$@" \
    --as-needed "${SDK_DIR}"/target/lib/*.so
}

#######################################
# Relinks the PS5 executable into a macOS executable next to libs and app0.
# Globals:
#   BUILD_DIR, RELINKER_BUILD, ROOT_DIR, RUN_DIR
# Returns:
#   Non-zero if the relinker fails.
#######################################
relink_for_mac() {
  local source_dir="${BUILD_DIR}/src"
  mkdir -p "${source_dir}/sce_module" "${RUN_DIR}/app0"
  cp "${BUILD_DIR}/eboot.elf" "${source_dir}/eboot.elf"
  if ! (cd "${source_dir}" && "${RELINKER_BUILD}/core/relinker/relinker" \
    --macos --to-rosetta eboot.elf "${RUN_DIR}/eboot" \
    > "${BUILD_DIR}/relink.log" 2>&1); then
    err "relink failed, see build/relink.log"
    return 1
  fi
  chmod 755 "${RUN_DIR}/eboot"
  rm -rf "${RUN_DIR}/app0/sce_sys" "${RUN_DIR}/app0/assets"
  cp -R "${ROOT_DIR}/sce_sys" "${ROOT_DIR}/assets" "${RUN_DIR}/app0/"
  ln -sfn "${RELINKER_BUILD}/core/libs/libs" "${RUN_DIR}/libs"
}

main() {
  if [[ -z "${SDK_DIR}" || -z "${NATIVE_TOOL}" ]]; then
    err "set PS5_PAYLOAD_SDK and PS5_NATIVE_TOOL (see README.md)"
    exit 1
  fi
  local tool
  for tool in "${NATIVE_TOOL}" "${SDK_DIR}/target/lib/libkernel.so"; do
    if [[ ! -e "${tool}" ]]; then
      err "missing: ${tool}"
      exit 1
    fi
  done
  if ! command -v "${LLD}" > /dev/null; then
    err "ld.lld not found; set LLD"
    exit 1
  fi
  mkdir -p "${BUILD_DIR}/obj"
  local -a objects=()
  local source object
  for source in "${ROOT_DIR}"/src/*.cpp "${ROOT_DIR}"/native/*.cpp; do
    object="${BUILD_DIR}/obj/$(basename "${source}").o"
    echo "CC   ${source#"${ROOT_DIR}"/}"
    if ! compile "${source}" "${object}"; then
      err "compile failed: ${source}"
      exit 1
    fi
    objects+=("${object}")
  done
  echo "LINK build/llvm-pie.elf"
  if ! link_elf "${objects[@]}"; then
    err "link failed"
    exit 1
  fi
  echo "PS5  build/eboot.elf"
  if ! "${NATIVE_TOOL}" link --in "${BUILD_DIR}/llvm-pie.elf" \
    --out "${BUILD_DIR}/eboot.elf" --stub-dir "${SDK_DIR}/target/lib" \
    --module-sdk "${MODULE_SDK}" --companion-sdk "${COMPANION_SDK}" \
    --file-name eboot.elf; then
    err "conversion to a PS5 executable failed"
    exit 1
  fi
  if [[ -z "${RELINKER_BUILD}" ]]; then
    echo "Done: build/eboot.elf (set ANYPS5_BUILD for a macOS build)"
    return 0
  fi
  echo "MAC  run/eboot"
  relink_for_mac || exit 1
  echo "Done: build/eboot.elf and run/eboot"
}

main "$@"
