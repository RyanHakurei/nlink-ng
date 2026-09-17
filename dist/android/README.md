# nlink-ng Android (Flutter)

Linux desktop stays **Qt**. This tree is the Android Flutter shell.

It talks to the same `nlink` Rust/libnspire engine via `libnlink.so`. USB uses Android USB-host (`UsbManager`) and native `USBDEVFS_BULK` ioctls — not libusb device discovery.

## Features

- Material You (`dynamic_color`) when the OS provides a wallpaper palette
- Detect TI-Nspire / CX II, request USB permission, connect
- Browse folders, upload, download, mkdir, rename, delete
- Screenshot, exit exam/Press-to-Test mode

OTG / USB-host hardware required. arm64-v8a only.

## Build

```bash
export JAVA_HOME="$HOME/.local/opt/jdk-17.0.16+8"   # or jdk17-openjdk
export ANDROID_SDK_ROOT="$HOME/.local/android-sdk"
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/27.0.12077973"
export PATH="$HOME/.local/opt/flutter/bin:$JAVA_HOME/bin:$PATH"

# native engine
dist/android/build-native.sh

cd dist/android
flutter build apk --release
# → build/app/outputs/flutter-apk/app-release.apk
```

Install: `adb install -r build/app/outputs/flutter-apk/app-release.apk`
