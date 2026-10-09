#!/usr/bin/env bash
# MM-303 build — Akai Force / MPC VST2 package (native JUCE VST2).
#
# Builds the plugin's native VST2 target in Docker (armhf, glibc 2.31), links
# the MpcPreset shim (+4 preset controls), extracts params from the source,
# runs device gates, and packages.
#
# Output: output/MockbaMod - VST - MM-303/
#
# Usage:
#   ./build.sh              build the package
#   ./build.sh --clean      clean (image + deps + build tree + output), then ask
#                           whether to rebuild
#   ./build.sh --no-tui     skip "Plugin Skins/TUI.json" (version.xml still written)
#   ./build.sh --no-presets skip the MpcPreset shim (+4 controls)
#   ./build.sh --release    build + package, then run ./release.sh
#
# Requires on host: docker, python3, git
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD

NAME=MM-303                       # package + artifact name
PDIR=plugin                       # plugin source dir
PROC="$PDIR/src/JC303.cpp"        # source that declares the APVTS parameters
PLUGIN_CODE=Mm03                  # JUCE plugin code -> uid
DOCKERFILE=docker/Dockerfile.armhf-glibc231-audio
DOCKER_TAG=mm303:armhf-glibc231-audio
NOTUI=0
CLEAN=0
NO_PRESETS=0
RELEASE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-tui)     NOTUI=1; shift ;;
    --no-presets) NO_PRESETS=1; shift ;;
    --clean)      CLEAN=1; shift ;;
    --release)    RELEASE=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ -f recipe.sh ] && . ./recipe.sh
: "${NIN:=0}"
: "${NOUT:=2}"
: "${IS_INSTRUMENT:=1}"

U="$(id -u):$(id -g)"
mkdir -p output

# plugin version (for meta + version.xml)
VERSION=$(sed -n 's/^project([^ ]* VERSION \([0-9.]*\).*/\1/p' "$PDIR/CMakeLists.txt" | head -n1)
[ -n "$VERSION" ] || VERSION=0.0.0
# uid = hex of the JUCE plugin code (native convention)
UID_HEX=$(python3 -c "print('%08x' % int.from_bytes('$PLUGIN_CODE'.encode(),'big'))")

echo "=== MM-303 build (native VST2) ==="
echo "Target:  Akai Force / MPC (armhf, glibc 2.31)"
echo "Docker:  $DOCKER_TAG"
echo "Plugin:  $PDIR -> package $NAME  (v$VERSION, uid $UID_HEX)"
echo "Format:  VST2, io $NIN/$NOUT, isInstrument=$IS_INSTRUMENT"
echo "Presets: $([ "$NO_PRESETS" = 1 ] && echo off || echo 'on (+4 controls)')"
echo "================================="

# --- clean / docker image --------------------------------------------------
if [ "$CLEAN" = 1 ]; then
  echo "Cleaning: docker image + fetched deps + build tree + output..."
  docker image rm -f "$DOCKER_TAG" 2>/dev/null || true
  rm -rf plugin/lib plugin/build plugin/bin output
  if [ -t 0 ]; then
    printf 'Cleaned. Rebuild now? [y/N] '
    read -r _ans
    case "$_ans" in
      y|Y|yes|YES) echo "Rebuilding..." ;;
      *) echo "Done (not rebuilding). Run ./build.sh to build."; exit 0 ;;
    esac
  else
    echo "Cleaned (non-interactive; not rebuilding). Run ./build.sh to build."
    exit 0
  fi
fi
# Image: use a local one, else pull the published one, else build.
IMAGE_REPO="${MM303_IMAGE_REPO:-ghcr.io/intelliriffer/mm303-build:armhf-glibc231-audio}"
if docker image inspect "$DOCKER_TAG" >/dev/null 2>&1; then
  echo "Using local Docker image $DOCKER_TAG"
elif docker pull "$IMAGE_REPO" >/dev/null 2>&1; then
  echo "Pulled $IMAGE_REPO"
  docker tag "$IMAGE_REPO" "$DOCKER_TAG"
else
  echo "Building Docker image $DOCKER_TAG..."
  docker build -f "$DOCKERFILE" -t "$DOCKER_TAG" .
fi

