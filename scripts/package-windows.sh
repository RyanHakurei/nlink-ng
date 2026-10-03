#!/usr/bin/env bash
# Build the self-extracting Windows installer from testing/windows.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${ROOT}/testing/windows"
OUT="${ROOT}/dist/nlink-ng-1.0.0-windows-x64.exe"
NSIS="${NLINK_NSIS:-/tmp/nsis-3.11}"
MAKENSIS="${NSIS}/Bin/makensis.exe"

if [[ ! -f "${SOURCE}/n-link.exe" ]]; then
  echo "missing ${SOURCE}/n-link.exe — run scripts/build-windows.sh first" >&2
  exit 1
fi
if [[ ! -f "${MAKENSIS}" ]]; then
  echo "makensis not found at ${MAKENSIS}" >&2
  exit 1
fi

mkdir -p "${ROOT}/dist"
SRC_WIN="$(winepath -w "${SOURCE}")"
OUT_WIN="$(winepath -w "${OUT}")"
NSI_WIN="$(winepath -w "${ROOT}/scripts/nlink-ng.nsi")"

wine "${MAKENSIS}" "/DSOURCE=${SRC_WIN}" "/DOUTFILE=${OUT_WIN}" "${NSI_WIN}"
echo "==> ${OUT}"
ls -lh "${OUT}"
file "${OUT}"
