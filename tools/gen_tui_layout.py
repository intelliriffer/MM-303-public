#!/usr/bin/env python3
"""gen_tui_layout.py - build a Force TUI json from layout.conf + templates.

Inputs (all under TUI-templates/, none modified):
  layout.conf   - pages:  TAB "name" "image"
                          <index> <name> <x> <y> <component-template.json>
  TUI.json      - page shell; contains the literal token  {tabs}
  tabs.json     - one tab prototype with {tabname} and {tabimage}
  <template>.json - component prototypes with {name} {index} {x} {y}
  preset.json   - Prog#/Load/Save controls, injected into the FIRST tab
                  ({prog}/{load}/{save} filled from plugin/MM-303.json)

Output:
  output/TUI.json  (validated JSON; use --out to change)

Usage: tools/gen_tui_layout.py [--layout F] [--out F]

Reports ALL layout problems (unparseable lines, missing templates, unknown
component types) at once and exits non-zero instead of stopping on the first.
"""
import copy
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TPL = ROOT / "TUI-templates"
BUILTIN_TYPES = {"Focus", "Knob", "Button", "Label", "Image", "Value",
                 "Slider", "ComboBox"}


def load_json(path):
    """Load and return the JSON document at *path*."""
    with open(path) as f:
        return json.load(f)


def param_index_by_name(params_path, name):
    """Return the VST parameter index whose display name is *name*.

    Args:
        params_path: path to a params json (e.g. plugin/MM-303.json).
        name: parameter display name, e.g. "Prog#".
    Returns:
        The integer parameter index.
    Raises:
        SystemExit: if the name is not present.
    """
    data = load_json(params_path)
    for p in data.get("parameters", {}).values():
        if p.get("name") == name:
            return p["index"]
    raise SystemExit(f"param {name!r} not found in {params_path}")


def sub_strings(obj, mapping):
    """Recursively replace {token} substrings in a JSON-like object.

    Args:
        obj: dict/list/str/scalar to walk.
        mapping: {token: replacement} applied to every string.
    Returns:
        A new object of the same shape with tokens substituted.
    """
    if isinstance(obj, dict):
        return {k: sub_strings(v, mapping) for k, v in obj.items()}
    if isinstance(obj, list):
        return [sub_strings(v, mapping) for v in obj]
    if isinstance(obj, str):
        for key, val in mapping.items():
            obj = obj.replace(key, val)
        return obj
    return obj


# Q-Link assignment: per tab, Q-Link = (bank-1)*cols+col
# where banks are rows bottom->top and cols are x clusters left->right. The
# Force otherwise falls back to the plugin's global parameter order.
QLINK_NON_KNOB = {"progSelect", "progLoad", "progSave", "greenLed", "Image"}


def _tab_knobs(tab):
    """Return (x, y, param) for each knob-like component in a built tab."""
    out = []
    for c in tab["componentDefinition"]["componentsData"]:
        ty = c["componentData"]["type"]
        if ty in QLINK_NON_KNOB:
            continue
        for e in c.get("handle remapping", {}).get("map", []):
            v = str(e.get("value", ""))
            if v.startswith("Parameter "):
                b = c["bounds"]["bounds"].split()
                out.append((int(b[0]), int(b[1]), int(v.split()[1])))
    return out


def _force_slot_inv(d):
    """Inverse of the Force's Q-Link slot remap.

    The Force maps our "Q-Link n" to physical slot
        slot(n) = ((n-1) % 4) * 4 + (4 - (n-1) // 4)   # transpose + v-flip
    (device-verified).  To make a param land on physical slot `d`, write it at
    Q-Link `_force_slot_inv(d)`.
    """
    q = d - 1
    c = q // 4
    r = 3 - (q % 4)
    return r * 4 + c + 1


def qlinks_for_tab(ql):
    """ql = [(param, qlink)] from layout.conf (6th field).

    qlink = 0/-1 -> off; else the physical slot that control should occupy on
    the device.  Entries are written in APPEARANCE order (slot 1 first), each
    placed at the Force's remapped key so it lands on the intended slot -- so
    the file reads top-to-bottom in the order the controls show on screen.
    """
    by_slot = {}
    for param, slot in ql:
        if 1 <= slot <= 16:
            by_slot[slot] = param

    q = {}
    for slot in range(1, 17):
        q[f"Q-Link {_force_slot_inv(slot)}"] = by_slot.get(slot, -1)
    return q


