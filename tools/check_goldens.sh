#!/usr/bin/env bash
# Golden-image gate: render the panel states headlessly and pixel-compare
# against tests/golden/*.png. Fuzz tolerates antialiasing drift between GPUs.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
tol="${TOL:-0.5%}"          # max fraction of pixels differing beyond fuzz
bin="$build/render_golden"
[ -x "$bin" ] || { echo "missing $bin — build first"; exit 2; }
command -v compare >/dev/null || { echo "ImageMagick 'compare' required"; exit 2; }

fail=0
for spec in "init 1560" "pad 936" "bass 1560"; do
    set -- $spec
    state=$1; w=$2
    h=$(( w * 416 / 1560 ))
    out=$(mktemp --suffix=.png)
    "$bin" "$out" "$state" "$w" 2>/dev/null
    golden="$root/tests/golden/panel-$state-$w.png"
    if [ ! -f "$golden" ]; then
        echo "GOLDEN-MISSING $golden (baseline run: cp $out $golden)"; fail=1
    else
        ae=$(compare -metric AE -fuzz 2% "$golden" "$out" null: 2>&1 | head -1)
        total=$(( w * h ))
        pct=$(python3 -c "print(f'{${ae:-0} / $total * 100:.3f}%')" 2>/dev/null || echo "ERR")
        ok=$(python3 -c "print(1 if float('${pct%\%}') <= 0.5 else 0)" 2>/dev/null || echo 0)
        if [ "$ok" = 1 ]; then echo "golden $state: OK ($pct diff @fuzz2%)"; else echo "golden $state: DRIFT ($pct > $tol)"; fail=1; fi
    fi
    rm -f "$out"
done
exit $fail
