# nlink-ng

Native Qt 6 linking program for the TI-Nspire (CX-II included).

USB protocol code is Rust (`nlink/`). The GUI is Qt Widgets (`qt/`). The same `n-link` binary is the CLI.

## Build

You need a Rust toolchain, CMake, a C/C++ compiler, Qt 6 Widgets, and libusb.

```bash
# Debian/Ubuntu 22.04+
sudo apt install cmake g++ qt6-base-dev libusb-1.0-0-dev pkg-config

cmake -S qt -B qt/build -DCMAKE_BUILD_TYPE=Release
cmake --build qt/build
```

### Arch Linux

```bash
cd dist/arch
makepkg -si
```

This installs the `nlink-ng` package (`/usr/bin/n-link`), a desktop entry, and udev rules for unprivileged USB access. It provides and conflicts with `n-link`.

```bash
./qt/build/n-link                 # GUI
./qt/build/n-link ls /            # list calculator root
./qt/build/n-link download /Examples ./backup
./qt/build/n-link rm /some/file.tns
```

On Linux, udev rules (installed by the Arch package, or see the original [n-link Linux notes](https://lights0123.com/n-link/#linux)) are required for unprivileged USB access.

## CLI

```
n-link upload <files...> <dest>
n-link download <files-or-dirs...> <dest>
n-link upload-os <file>
n-link copy <from> <to>
n-link move <from> <to>
n-link mkdir <path>
n-link rmdir <path>
n-link rm <files...>
n-link ls <path>
n-link license
```

## Layout

| Path | Role |
|---|---|
| `nlink/` | Rust crate: libnspire/USB, CLI, C FFI |
| `qt/` | Qt 6 Widgets GUI |
