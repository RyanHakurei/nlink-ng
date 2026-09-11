#!/usr/bin/env bash
# Build a relocatable x86_64 AppImage of the Qt GUI/CLI.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${NLINK_VERSION:-0.5.0}"
BUILD="${ROOT}/qt/build"
STAGE="${BUILD}/appimage"
PLUGIN_FILTER="${STAGE}/qt-plugins"
QMAKE_WRAP="${STAGE}/qmake-wrapper"
OUT_DIR="${ROOT}/dist"
APPDIR="${STAGE}/AppDir"

export PATH="${HOME}/.local/opt/linuxdeploy:${HOME}/.local/bin:${PATH}"
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1
export LINUXDEPLOY_OUTPUT_VERSION="${VERSION}"

if ! command -v linuxdeploy >/dev/null 2>&1; then
  echo "linuxdeploy not found on PATH" >&2
  echo "Install linuxdeploy and linuxdeploy-plugin-qt into ~/.local/opt/linuxdeploy" >&2
  exit 1
fi

echo "==> cmake release build"
cmake -S "${ROOT}/qt" -B "${BUILD}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD}" -j"$(nproc)"

rm -rf "${STAGE}"
mkdir -p "${PLUGIN_FILTER}" "${OUT_DIR}" "${STAGE}"

QT_PLUGINS="$(qmake6 -query QT_INSTALL_PLUGINS)"

copy_plugin() {
  local rel="$1"
  local src="${QT_PLUGINS}/${rel}"
  if [[ -f "${src}" ]]; then
    mkdir -p "${PLUGIN_FILTER}/$(dirname "${rel}")"
    cp -a "${src}" "${PLUGIN_FILTER}/${rel}"
  fi
}

# Official Qt plugins only — skip KDE kimg_*/breeze/gtk so linuxdeploy
# does not try to bundle missing extras such as libjxr.so.0.
copy_plugin platforms/libqxcb.so
copy_plugin platforms/libqwayland.so
copy_plugin platforms/libqminimal.so
copy_plugin platforms/libqoffscreen.so
copy_plugin xcbglintegrations/libqxcb-glx-integration.so
copy_plugin xcbglintegrations/libqxcb-egl-integration.so
copy_plugin platforminputcontexts/libcomposeplatforminputcontextplugin.so
copy_plugin platforminputcontexts/libibusplatforminputcontextplugin.so
copy_plugin imageformats/libqgif.so
copy_plugin imageformats/libqico.so
copy_plugin imageformats/libqjpeg.so
copy_plugin imageformats/libqsvg.so
copy_plugin iconengines/libqsvgicon.so
copy_plugin platformthemes/libqxdgdesktopportal.so
copy_plugin wayland-shell-integration/libxdg-shell.so || true
copy_plugin wayland-graphics-integration-client/libqt-plugin-wayland-egl.so || true
copy_plugin wayland-decoration-client/libbradient.so || true

cat > "${QMAKE_WRAP}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
if [[ "\${1:-}" == "-query" && "\${2:-}" == "QT_INSTALL_PLUGINS" ]]; then
  printf '%s\n' "${PLUGIN_FILTER}"
  exit 0
fi
if [[ "\${1:-}" == "-query" && \$# -eq 1 ]]; then
  /usr/bin/qmake6 -query | sed "s|^QT_INSTALL_PLUGINS:.*|QT_INSTALL_PLUGINS:${PLUGIN_FILTER}|"
  exit 0
fi
exec /usr/bin/qmake6 "\$@"
EOF
chmod +x "${QMAKE_WRAP}"

cp "${ROOT}/qt/nlink-ng.desktop" "${STAGE}/nlink-ng.desktop"
cp "${ROOT}/qt/icons/icon.png" "${STAGE}/nlink-ng.png"

# Wayland helper plugins live next to the platform plugin; copy if present.
for d in wayland-shell-integration wayland-graphics-integration-client wayland-decoration-client; do
  if [[ -d "${QT_PLUGINS}/${d}" ]]; then
    mkdir -p "${PLUGIN_FILTER}/${d}"
    find "${QT_PLUGINS}/${d}" -maxdepth 1 -name 'lib*.so' -exec cp -a {} "${PLUGIN_FILTER}/${d}/" \;
  fi
done

export QMAKE="${QMAKE_WRAP}"
cd "${STAGE}"

echo "==> linuxdeploy"
linuxdeploy \
  --appdir "${APPDIR}" \
  --executable "${BUILD}/n-link" \
  --desktop-file "${STAGE}/nlink-ng.desktop" \
  --icon-file "${STAGE}/nlink-ng.png" \
  --icon-filename nlink-ng \
  --library /usr/lib/libusb-1.0.so.0 \
  --plugin qt \
  --output appimage

shopt -s nullglob
built=( "${STAGE}"/*.AppImage )
if [[ ${#built[@]} -eq 0 ]]; then
  echo "linuxdeploy did not produce an AppImage" >&2
  exit 1
fi

dest="${OUT_DIR}/nlink-ng-${VERSION}-x86_64.AppImage"
cp -f "${built[0]}" "${dest}"
chmod +x "${dest}"
echo "==> ${dest}"
ls -lh "${dest}"
file "${dest}"
