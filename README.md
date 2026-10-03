# nlink-ng

Native Qt 6 linking program for TI-Nspire, TI-83/84 (USB and SilverLink), TI-84 Plus CE, and TI-84 Evo.

83/84, CE, and Evo transfers follow the published link protocols. They have not been verified against a physical calculator in this tree. Nspire behavior is unchanged.

Nspire, TI-83/84, and SilverLink transfers are C++ (`nlink/cpp/`). TI-84 Evo transfers stay in Rust (`nlink/src/link/evo`). The GUI is Qt Widgets (`qt/`). The same `n-link` binary is the CLI.

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

### Windows

`scripts/build-windows.sh` builds a 64-bit Windows `n-link.exe` and the Qt and libusb DLLs it needs, in `testing/windows/`. On Linux without a MinGW compiler the script does that build inside Docker. Double-click `n-link.exe` to open the window. From Command Prompt:

```bat
n-link.exe ls /
```

The calculator shows up after its USB driver is WinUSB. Install that with [Zadig](https://zadig.akeo.ie/) for the TI device (0451:e012, e022, e001, e003, e008, or e018). An empty device list means nothing is plugged in.

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
| `nlink/` | C++ core, plus the Rust TI-84 Evo protocol |
| `qt/` | Qt 6 Widgets GUI and the `n-link` binary |
