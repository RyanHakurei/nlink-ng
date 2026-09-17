#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/.local/android-sdk}"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_SDK_ROOT/ndk/27.0.12077973}"
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
# Prefer user rustup if present.
if [[ -f "$HOME/.cargo/env" ]]; then
  # shellcheck disable=SC1091
  source "$HOME/.cargo/env"
fi
cd "$ROOT/nlink"
cargo ndk -t arm64-v8a -o "$ROOT/dist/android/android/app/src/main/jniLibs" build --release
echo "Wrote libnlink.so under dist/android/android/app/src/main/jniLibs/arm64-v8a/"
