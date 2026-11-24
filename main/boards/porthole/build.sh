#!/usr/bin/env bash
# Build the Skipper Porthole firmware: build-porthole/merged-binary.bin (+ xiaozhi.bin, generated_assets.bin).
# Run from anywhere. Env: IDF_PATH (default ~/esp/esp-idf-v6.0.3), AVATAR_RENDER_DIR (default: the
# skipper-ai checkout this submodule lives in, ../../avatar-render), BUILD_DIR (default build-porthole).
set -euo pipefail
FW="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$FW"
# ESP-IDF's export.sh uses the first python3 on PATH and wants a venv built for that exact version
# (~/.espressif/python_env/idf<IDF major.minor>_py<X.Y>_env). If Homebrew moves python3 ahead (e.g.
# to 3.15), put a python3/python shim for a version that has one first on PATH instead of reinstalling IDF.
idf_python_shim() {
  local tools="${IDF_TOOLS_PATH:-$HOME/.espressif}" vf="${IDF_PATH:-$HOME/esp/esp-idf-v6.0.3}/tools/cmake/version.cmake" idf p v shim
  idf="$(sed -n 's/^set(IDF_VERSION_MAJOR \([0-9]*\))$/\1/p' "$vf" 2>/dev/null).$(sed -n 's/^set(IDF_VERSION_MINOR \([0-9]*\))$/\1/p' "$vf" 2>/dev/null)"
  for p in python3 python3.12 python3.13 python3.11 python3.10; do
    command -v "$p" >/dev/null 2>&1 || continue
    v=$("$p" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null) || continue
    [ -d "$tools/python_env/idf${idf}_py${v}_env" ] || continue
    [ "$p" = python3 ] && return 0
    shim="$(mktemp -d "${TMPDIR:-/tmp}/idf-python.XXXXXX")"
    ln -s "$(command -v "$p")" "$shim/python3"; ln -s "$(command -v "$p")" "$shim/python"
    export PATH="$shim:$PATH"
    return 0
  done
}
idf_python_shim
if [ -z "${IDF_PATH:-}" ] || ! command -v idf.py >/dev/null 2>&1; then
  . "${IDF_PATH:-$HOME/esp/esp-idf-v6.0.3}/export.sh" >/dev/null
fi
BUILD_DIR="${BUILD_DIR:-build-porthole}"
idf.py -B "$BUILD_DIR" -D IDF_TARGET=esp32s3 -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;main/boards/porthole/sdkconfig.porthole" build merge-bin
