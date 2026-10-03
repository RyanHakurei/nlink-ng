#!/usr/bin/env bash
# Build a 64-bit Windows n-link folder (exe + DLLs) into testing/windows/.
# Uses a MinGW-w64 compiler on PATH, or Docker on Linux when it is missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/qt/build-windows"
OUT="${ROOT}/testing/windows"
MINGW_PREFIX="${MINGW_PREFIX:-/usr/x86_64-w64-mingw32}"
if [[ -d "${MINGW_PREFIX}/sys-root/mingw/bin" ]]; then
  MINGW_ROOT="${MINGW_PREFIX}/sys-root/mingw"
else
  MINGW_ROOT="${MINGW_PREFIX}"
fi
IMAGE="${NLINK_MINGW_IMAGE:-nlink-ng-mingw}"

is_system_dll() {
  local name="${1,,}"
  case "$name" in
    api-ms-win-*|ext-ms-win-*) return 0 ;;
    kernel32.dll|kernelbase.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll| \
      ole32.dll|oleaut32.dll|ws2_32.dll|bcrypt.dll|ntdll.dll|setupapi.dll| \
      cfgmgr32.dll|imm32.dll|comdlg32.dll|winmm.dll|msvcrt.dll|rpcrt4.dll| \
      shlwapi.dll|uxtheme.dll|dwmapi.dll|d3d11.dll|dxgi.dll|opengl32.dll| \
      combase.dll|userenv.dll|crypt32.dll|bcryptprimitives.dll|version.dll| \
      iphlpapi.dll|hid.dll|winspool.drv|mpr.dll|netapi32.dll|wtsapi32.dll| \
      sechost.dll|gdiplus.dll|usp10.dll|dwrite.dll|d2d1.dll|windowscodecs.dll| \
      propsys.dll|shcore.dll|comctl32.dll|wsock32.dll|normaliz.dll| \
      authz.dll|dnsapi.dll|mscoree.dll|nsi.dll|psapi.dll|powrprof.dll| \
      wlanapi.dll|dxva2.dll|d3d9.dll|d3d12.dll|winhttp.dll|cryptbase.dll| \
      sspicli.dll|secur32.dll|ncrypt.dll|dxcore.dll|dcomp.dll) return 0 ;;
    *) return 1 ;;
  esac
}

declare -a SEARCH_DIRS=()
declare -A COPIED=()

find_dll() {
  local name="$1" dir found
  for dir in "${SEARCH_DIRS[@]}"; do
    if [[ -f "${dir}/${name}" ]]; then
      echo "${dir}/${name}"
      return 0
    fi
    found="$(find "$dir" -maxdepth 1 -iname "$name" -print -quit 2>/dev/null || true)"
    if [[ -n "$found" ]]; then
      echo "$found"
      return 0
    fi
  done
  return 1
}

