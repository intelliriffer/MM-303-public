#!/usr/bin/env python3
"""Generate a MockbaMod params json from a JUCE AudioProcessor source file.

Parses the APVTS constructor's make_unique<AudioParameter*> list and emits
the same schema as tools/wrap-lv2.py (knob/steps + caseLabel/variable A..).

Usage: juce_params_json.py <Processor.cpp> <name> [<preset.json>] > out.json
With a 3rd positional preset.json, the MpcPreset control params (tools/preset.json)
are appended after the real params, flagged "presetControl": true
(override_safe: false, not automatable, not serialized by the shim).
Fallback: if the source has no APVTS make_unique list (e.g. params created at
runtime from Faust zones), scans the source dir for
ui_interface->addHorizontalSlider("label", zone, init, min, max, step).
Limitation: JUCE idiom parser (make_unique list); AudioParameterInt max that
is a runtime expression is approximated 0..31 and flagged in evidence.
"""
import json
import re
import sys


def letters(i: int) -> str:
    """Return the spreadsheet-style column label for a 0-based index (A..Z, AA..)."""
    s = ""
    i += 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(ord("A") + r) + s
    return s


def strip_comments(text: str) -> str:
    """Remove // line comments and /* block */ comments from C/C++ text."""
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def apvts_block(text: str):
    """Return the body of the APVTS constructor's initializer list, or None.

    Args:
        text: comment-stripped C++ source.
    Returns:
        The text between the outer braces of the APVTS initializer, or None if
        no APVTS Identifier is present.
    """
    m = re.search(r'juce::Identifier\s*\(\s*"APVTS"\s*\)', text)
    if not m:
        return None
    brace = text.index("{", m.end())
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1 : i]
    sys.exit("unbalanced APVTS block")


FAUST_RE = re.compile(
    r'addHorizontalSlider\s*\(\s*"([^"]+)"\s*,\s*[^,]+,\s*'
    r'([^,]+),\s*([^,]+),\s*([^,]+),\s*([^)]*)\)'
)


def _f(tok: str) -> float:
    return float(tok.strip().rstrip("fF"))


def faust_params(src: str) -> dict:
    """Params created from Faust zones (FaustUIBridge::addHorizontalSlider)."""
    import os

    files = [src]
    d = os.path.dirname(src) or "."
    files += sorted(
        os.path.join(d, f) for f in os.listdir(d)
        if f.endswith(".cpp") and os.path.join(d, f) != src
    )
    for path in files:
        try:
            text = strip_comments(open(path, encoding="utf-8").read())
        except OSError:
            continue
        matches = FAUST_RE.findall(text)
        if not matches:
            continue
        params = {}
        for idx, (label, _init, lo, hi, _step) in enumerate(matches):
            params[str(idx)] = {
                "index": idx,
                "caseLabel": f"kParam{letters(idx)}",
                "name": label,
                "label": None,
                "variable": letters(idx),
                "transform": "value",
                "range": [_f(lo), _f(hi)],
                "displayExpr": letters(idx),
                "displayRange": [_f(lo), _f(hi)],
                "type": "knob",
                "displayValues": None,
                "unit": None,
                "display": {
                    "format": "raw",
                    "note": f"{label} range {_f(lo):g} {_f(hi):g} unit -",
                },
                "confidence": 1.0,
                "evidence": [
                    f'juce_params_json.py: Faust addHorizontalSlider "{label}"'
                    f" ({os.path.basename(path)})"
                ],
                "override_safe": True,
            }
        return params
    sys.exit(f"{src}: no APVTS make_unique list and no Faust "
             "addHorizontalSlider calls found")


