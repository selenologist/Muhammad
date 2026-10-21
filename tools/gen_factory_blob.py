#!/usr/bin/env python3
"""Extract Sequential's factory wire bank from src/p5_tables.h into the
strippable sidecar (factory/muhammad-factory.p5fac) and remove the embedded
table from the header, so the COMPILED BINARY contains zero proprietary bytes.

Blob format (little-endian):
    char magic[4]  = "P5FC"
    u32  record_size (133)
    u32  count     (200)
    count * record_size raw wire bytes, program order 0..199

Program names live inside the wire bytes (73..92, P5 name field) - nothing
Sequential-owned remains in the binary. Rerunning is idempotent: if the array
is already pruned the blob is still rebuilt from the existing sidecar (verify
mode), else from the header.
"""
import re, struct, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
HDR = ROOT / "src" / "p5_tables.h"
OUT = ROOT / "factory" / "muhammad-factory.p5fac"

src = HDR.read_text()
m = re.search(r"static const uint8_t PW_FACTORY_RAW\[PW_FACTORY_N\]\[133\] = \{(.*?)\n\};",
              src, re.S)
if m:
    body = m.group(1)
    rows = re.findall(r"\{([0-9A-Fa-fx, ]+)\}", body)
    progs = []
    for r in rows:
        vals = [int(tok, 16) for tok in re.findall(r"0x[0-9A-Fa-f]{2}", r)]
        assert len(vals) == 133, f"row width {len(vals)}"
        progs.append(bytes(vals))
    assert len(progs) == 200, f"count {len(progs)}"
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_bytes(b"P5FC" + struct.pack("<II", 133, 200) + b"".join(progs))
    # prune the table + its comment block from the header
    start = src.rindex("/*", 0, m.start())
    hdr_comment = src[start:m.end() + 3]
    note = ("/* Sequential's 200 factory programs moved OUT of the binary (GPL-purity\n"
            " * ruling): they live in the strippable sidecar factory/muhammad-factory.p5fac\n"
            " * (P5FC format, see tools/gen_factory_blob.py) and load at runtime through\n"
            " * src/p5_factory.hpp. PW_FACTORY_N stays as the grid SIZE only. */")
    src = src.replace(hdr_comment, note)
    src = src.replace('#include <cstdint>', '#include <cstdint>\n#include "p5_factory.hpp"', 1)
    HDR.write_text(src)
    print(f"blob written ({OUT.stat().st_size} bytes) + header pruned")
elif OUT.exists():
    blob = OUT.read_bytes()
    magic = blob[:4]; rec, n = struct.unpack("<II", blob[4:12])
    assert magic == b"P5FC" and rec == 133 and n == 200 and len(blob) == 12 + 133 * 200
    print(f"header already pruned; sidecar verified ({n} x {rec})")
else:
    sys.exit("no PW_FACTORY_RAW table and no sidecar - repo state unexpected")