copy_pe() {
  local src="$1" dest_dir="$2" base dest dll found
  base="$(basename "$src")"
  dest="${dest_dir}/${base}"
  if [[ -n "${COPIED[$dest]:-}" ]]; then
    return
  fi
  COPIED[$dest]=1
  mkdir -p "$dest_dir"
  cp -a "$src" "$dest"
  while IFS= read -r dll; do
    dll="${dll//$'\r'/}"
    [[ -z "$dll" ]] && continue
    if is_system_dll "$dll"; then
      continue
    fi
    if found="$(find_dll "$dll")"; then
      copy_pe "$found" "$OUT"
    else
      echo "error: ${base} needs ${dll}, and it was not found under ${MINGW_ROOT}" >&2
      exit 1
    fi
  done < <(x86_64-w64-mingw32-objdump -p "$src" | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p')
}

copy_plugin() {
  local rel="$1" required="$2" found
  found="$(find "${MINGW_ROOT}" "${MINGW_PREFIX}" -path "*/${rel}" -print -quit 2>/dev/null || true)"
  if [[ -z "$found" ]]; then
    if [[ "$required" == "required" ]]; then
      echo "error: Qt plugin ${rel} was not found under ${MINGW_ROOT}" >&2
      exit 1
    fi
    return 0
  fi
  copy_pe "$found" "${OUT}/$(dirname "$rel")"
}

stage_runtime() {
  local exe="$1" gcc_dll
  SEARCH_DIRS=("${MINGW_ROOT}/bin" "${MINGW_PREFIX}/bin")
  gcc_dll="$(x86_64-w64-mingw32-g++ -print-file-name=libstdc++-6.dll)"
  if [[ -f "$gcc_dll" ]]; then
    SEARCH_DIRS+=("$(dirname "$gcc_dll")")
  fi
  gcc_dll="$(x86_64-w64-mingw32-gcc -print-file-name=libgcc_s_seh-1.dll)"
  if [[ -f "$gcc_dll" ]]; then
    SEARCH_DIRS+=("$(dirname "$gcc_dll")")
  fi
  rm -rf "$OUT"
  mkdir -p "$OUT"
  copy_pe "$exe" "$OUT"
  copy_plugin "platforms/qwindows.dll" required
  copy_plugin "styles/qmodernwindowsstyle.dll" optional
  x86_64-w64-mingw32-strip --strip-unneeded "${OUT}/n-link.exe"
  echo "==> ${OUT}/n-link.exe"
}

build_here() {
  if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "x86_64-w64-mingw32-g++ was not found" >&2
    exit 1
  fi
  if ! command -v rustup >/dev/null 2>&1; then
    echo "rustup is required to install the x86_64-pc-windows-gnu target" >&2
    exit 1
  fi
  if ! rustup target list --installed | grep -qx 'x86_64-pc-windows-gnu'; then
    rustup target add x86_64-pc-windows-gnu
  fi
  # Rust's x86_64-pc-windows-gnu target is msvcrt. A UCRT toolchain will not link it.
  if echo | x86_64-w64-mingw32-gcc -dM -E - | grep -q '__UCRT__\|_UCRT'; then
    echo "This MinGW gcc defaults to UCRT. n-link's Rust target needs the msvcrt MinGW toolchain." >&2
    exit 1
  fi
  export PKG_CONFIG="${PKG_CONFIG:-x86_64-w64-mingw32-pkg-config}"
  export PKG_CONFIG_ALLOW_CROSS=1
  if [[ -d "${MINGW_ROOT}/lib/pkgconfig" ]]; then
    export PKG_CONFIG_PATH="${MINGW_ROOT}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  fi
  export CC_x86_64_pc_windows_gnu="${CC_x86_64_pc_windows_gnu:-x86_64-w64-mingw32-gcc}"
  export CXX_x86_64_pc_windows_gnu="${CXX_x86_64_pc_windows_gnu:-x86_64-w64-mingw32-g++}"
  export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-x86_64-w64-mingw32-gcc}"

  echo "==> cmake windows release"
  cmake -S "${ROOT}/qt" -B "${BUILD}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT}/qt/cmake/mingw-w64-x86_64.cmake"
  cmake --build "${BUILD}" -j"$(nproc)"
  stage_runtime "${BUILD}/n-link.exe"
}

run_in_docker() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "Install mingw-w64 (gcc, Qt 6, libusb, zlib) or Docker." >&2
    exit 1
  fi
  echo "==> docker image ${IMAGE}"
  docker build -t "${IMAGE}" -f "${ROOT}/scripts/windows-toolchain.Dockerfile" "${ROOT}/scripts"
  docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp/nlink-home \
    -e CARGO_HOME=/cargo \
    -e RUSTUP_HOME=/opt/nlink-rustup \
    -e PATH="/opt/nlink-cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    -v "${HOME}/.cargo:/cargo" \
    -v "${ROOT}:${ROOT}" \
    -w "${ROOT}" \
    "${IMAGE}" \
    bash -c 'mkdir -p "$HOME" && exec bash scripts/build-windows.sh'
}

if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  build_here
elif [[ "$(uname -s)" == Linux ]]; then
  run_in_docker
else
  echo "x86_64-w64-mingw32-g++ was not found" >&2
  exit 1
fi