def append_preset_controls(params: dict, preset_path: str) -> None:
    """Append the MpcPreset control params (from preset.json) to *params*.

    These are flagged ``presetControl`` / ``override_safe: false`` and are not
    serialized by the shim.
    """
    preset = json.load(open(preset_path, encoding="utf-8"))
    base = len(params)
    for j, c in enumerate(preset.get("controls") or []):
        idx = base + j
        var = letters(idx)
        lo, hi = float(c["range"][0]), float(c["range"][1])
        ptype_json = c.get("type", "knob")
        values = c.get("displayValues")
        params[str(idx)] = {
            "index": idx,
            "caseLabel": f"kParam{var}",
            "name": c["name"],
            "label": None,
            "variable": var,
            "transform": "value",
            "range": [lo, hi],
            "displayExpr": var,
            "displayRange": [lo, hi],
            "type": ptype_json,
            "displayValues": values,
            "unit": None,
            "display": {
                "format": "list" if ptype_json == "steps" else "raw",
                "note": f"{c['name']} range {lo:g} {hi:g} unit -",
            },
            "confidence": 1.0,
            "evidence": ["tools/preset.json"],
            "override_safe": False,
            "presetControl": True,
        }


def main() -> None:
    """CLI entry point: emit the params json for a processor source file."""
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    src, name = sys.argv[1], sys.argv[2]
    text = strip_comments(open(src, encoding="utf-8").read())
    block = apvts_block(text)

    if block is None:
        params = faust_params(src)
    else:
        params = {}
        chunks = re.split(r"std::make_unique\s*<\s*juce::AudioParameter", block)
        for idx, chunk in enumerate(chunks[1:], start=0):
            tm = re.match(r"(Float|Bool|Choice|Int)\s*>\s*\(\s*", chunk)
            if not tm:
                sys.exit(f"param {idx}: unknown type head: {chunk[:60]!r}")
            ptype = tm.group(1)
            rest = chunk[tm.end() :]
            ids = re.findall(r'"([^"]+)"', rest[:400])
            if len(ids) < 2:
                sys.exit(f"param {idx}: id/label not found")
            pid, label = ids[0], ids[1]
            var = letters(idx)

            if ptype in ("Float", "Int"):
                nums = re.findall(r"-?\d+\.?\d*\s*[fF]?", rest)
                if ptype == "Float" and len(nums) >= 2:
                    lo, hi = float(nums[0].rstrip("fF")), float(nums[1].rstrip("fF"))
                elif ptype == "Int":
                    lo = float(nums[0]) if nums else 0.0
                    hi = float(nums[1]) if len(nums) > 1 and "." in nums[1] else (
                        float(nums[1]) if len(nums) > 1 and nums[1].isdigit() else 31.0
                    )
                    if "loadOverdriveTones" in rest:
                        hi = 31.0
                else:
                    sys.exit(f"param {idx} ({pid}): cannot parse range")
                ptype_json, values = "knob", None
            elif ptype == "Bool":
                lo, hi = 0.0, 1.0
                ptype_json, values = "steps", ["Off", "On"]
            else:  # Choice
                arr = re.search(r"StringArray\s*\{(.*?)\}", rest, flags=re.S)
                if not arr:
                    sys.exit(f"param {idx} ({pid}): StringArray not found")
                values = re.findall(r'"([^"]+)"', arr.group(1))
                lo, hi = 0.0, float(len(values) - 1)
                ptype_json = "steps"

            params[str(idx)] = {
                "index": idx,
                "caseLabel": f"kParam{var}",
                "name": label,
                "label": None,
                "variable": var,
                "transform": "value",
                "range": [lo, hi],
                "displayExpr": var,
                "displayRange": [lo, hi],
                "type": ptype_json,
                "displayValues": values,
                "unit": None,
                "display": {
                    "format": "list" if ptype_json == "steps" else "raw",
                    "note": f"{label} range {lo:g} {hi:g} unit -",
                },
                "confidence": 1.0,
                "evidence": [f'juce_params_json.py: juce::AudioParameter{ptype} "{pid}"'],
                "override_safe": True,
            }

    if len(sys.argv) == 4:
        append_preset_controls(params, sys.argv[3])

    print(json.dumps({"name": name, "numParams": len(params),
                      "parameters": params}, indent=1))


if __name__ == "__main__":
    main()
