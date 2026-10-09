#!/usr/bin/env python3
"""render_meta.py - fill templates/plugin-meta.xml for a package.

Usage: tools/render_meta.py --template templates/plugin-meta.xml \
           --name <N> --uid <8hex> --nin <int> --nout <int> \
           --instrument <int> --out <path>/plugin-meta.xml
Exits 1 if any {PLACEHOLDER} survives rendering.
"""
import argparse
import re
import sys
from pathlib import Path


def render(template, name, uid, nin, nout, instrument):
    """Fill the {NAME}/{UID}/{NIN}/{NOUT}/{IS_INSTRUMENT} tokens in *template*."""
    return (template
            .replace("{NAME}", name)
            .replace("{UID}", uid)
            .replace("{NIN}", str(nin))
            .replace("{NOUT}", str(nout))
            .replace("{IS_INSTRUMENT}", str(instrument)))


def has_unreplaced_placeholder(text):
    """True if any known {PLACEHOLDER} token survived rendering."""
    # Only flag our known placeholders; ignore braces in user data (name, etc.)
    return bool(re.search(r'\{(NAME|UID|NIN|NOUT|IS_INSTRUMENT)\}', text))


def main(argv=None):
    """CLI entry point: render plugin-meta.xml; returns a process exit code."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--template", required=True, type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--uid", required=True)
    ap.add_argument("--nin", type=int, default=0)
    ap.add_argument("--nout", type=int, default=2)
    # no default: every caller must pass the detected/derived value (R1/R2,
    # audit.md F1) — a default here would silently mask missing detection
    ap.add_argument("--instrument", type=int, required=True, choices=(0, 1))
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args(argv)

    try:
        text = a.template.read_text(encoding="utf-8")
    except OSError as e:
        print(f"render_meta: {e}", file=sys.stderr)
        return 1
    out = render(text, a.name, a.uid, a.nin, a.nout, a.instrument)
    if has_unreplaced_placeholder(out):
        print("render_meta: unreplaced placeholder remains", file=sys.stderr)
        return 1
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(out, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
