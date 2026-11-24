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
# The pre-fix arc must fail the arc check (guards the check itself).
"$CC" -O2 -DPORTHOLE_ARC_LEGACY=1 -I"$AR/include" -I"$AR/src" -I"$HERE/.." "$HERE/arc_check.c" "$AR/src/avatar.c" -lm -o "$OUT/arc_legacy"
if "$OUT/arc_legacy" > /dev/null; then echo "arc_check passed the legacy arc: the check is broken"; exit 1; fi
echo "== legacy arc correctly fails"