def build_qlinks(tab_qlinks):
    """Build the Q-Links.json document from per-tab (param, slot) lists.

    Args:
        tab_qlinks: list (per tab) of [(param_index, qlink_slot), ...].
    Returns:
        The complete Q-Links.json dict (version 4, Screen + Program modes).
    """
    qmap = [{"Tab": ti, "SubTab": 1, "Bank Direction": "Column",
             "Q-Links": qlinks_for_tab(ql)}
            for ti, ql in enumerate(tab_qlinks, start=1)]
    prog = qmap[0]["Q-Links"] if qmap else {}
    return {"version": 4,
            "info": {"version": 1, "type": "CompleteDescription"},
            "Screen Mode Q-Links": {"version": 4, "map": qmap},
            "Program Mode Q-Links": prog}


def parse_layout(path):
    """Return (tabs, errors)."""
    tabs, errors, cur = [], [], None
    for ln, raw in enumerate(open(path), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        m = re.match(r'^TAB\s+"([^"]*)"\s+"([^"]*)"(?:\s+(\S+))?', line)
        if m:
            cur = {"name": m.group(1), "image": m.group(2),
                   # optional 3rd token: "shim"/"preset" routes the Prog#/Load/
                   # Save bar onto this tab (default: first tab).
                   "shim": (m.group(3) or "").lower() in ("shim", "preset"),
                   "components": []}
            tabs.append(cur)
            continue
        m = re.match(r'^(\d+)\s+(\S+)\s+(-?\d+)\s+(-?\d+)\s+(\S+)(?:\s+(-?\d+))?\s*$', line)
        if m:
            if cur is None:
                errors.append(f"line {ln}: component before any TAB: {line!r}")
            else:
                # optional 6th field: Q-Link order (0/-1 = off, else the slot)
                qlink = int(m.group(6)) if m.group(6) is not None else -1
                if qlink < -1 or qlink > 16:
                    errors.append(f"line {ln}: qlink {qlink} out of range (-1..16)")
                cur["components"].append({
                    "index": int(m.group(1)), "name": m.group(2),
                    "x": int(m.group(3)), "y": int(m.group(4)),
                    "template": m.group(5), "qlink": qlink})
            continue
        errors.append(f"line {ln}: cannot parse: {line!r}")
    return tabs, errors


def check_instance_type(template, instance, def_keys, c, errors):
    """A combined template's instance type must be one of its own def keys."""
    ty = instance.get("componentData", {}).get("type")
    if ty not in def_keys:
        errors.append(f"{template}: instance type {ty!r} is not one of this "
                      f"template's definition keys {def_keys} "
                      f"(param {c['index']} {c['name']})")


def main():
    """CLI entry point: generate TUI.json (+ Q-Links.json) from layout.conf."""
    layout = TPL / "layout.conf"
    out = ROOT / "output" / "TUI.json"
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--layout":
            layout = pathlib.Path(args[i + 1])
        elif a == "--out":
            out = pathlib.Path(args[i + 1])

    tabs_proto = load_json(TPL / "tabs.json")
    shell = (TPL / "TUI.json").read_text()
    if "{tabs}" not in shell:
        sys.exit("TUI.json is missing the {tabs} token")

    tabs, errors = parse_layout(layout)

    # --- resolve templates once; collect inline component definitions ------
    # A "combined" template is a single JSON object
    #   { "definitions": [ {key,value}, ... ], "component": {instance},
    #     "importFiles": [ ... ] }
    # The instance is placed in the tab; the definitions are registered in the
    # package TUI.json's localComponentDefinitions, so a one-off control can live
    # in one file. A plain template (an instance object) is used as-is.
    tpl_cache = {}
    local_defs = []
    import_files = []
    for c in (c for t in tabs for c in t["components"]):
        p = TPL / c["template"]
        if not p.is_file():
            errors.append(f"missing template: {c['template']} "
                          f"(param {c['index']} {c['name']})")
            continue
        if p in tpl_cache:
            continue
        obj = load_json(p)
        if isinstance(obj, dict) and "component" in obj:
            # { "definitions": [ {key,value}, ... ], "component": {...} } form
            inst = obj["component"]
            defs = obj.get("definitions", [])
            tpl_cache[p] = inst
            local_defs.extend(defs)
            import_files.extend(obj.get("importFiles", []))
            check_instance_type(c["template"], inst, [d.get("key") for d in defs],
                                c, errors)
        elif (isinstance(obj, dict) and "key" in obj and "value" in obj
              and ("componentData" in obj or "bounds" in obj)):
            # single-object form: the def's key/value beside the instance keys
            inst = {k: v for k, v in obj.items()
                    if k not in ("key", "value", "importFiles")}
            tpl_cache[p] = inst
            local_defs.append({"key": obj["key"], "value": obj["value"]})
            import_files.extend(obj.get("importFiles", []))
            check_instance_type(c["template"], inst, [obj["key"]], c, errors)
        else:
            tpl_cache[p] = obj

    assets_path = ROOT / "skin" / "assets.json"
    assets_keys = set()
    if assets_path.exists():
        assets_keys = {x["key"] for x in
                       load_json(assets_path)["componentDefinitions"]["localComponentDefinitions"]}

    # One-off definitions (from combined templates) must have unique keys and
    # must not shadow a component already defined in skin/assets.json.
    local_def_keys = [d.get("key") for d in local_defs]
    seen_def_keys = set()
    for k in local_def_keys:
        if k is None:
            errors.append("combined template definition is missing a 'key'")
        elif k in seen_def_keys:
            errors.append(f"duplicate component definition key {k!r} "
                          f"(same key used by two templates)")
        elif k in assets_keys:
            errors.append(f"component definition key {k!r} collides with skin/assets.json")
        seen_def_keys.add(k)

    known = set(BUILTIN_TYPES) | {k for k in local_def_keys if k} | assets_keys

    # --- build ------------------------------------------------------------
    built_tabs, ncomp = [], 0
    tab_qlinks = []   # per tab: list of (param, qlink-order) from layout.conf
    for ti, tab in enumerate(tabs):
        comps = []
        ql = []
        for c in tab["components"]:
            tpl_file = TPL / c["template"]
            if tpl_file not in tpl_cache:
                continue  # missing / already reported
            comp = sub_strings(tpl_cache[tpl_file], {
                "{name}": c["name"], "{index}": str(c["index"]),
                "{x}": str(c["x"]), "{y}": str(c["y"])})
            ty = comp.get("componentData", {}).get("type")
            if ty not in known:
                errors.append(f"unknown component type {ty!r} "
                              f"(param {c['index']} {c['name']}, {c['template']})")
            comps.append(comp)
            for e in comp.get("handle remapping", {}).get("map", []):
                v = str(e.get("value", ""))
                if v.startswith("Parameter "):
                    ql.append((int(v.split()[1]), c["qlink"]))
        tab_qlinks.append(ql)
        seen = {}
        for param, slot in ql:
            if 1 <= slot <= 16:
                if slot in seen:
                    errors.append(f"tab {ti+1}: qlink {slot} used twice "
                                  f"(params {seen[slot]} and {param})")
                seen[slot] = param
        ncomp += len(comps)
        t = sub_strings(copy.deepcopy(tabs_proto),
                        {"{tabname}": tab["name"], "{tabimage}": tab["image"]})
        t["fnKeyIndex"] = ti
        t["componentDefinition"]["componentsData"] = comps
        built_tabs.append(t)

    # Preset controls (Prog# / Load / Save) on the FIRST tab.
    preset_path = TPL / "preset.json"
    params_path = ROOT / "plugin" / "MM-303.json"
    if built_tabs and preset_path.exists() and params_path.exists():
        preset = sub_strings(load_json(preset_path), {
            "{prog}": str(param_index_by_name(params_path, "Prog#")),
            "{load}": str(param_index_by_name(params_path, "Load")),
            "{save}": str(param_index_by_name(params_path, "Save"))})
        # Inject the preset bar into every tab flagged "shim" (or the first tab
        # if none is flagged); one deep copy per tab.
        targets = [i for i, tb in enumerate(tabs) if tb.get("shim")] or [0]
        for ti in targets:
            built_tabs[ti]["componentDefinition"]["componentsData"][:0] = copy.deepcopy(preset)
            ncomp += len(preset)

    if errors:
        print(f"{len(errors)} problem(s):", file=sys.stderr)
        for e in errors:
            print("  -", e, file=sys.stderr)
        sys.exit(1)

    data = json.loads(shell.replace("{tabs}", json.dumps(built_tabs)))
    if local_defs or import_files:
        cdef = data["pageData"]["componentDefinitions"]
        cdef["localComponentDefinitions"] = local_defs + cdef.get("localComponentDefinitions", [])
        if import_files:
            cdef["importFiles"] = cdef.get("importFiles", []) + import_files
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        json.dump(data, f, indent=2)
    print(f"wrote {out} ({len(built_tabs)} tabs, {ncomp} components)")

    ql_out = out.with_name("Q-Links.json")
    with open(ql_out, "w") as f:
        json.dump(build_qlinks(tab_qlinks), f, indent=2)
    print(f"wrote {ql_out}")


if __name__ == "__main__":
    main()
