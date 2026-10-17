#!/usr/bin/env python3
"""Diff a hand-edited panel SVG against the generated base, in LAYOUT UNITS.

Workflow: open muhammad_panel.svg in Inkscape, drag whole groups (Tab selects
groups; the Objects dock Ctrl+Shift+O names them by data-param), resize boxes,
File > Save As > Plain SVG, then:

    python3 tools/svg_diff.py muhammad_panel.svg your_edits.svg

Read the reported deltas as INTENT (weighting / gutters / margin balance), not
as literal coordinates to paste into the generator — the generator owns the
grid and stays parametric.
"""
import re
import sys
import xml.etree.ElementTree as ET

TR = re.compile(r"translate\(\s*(-?[\d.]+)[ ,]+(-?[\d.]+)")
MX = re.compile(r"matrix\(\s*(-?[\d.]+)[ ,]+(-?[\d.]+)[ ,]+(-?[\d.]+)[ ,]+(-?[\d.]+)[ ,]+(-?[\d.]+)[ ,]+(-?[\d.]+)")
NUM = lambda el, a: float(el.get(a, 0) or 0)

def tag(e):
    return e.tag.rsplit('}', 1)[-1]

def translate_of(el):
    t = el.get("transform", "")
    m = TR.search(t)
    if m:
        return float(m.group(1)), float(m.group(2))
    m = MX.search(t)
    if m:
        return float(m.group(5)), float(m.group(6))
    return 0.0, 0.0

def collect(path):
    """key -> kind, position, and (for boxes) size. Text nodes are keyed by
    their content (with an occurrence index if duplicated) at their absolute
    layout position — captions the user drags independently of their control
    are part of the edit."""
    out = {}
    tcount = {}
    tree = ET.parse(path)
    def walk(el, ox, oy):
        dx, dy = translate_of(el)
        x, y = ox + dx, oy + dy
        cls = el.get("class", "")
        if tag(el) == "g" and ("knob" in cls or "button" in cls or "display" in cls):
            key = el.get("data-param") or el.get("data-box")
            if key:
                out[key] = dict(kind="ctl", x=x, y=y)
        if tag(el) == "rect" and "box" in cls:
            key = el.get("data-box")
            if key:
                # boxes are placed by their own x/y attributes (inside the
                # group's accumulated translate), not by a group transform
                out[key] = dict(kind="box", x=x + NUM(el, "x"), y=y + NUM(el, "y"),
                                w=NUM(el, "width"), h=NUM(el, "height"))
        if tag(el) == "text" and (el.text or "").strip():
            content = " ".join((el.text or "").split())
            i = tcount.get(content, 0)
            tcount[content] = i + 1
            key = f"TEXT[{content}]" if i == 0 else f"TEXT[{content}]#{i}"
            out[key] = dict(kind="txt", x=x + NUM(el, "x"), y=y + NUM(el, "y"),
                            w=0.0, h=0.0)
        for ch in el:
            walk(ch, x, y)
    walk(tree.getroot(), 0.0, 0.0)
    return out

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base, edit = collect(sys.argv[1]), collect(sys.argv[2])
    only_base = sorted(set(base) - set(edit))
    only_edit = sorted(set(edit) - set(base))
    print(f"{'element':<34} {'kind':<5} {'dx':>7} {'dy':>7} {'dw':>7} {'dh':>7}")
    moved = 0
    for k in sorted(set(base) & set(edit)):
        b, e = base[k], edit[k]
        dx, dy = e["x"] - b["x"], e["y"] - b["y"]
        dw = dh = 0.0
        if b["kind"] == "box":
            dw, dh = e["w"] - b["w"], e["h"] - b["h"]
        if max(abs(dx), abs(dy), abs(dw), abs(dh)) < 1.5:
            continue
        moved += 1
        print(f"{k:<34} {b['kind']:<5} {dx:>+7.1f} {dy:>+7.1f} {dw:>+7.1f} {dh:>+7.1f}")
    if not moved:
        print("(no differences over 1.5 layout px)")
    # gutter report for boxes on the same row (top-edge within 12px)
    boxes = {k: v for k, v in base.items() if v["kind"] == "box"}
    eboxes = {k: v for k, v in edit.items() if v["kind"] == "box"}
    print("\nrow gutters that changed (right-edge to next left-edge):")
    rows = {}
    for k, v in boxes.items():
        rows.setdefault(round(v["y"] / 24), []).append((v["x"], v["x"] + v["w"], k))
    shown = False
    for _, items in sorted(rows.items()):
        items.sort()
        for (x1, r1, k1), (x2, r2, k2) in zip(items, items[1:]):
            g_base = x2 - r1
            if k1 in eboxes and k2 in eboxes:
                g_edit = eboxes[k2]["x"] - (eboxes[k1]["x"] + eboxes[k1]["w"])
                if abs(g_edit - g_base) >= 1.5:
                    print(f"  {k1} -> {k2}:  {g_base:.0f} -> {g_edit:.0f}")
                    shown = True
    if not shown:
        print("  (none)")
    for k in only_edit:
        print(f"ADDED in edit: {k}")
    for k in only_base:
        print(f"REMOVED in edit: {k}")

if __name__ == "__main__":
    main()