# --- build the native VST2 (container) -------------------------------------
echo "=== Build VST2 ==="
# Run as root: Docker Desktop presents the bind-mount root as root-owned, and
# git's ownership check aborts CMake's FetchContent when the uid differs.
docker run --rm -v "$ROOT":/repo -w /repo \
  -e HOME=/tmp -e GIT_TERMINAL_PROMPT=0 -e SHIM_LOG="${SHIM_LOG:-0}" \
  "$DOCKER_TAG" bash -euc '
  git config --global --add safe.directory "*" 2>/dev/null || true
  cd plugin
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
        -DJUCE_ENABLE_MODULE_SOURCE_GROUPS=OFF \
        -DMM303_SHIM_LOG=$([ "$SHIM_LOG" = 1 ] && echo ON || echo OFF)
  cmake --build build --target MM303_VST --config Release
'
# Hand ownership of what root created back to the host user.
docker run --rm -v "$ROOT":/repo -w /repo "$DOCKER_TAG" \
  bash -c 'chown -R '"$U"' plugin/lib plugin/build 2>/dev/null || true'

SO=$(find "$PDIR/build" -path '*VST*' -name '*.so' | head -n1 || true)
[ -n "$SO" ] || { echo "MM-303: no VST .so under $PDIR/build" >&2; exit 1; }
echo "SO=$SO"

# --- params json (real params + optional preset controls) ------------------
PRESET_ARG=""
[ "$NO_PRESETS" = 1 ] || PRESET_ARG="tools/preset.json"
PC_REAL=$(python3 tools/juce_params_json.py "$PROC" "$NAME" \
          | python3 -c 'import json,sys;print(json.load(sys.stdin)["numParams"])')
if [ "$NO_PRESETS" = 1 ]; then
  python3 tools/juce_params_json.py "$PROC" "$NAME" > "$PDIR/$NAME.json"
else
  python3 tools/juce_params_json.py "$PROC" "$NAME" tools/preset.json > "$PDIR/$NAME.json"
fi
PC=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["numParams"])' "$PDIR/$NAME.json")
echo "params: $PC_REAL real + $((PC - PC_REAL)) preset = $PC"

# --- device gates + smokes (container) -------------------------------------
echo "=== Device gates ==="
docker run --rm -u "$U" -v "$ROOT":/repo -w /repo \
  -e NAME="$NAME" -e SO="$SO" -e UID_HEX="$UID_HEX" \
  -e PC="$PC" -e PC_REAL="$PC_REAL" -e NO_PRESETS="$NO_PRESETS" \
  -e NIN="$NIN" -e NOUT="$NOUT" \
  "$DOCKER_TAG" bash -euc '
    set -e -o pipefail
    file "$SO"
    nm -D "$SO" | grep " VSTPluginMain$" >/dev/null || { echo "FAIL: no VSTPluginMain export"; exit 1; }
    readelf -V "$SO" | grep -oE "GLIBC_2\.[0-9]+" | sort -u
    NEWG=$(readelf -V "$SO" | grep -oE "GLIBC_2\.(3[5-9]|[4-9][0-9])" | sort -u | head -n1 || true)
    [ -z "$NEWG" ] || { echo "FAIL: $NEWG requires glibc newer than 2.34"; exit 1; }
    # Device-proven DT_NEEDED allowlist (Force has no X11/GL; fontconfig/
    # freetype/asound are present).
    EXTRA=$(readelf -d "$SO" | grep NEEDED \
      | grep -vE "libstdc\+\+\.so|libm\.so|libgcc_s\.so|libc\.so|ld-linux|libdl\.so(\.2)?|libpthread\.so(\.0)?|librt\.so(\.1)?|libasound\.so(\.2)?|libfontconfig\.so(\.1)?|libfreetype\.so(\.6)?" || true)
    if [ -n "$EXTRA" ]; then echo "$EXTRA" | sed "s/^/FAIL - non-baseline DT_NEEDED: /"; exit 1; fi
    # no device-unresolvable symbol may remain undefined. Only X11/GL are
    # unresolvable (the device has fontconfig/freetype/asound, which are
    # NEEDED, so Fc_/FT_/snd_ symbols resolve at load).
    RISKU=$(nm -D --undefined-only "$SO" | grep -E " U (_X|X|XF|Xft|XRender|gl|glX|GL)" || true)
    if [ -n "$RISKU" ]; then echo "$RISKU" | sed "s/^/FAIL - device-unresolvable undef: /"; exit 1; fi
    g++ -O1 -Wall -o /tmp/juce_smoke tools/juce_smoke.c -I templates/vst2-abi -ldl -lm
    stdbuf -oL -eL /tmp/juce_smoke "$SO" "0x$UID_HEX" "$NAME" "$PC" | tee /tmp/juce_smoke.out
    python3 tools/midi_layout_check.py "$(ls -t "$(dirname "$SO")"/"Midi Clips"/MM-303-*.mid | head -1)"
    if [ "$NO_PRESETS" != 1 ]; then
      gcc -O1 -Wall -o /tmp/preset_smoke tools/preset_smoke.c -ldl -lm
      stdbuf -oL -eL /tmp/preset_smoke "$SO" "0x$UID_HEX" "$PC_REAL" "$PC" "34,35,38,39,40,41,42,44" | tee /tmp/preset_smoke.out
    fi
    g++ -O2 -std=c++17 -I plugin/src/dsp/mxr tools/mxr_golden_test.cpp plugin/src/dsp/mxr/MxrDistortion.cpp -o /tmp/mxr_golden && /tmp/mxr_golden
    g++ -O2 -std=c++17 -I plugin/src tools/transpose_shift_test.cpp -o /tmp/transpose_shift && /tmp/transpose_shift
    g++ -O2 -std=c++17 -I plugin/src tools/seq_rotate_test.cpp -o /tmp/seq_rotate && /tmp/seq_rotate
    strip "$SO"
    echo "GATES-OK"
  '

