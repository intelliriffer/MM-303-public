#!/usr/bin/env python3
"""gen_layout_from_params.py - build a basic TUI layout.conf from MM-303.json.

Reads the plugin params json (default: output/<pkg>/MM-303.json) and writes
TUI-templates/layout.conf with 16 controls per tab (8 columns x 2 rows), all
using knob.json.  The four shim controls (Prog#, Load, Save, Status) are
skipped unless --all is given.

Usage: tools/gen_layout_from_params.py [--params F] [--out F] [--tab-image IMG]
                                        [--template knob.json] [--all]
"""
import argparse
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

COLS, ROWS = 8, 2
X0, Y0 = 60, 120
DX, DY = 150, 260
SHIM = {"Prog#", "Load", "Save", "Status"}


def safe(name):
    return name.replace(" ", "_")


def default_params():
    """Return the default params-json path (packaged output first)."""
    # Prefer output/<pkg>/MM-303.json (packaged), then output/MM-303.json.
    for p in sorted((ROOT / "output").glob("*/MM-303.json")):
        return str(p)
    return str(ROOT / "output" / "MM-303.json")


def main():
    """CLI entry point: write a starter TUI-templates/layout.conf."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--params", default=default_params())
    ap.add_argument("--out", default=str(ROOT / "TUI-templates" / "layout.conf"))
    ap.add_argument("--tab-image", default="main.png")
    ap.add_argument("--template", default="knob.json")
    ap.add_argument("--all", action="store_true",
                    help="include the shim controls too")
    a = ap.parse_args()

    data = json.load(open(a.params))
    params = sorted(data["parameters"].values(), key=lambda p: p["index"])
    if not a.all:
        params = [p for p in params if p["name"] not in SHIM]

    per = COLS * ROWS
    lines = ["# Auto-generated from %s by tools/gen_layout_from_params.py"
             % pathlib.Path(a.params).name,
             "# <index> <name> <x> <y> <template> <qlink>",
             "# qlink: 0/-1 = no Q-Link; 1..16 = the slot that control appears in"]

    for ti in range(0, len(params), per):
        page = params[ti:ti + per]
        lines.append('')
        lines.append('TAB "Page %d" "%s"' % (ti // per + 1, a.tab_image))
        for k, p in enumerate(page):
            x = X0 + (k % COLS) * DX
            y = Y0 + (k // COLS) * DY
            qlink = k + 1 if k < 16 else 0   # default: order within the tab
            lines.append("%d %s %d %d %s %d"
                         % (p["index"], safe(p["name"]), x, y, a.template, qlink))

    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print(f"wrote {out} ({len(params)} params, "
          f"{(len(params) + per - 1) // per} tabs)")


if __name__ == "__main__":
    main()
