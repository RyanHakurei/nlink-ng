#!/usr/bin/env bash
# Build libnlink.so: C++ core + libnspire, with the TI-84 Evo link still in Rust.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/.local/android-sdk}"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_SDK_ROOT/ndk/27.0.12077973}"
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
if [[ -f "$HOME/.cargo/env" ]]; then
  # shellcheck disable=SC1091
  source "$HOME/.cargo/env"
fi

PREBUILT="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
CC="$PREBUILT/bin/aarch64-linux-android24-clang"
CXX="$PREBUILT/bin/aarch64-linux-android24-clang++"
if [[ ! -x "$CXX" || ! -x "$CC" ]]; then
  echo "NDK clang not found under $PREBUILT" >&2
  exit 1
fi

OUT="$ROOT/dist/android/android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$OUT"

cd "$ROOT/nlink"
cargo ndk -t arm64-v8a build --offline --release
RUST_LIB="$ROOT/nlink/target/aarch64-linux-android/release/libnlink.a"
if [[ ! -f "$RUST_LIB" ]]; then
  echo "Rust static library missing: $RUST_LIB" >&2
  exit 1
fi

NS="$ROOT/nlink/vendor/libnspire-sys/libnspire/src"
CPP="$ROOT/nlink/cpp"
ANDROID_USB="$ROOT/nlink/vendor/libnspire-sys/android"
QT_INCLUDE="$ROOT/qt/include"
OBJ="${TMPDIR:-/tmp}/nlink-android-objs"
rm -rf "$OBJ"
mkdir -p "$OBJ"

common_includes=(
  -I"$ANDROID_USB"
  -I"$CPP"
  -I"$QT_INCLUDE"
  -I"$NS"
  -I"$NS/services"
  -I"$NS/api"
)

c_sources=(
  "$NS/data.c"
  "$NS/error.c"
  "$NS/init.c"
  "$NS/packet.c"
  "$NS/service.c"
  "$NS/usb.c"
  "$NS/services/devinfo.c"
  "$NS/services/dir.c"
  "$NS/services/file.c"
  "$NS/services/os.c"
  "$NS/services/screenshot.c"
  "$NS/services/view.c"
)
cpp_sources=(
  "$CPP/archive.cpp"
  "$CPP/cli.cpp"
  "$CPP/dbus.cpp"
  "$CPP/device.cpp"
  "$CPP/dusb.cpp"
  "$CPP/ffi.cpp"
  "$CPP/json.cpp"
  "$CPP/nspire_err.cpp"
  "$CPP/paths.cpp"
  "$CPP/png.cpp"
  "$CPP/progress.cpp"
  "$CPP/romdump.cpp"
  "$CPP/ti8x.cpp"
  "$CPP/viewframe.cpp"
  "$NS/cx2.cpp"
)

objects=()
for src in "${c_sources[@]}"; do
  obj="$OBJ/$(basename "$src").o"
  "$CC" -fPIC -D__ANDROID__ -O2 "${common_includes[@]}" -c "$src" -o "$obj"
  objects+=("$obj")
done
for src in "${cpp_sources[@]}"; do
  obj="$OBJ/$(basename "$src").o"
  "$CXX" -fPIC -std=c++17 -D__ANDROID__ -O2 "${common_includes[@]}" -c "$src" -o "$obj"
  objects+=("$obj")
done

"$CXX" -shared -fPIC -s "${objects[@]}" \
  -Wl,--whole-archive "$RUST_LIB" -Wl,--no-whole-archive \
  -lz -ldl -lm -llog \
  -o "$OUT/libnlink.so"
rm -rf "$OBJ"

echo "Wrote $OUT/libnlink.so"