# --- package ---------------------------------------------------------------
PKG="output/MockbaMod - VST - $NAME"
mkdir -p "$PKG/payload" "$PKG/licenses"
cp "$SO" "$PKG/$NAME.so"
chmod +x "$PKG/$NAME.so"
cp "$PDIR/$NAME.json" "$PKG/$NAME.json"
# Ship the default config next to the .so (edit-on-device; also embedded
# into the plugin at build time as the first-launch fallback).
[ -f templates/MM-303.conf ] && cp templates/MM-303.conf "$PKG/$NAME.conf"
python3 tools/render_meta.py --template templates/plugin-meta.xml \
  --name "$NAME" --uid "$UID_HEX" \
  --nin "$NIN" --nout "$NOUT" --instrument "$IS_INSTRUMENT" \
  --out "$PKG/plugin-meta.xml"
python3 - "$PKG/plugin-meta.xml" "$VERSION" <<'PY'
import re, sys, pathlib
p, v = pathlib.Path(sys.argv[1]), sys.argv[2]
p.write_text(re.sub(r'version="[0-9.]*"', 'version="%s"' % v, p.read_text(), count=1))
PY
# version.xml (custom TUI.json is produced separately from TUI-templates/)
python3 tools/gen_tui.py "$NAME" --version "$VERSION" \
  --params-json "$PDIR/$NAME.json" --package-dir "$PKG"

# Assemble the Force skin payload: copy skin/ contents into "Plugin Skins/"
# and generate TUI.json there from TUI-templates/layout.conf.
[ -d skin ] && ./skinGen.sh "$PKG"
# the plugin's own license
LIC=
for c in "$PDIR"/LICENSE* "$PDIR"/COPYING*; do
  if [ -f "$c" ]; then LIC=$c; break; fi
done
if [ -n "$LIC" ]; then cp "$LIC" "$PKG/LICENSE"; fi
{
  echo "Native VST2 plugin: $NAME"
  echo "MpcPreset shim: templates/preset (Force preset management, +4 controls)"
  echo "VST2 ABI headers: templates/vst2-abi (xaymar, BSD-2)"
} > "$PKG/licenses/SOURCE.txt"
# third-party notices (math_approx BSD-3, guitarix GxDistortionPlus GPL-2+)
if [ -f THIRD-PARTY-NOTICES.txt ]; then
  cp THIRD-PARTY-NOTICES.txt "$PKG/licenses/THIRD-PARTY-NOTICES.txt"
fi
find "$PKG" -name .DS_Store -delete
echo "=== MM-303 package: $PKG ==="
ls -la "$PKG"

# --- release ---------------------------------------------------------------
# Only with --release: hand off to release.sh (version bump, release folder,
# public-repo sync, commit/tag, ask to push). Plain ./build.sh never does this.
if [ "$RELEASE" = 1 ]; then
  echo "=== Release (--release) ==="
  exec ./release.sh
fi
