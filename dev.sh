#!/bin/bash
set -e

lumina_perf_logging=OFF

case "${1:-}" in
  "")
    ;;
  performance)
    lumina_perf_logging=ON
    ;;
  *)
    echo "Usage: $0 [performance]" >&2
    exit 2
    ;;
esac

if [ "$#" -gt 1 ]; then
  echo "Usage: $0 [performance]" >&2
  exit 2
fi

cmake -S . -B build \
  -DBUILD_DOCS=OFF \
  -DLUMINA_ENABLE_STREAM_PERF_LOGGING="$lumina_perf_logging"
npm run build-clean
cmake --build build --target sunshine --parallel 4

INSTALL_DIR="$HOME/.local/share/lumina"
mkdir -p "$INSTALL_DIR/assets/web"
cp -f build/lumina "$INSTALL_DIR/lumina"
cp -Rf build/assets/web/. "$INSTALL_DIR/assets/web/"

codesign --sign - --entitlements "$INSTALL_DIR/hid_entitlements.plist" --force "$INSTALL_DIR/lumina"
codesign --sign - --force "$INSTALL_DIR/vd_helper"
