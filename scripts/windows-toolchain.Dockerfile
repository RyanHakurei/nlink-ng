# Fedora's repositories include MinGW Qt 6, libusb, and zlib. Arch's do not.
FROM fedora:latest

RUN dnf -y install \
      bash \
      ca-certificates \
      cmake \
      curl \
      gcc \
      make \
      pkgconf \
      mingw64-filesystem \
      mingw64-gcc-c++ \
      mingw64-qt6-qtbase \
      mingw64-libusb1 \
      mingw64-zlib \
    && curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain stable --no-modify-path \
    && /root/.cargo/bin/rustup target add x86_64-pc-windows-gnu \
    && mv /root/.cargo /opt/nlink-cargo \
    && mv /root/.rustup /opt/nlink-rustup \
    && chmod -R a+rX /opt/nlink-cargo /opt/nlink-rustup \
    && dnf clean all

ENV RUSTUP_HOME=/opt/nlink-rustup
ENV PATH="/opt/nlink-cargo/bin:${PATH}"
