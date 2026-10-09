#!/usr/bin/env python3
"""gen_tui.py - write the Force version.xml for a package.

The custom TUI.json is no longer generated here (the skin is produced
separately via tools/gen_tui_layout.py from TUI-templates/).  This only writes
version.xml so the Force binds the plugin to the package.

Usage: tools/gen_tui.py <Name> --version X.Y.Z --package-dir DIR
"""
import argparse
import pathlib


def main():
    """CLI entry point: write a package's Force version.xml."""
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--version", default="1.0.0.0")
    ap.add_argument("--package-dir", required=True)
    ap.add_argument("--params-json")           # accepted, unused (kept for CLI compat)
    ap.add_argument("--NOTUI", action="store_true")  # accepted, unused
    a = ap.parse_args()

    out = pathlib.Path(a.package_dir)
    (out / "Plugin Skins").mkdir(parents=True, exist_ok=True)

    ver4 = a.version if a.version.count(".") >= 3 else f"{a.version}.0"
    (out / "version.xml").write_text(
        "<?xml version='1.0' encoding='utf-8'?>\n"
        '<plugincontent version="1.0">\n'
        f"\t<identifier>mockbamod.vst.{a.name.lower()}</identifier>\n"
        f"\t<version>{ver4}</version>\n"
        "</plugincontent>")
    print(f"{a.name}: version.xml written ({ver4})")


if __name__ == "__main__":
    main()
