#!/bin/sh
# Regenerate src/p5_tables.h from the melodee firmware's generated DSP tables.
# The tables are bit-exact extractions (floats as 9g decimals, ints as decimals,
# factory programs as hex bytes), so the port and the firmware share one source
# of numeric truth. Usage: MELODEE=/path/to/melodee tools/gen_tables.sh
set -e
# WARNING: p5_tables.h carries a HAND-EXTENDED cutoff->g block (indices 128-159,
# 21 kHz headroom) that the dump does not emit. After running this, restore that
# block (git diff will show it deleted) or the cutoff cap silently reverts to 16 kHz.
cd "$(dirname "$0")/.."
MELODEE=${MELODEE:-/home/user/build/melodee}
[ -f "$MELODEE/build/gen/melodee_tables.h" ] || {
    echo "run the melodee generators first (tools/build.py generate)" >&2; exit 1; }
c99=$(mktemp "${TMPDIR:-/tmp}/p5tables.XXXXXX.c")
{
  echo '#include <stdio.h>'
  echo '#include <stdint.h>'
  echo '#include <string.h>'
  echo 'typedef struct { uint8_t raw[133]; uint8_t size, model, command, group, program; } p5_patch_t;'
  echo '#define MELODEE_DUAL_CORE 0'
  echo '#define P5_OVERSAMPLE 1'
  echo "#include \"$MELODEE/build/gen/melodee_tables.h\""
  echo "#include \"$MELODEE/build/gen/melodee_prophet_factory.h\""
  cat tools/dump_tables_body.c
} > "$c99"
cc -w -O1 -I"$MELODEE/firmware/src" "$c99" -o "$c99.bin"
"$c99.bin" > src/p5_tables.h
rm -f "$c99" "$c99.bin"
echo "wrote src/p5_tables.h ($(wc -l < src/p5_tables.h) lines)"
