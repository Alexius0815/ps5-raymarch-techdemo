#!/bin/bash
#
# Runs the macOS build of the Raymarch Tech Demo (run/eboot from build.sh with
# ANYPS5_BUILD set) through the Khronos Vulkan loader and MoltenVK.
#
# Environment:
#   VULKAN_LOADER_DIR  Directory containing libvulkan.1.dylib (required).
#   MOLTENVK_ICD       Path to MoltenVK_icd.json (required).
#
# Usage: run_macos.sh
# Output: run/play.log. Click the window before using the controller.

set -o nounset

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR
readonly RUN_DIR="${ROOT_DIR}/run"
readonly DEMO_LOG="${RUN_DIR}/play.log"

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

main() {
  if [[ -z "${VULKAN_LOADER_DIR:-}" || -z "${MOLTENVK_ICD:-}" ]]; then
    err "set VULKAN_LOADER_DIR and MOLTENVK_ICD (see README.md)"
    exit 1
  fi
  if [[ ! -x "${RUN_DIR}/eboot" ]]; then
    err "not built: run ANYPS5_BUILD=... ./build.sh first"
    exit 1
  fi
  if ! cd -- "${RUN_DIR}"; then
    err "cannot change to ${RUN_DIR}"
    exit 1
  fi
  echo "Raymarch Tech Demo starting ... click the window. L3 shows the controls."
  # Guest programs get only the Vulkan loader on their library path.
  DYLD_LIBRARY_PATH="${VULKAN_LOADER_DIR}" \
    VK_DRIVER_FILES="${MOLTENVK_ICD}" \
    MVK_CONFIG_LOG_LEVEL=1 \
    ./eboot > "${DEMO_LOG}" 2>&1
  local -i exit_code=$?
  echo "Demo finished (status ${exit_code}). Log: run/play.log"
  exit "${exit_code}"
}

main "$@"
