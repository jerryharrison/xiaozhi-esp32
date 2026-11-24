#!/usr/bin/env bash
# Host checks for the Porthole's partial redraws and volume arc. Usage: tools/check.sh [avatar-render dir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
AR="${1:-$HERE/../../../../../../avatar-render}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CC="${CC:-cc}"
for t in dirty_check arc_check; do
  "$CC" -O2 -Wall -I"$AR/include" -I"$AR/src" -I"$HERE/.." "$HERE/$t.c" "$AR/src/avatar.c" -lm -o "$OUT/$t"
  echo "== $t"; "$OUT/$t"
done
# Cards: needs cJSON (managed_components after one firmware build, or CJSON_DIR).
CJ="${CJSON_DIR:-$HERE/../../../../managed_components/espressif__cjson/cJSON}"
if [ -f "$CJ/cJSON.c" ]; then
  CXX="${CXX:-c++}"
  "$CC" -O2 -c "$CJ/cJSON.c" -o "$OUT/cjson.o"
  "$CC" -O2 -I"$AR/include" -I"$AR/src" -c "$AR/src/avatar.c" -o "$OUT/avatar.o"
  "$CXX" -std=c++17 -O2 -Wall -I"$AR/include" -I"$AR/src" -I"$HERE/.." -I"$CJ" "$HERE/card_check.cc" "$OUT/avatar.o" "$OUT/cjson.o" -o "$OUT/card_check"
  echo "== card_check"; "$OUT/card_check"
else
  echo "== card_check skipped: no cJSON at $CJ (build the firmware once, or set CJSON_DIR)"
fi
# The pre-fix arc must fail the arc check (guards the check itself).
"$CC" -O2 -DPORTHOLE_ARC_LEGACY=1 -I"$AR/include" -I"$AR/src" -I"$HERE/.." "$HERE/arc_check.c" "$AR/src/avatar.c" -lm -o "$OUT/arc_legacy"
if "$OUT/arc_legacy" > /dev/null; then echo "arc_check passed the legacy arc: the check is broken"; exit 1; fi
echo "== legacy arc correctly fails"
