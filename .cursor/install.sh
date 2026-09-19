#!/usr/bin/env bash
# Idempotent environment setup for raspberrypi-mmbasic / mmcore.
#
# Linux (Cursor cloud / Debian-ish): installs apt packages + ARM GNU
# toolchains, inits submodules, venv, and builds the QEMU console image.
#
# macOS (local T3 Code worktrees): assumes Homebrew + ARM none-elf
# toolchains are already present, then inits submodules and the pytest
# venv. Skips apt/toolchain downloads and the full image build so
# worktree creation stays fast. Set MMCORE_WORKTREE_BUILD=1 to also build.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OS="$(uname -s)"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\n\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

setup_submodules_and_venv() {
  log "Initialising git submodules (circle, picomite-fork)"
  git -C "${REPO_ROOT}" submodule update --init

  log "Creating Python virtualenv for the test harness"
  if [ ! -x "${REPO_ROOT}/.venv/bin/python" ]; then
    python3 -m venv "${REPO_ROOT}/.venv"
  fi
  "${REPO_ROOT}/.venv/bin/pip" install -q --upgrade pip
  "${REPO_ROOT}/.venv/bin/pip" install -q -r "${REPO_ROOT}/requirements.txt"
}

setup_macos() {
  log "macOS setup (Homebrew toolchains expected)"
  export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"

  need_cmd aarch64-none-elf-gcc
  need_cmd arm-none-eabi-gcc
  need_cmd qemu-system-aarch64
  need_cmd cmake
  need_cmd python3
  need_cmd tesseract
  need_cmd convert
  need_cmd make
  need_cmd git

  setup_submodules_and_venv

  if [ "${MMCORE_WORKTREE_BUILD:-0}" = "1" ]; then
    log "Building the bare-metal console image (MMCORE_WORKTREE_BUILD=1)"
    bash "${REPO_ROOT}/scripts/build.sh"
  else
    log "Skipping full image build (set MMCORE_WORKTREE_BUILD=1 to enable)"
  fi

  log "Toolchain + tooling versions"
  aarch64-none-elf-gcc --version | head -1
  arm-none-eabi-gcc --version | head -1
  qemu-system-aarch64 --version | head -1
  tesseract --version 2>&1 | head -1
  cmake --version | head -1

  log "macOS environment ready"
}

setup_linux() {
  ARM_RELEASE="15.2.rel1"
  ARM_BASE="https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_RELEASE}/binrel"
  OPT_DIR="/opt"
  BIN_DIR="/usr/local/bin"
  TOOLCHAINS=("aarch64-none-elf" "arm-none-eabi")

  SUDO=""
  if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
  fi

  apt_get() {
    local i
    for i in 1 2 3 4 5; do
      if $SUDO DEBIAN_FRONTEND=noninteractive apt-get "$@"; then
        return 0
      fi
      log "apt-get $* failed (attempt $i); retrying"
      sleep $((i * 4))
      $SUDO apt-get update -y || true
    done
    return 1
  }

  log "Installing host build + emulation + test prerequisites"
  apt_get update -y
  apt_get install -y --no-install-recommends \
    build-essential make cmake git curl wget ca-certificates \
    xz-utils python3 python3-venv python3-pip pkg-config gawk \
    qemu-system-arm tesseract-ocr imagemagick

  install_toolchain() {
    local name="$1"
    local link="${OPT_DIR}/${name}"
    if [ -x "${BIN_DIR}/${name}-gcc" ] && [ -d "${link}" ]; then
      log "Toolchain ${name} already installed; skipping"
      return 0
    fi

    local tarball="arm-gnu-toolchain-${ARM_RELEASE}-x86_64-${name}.tar.xz"
    local cached="/tmp/tc/${tarball}"
    local src

    if [ -f "${cached}" ]; then
      src="${cached}"
    else
      log "Downloading ${tarball}"
      src="${OPT_DIR}/${tarball}"
      $SUDO curl -fSL --retry 5 --retry-all-errors --retry-delay 4 \
        --connect-timeout 30 "${ARM_BASE}/${tarball}" -o "${src}"
    fi

    log "Extracting ${name} toolchain into ${OPT_DIR}"
    local extracted_dir="${OPT_DIR}/arm-gnu-toolchain-${ARM_RELEASE}-x86_64-${name}"
    $SUDO rm -rf "${extracted_dir}"
    $SUDO tar -xf "${src}" -C "${OPT_DIR}"
    $SUDO ln -sfn "${extracted_dir}" "${link}"
    [ "${src}" = "${OPT_DIR}/${tarball}" ] && $SUDO rm -f "${src}" || true

    log "Linking ${name} binaries into ${BIN_DIR}"
    for bin in "${link}/bin/${name}-"*; do
      $SUDO ln -sfn "${bin}" "${BIN_DIR}/$(basename "${bin}")"
    done
  }

  for tc in "${TOOLCHAINS[@]}"; do
    install_toolchain "${tc}"
  done

  setup_submodules_and_venv

  log "Building the bare-metal console image"
  bash "${REPO_ROOT}/scripts/build.sh"

  log "Toolchain + tooling versions"
  aarch64-none-elf-gcc --version | head -1
  arm-none-eabi-gcc --version | head -1
  qemu-system-aarch64 --version | head -1
  tesseract --version 2>&1 | head -1
  cmake --version | head -1

  log "Linux environment ready"
}

case "${OS}" in
  Darwin) setup_macos ;;
  Linux)  setup_linux ;;
  *)      die "unsupported OS: ${OS}" ;;
esac
