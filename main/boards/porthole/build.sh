#!/usr/bin/env bash
# Build the Skipper Porthole firmware: build-porthole/merged-binary.bin (+ xiaozhi.bin, generated_assets.bin).
# Run from anywhere. Env: IDF_PATH (default ~/esp/esp-idf-v6.0.3), AVATAR_RENDER_DIR (default: the
# skipper-ai checkout this submodule lives in, ../../avatar-render), BUILD_DIR (default build-porthole).
set -euo pipefail
FW="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$FW"
if [ -z "${IDF_PATH:-}" ] || ! command -v idf.py >/dev/null 2>&1; then
  . "${IDF_PATH:-$HOME/esp/esp-idf-v6.0.3}/export.sh" >/dev/null
fi
BUILD_DIR="${BUILD_DIR:-build-porthole}"
idf.py -B "$BUILD_DIR" -D IDF_TARGET=esp32s3 -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;main/boards/porthole/sdkconfig.porthole" build merge-bin
