#!/usr/bin/env python3
"""midi_layout_check.py - assert an exported Capture .mid has the expected layout.

Usage: tools/midi_layout_check.py <file.mid>

Checks (Standard MIDI File):
  - ticksPerQuarterNote == 960 (the Akai Force resolution);
  - the first 16 note-ons land on ticks 0, 240, 480, ... (16th-note grid);
Exits non-zero with a message on the first failure.
"""
import struct
import sys


def vlq(d, i):
    v = 0
    while True:
        b = d[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, i


def main(path):
    d = open(path, "rb").read()
    assert d[:4] == b"MThd", "not an SMF (missing MThd)"
    tpqn = struct.unpack(">H", d[12:14])[0]
    assert tpqn == 960, f"TPQN {tpqn} != 960"

    i = 8 + struct.unpack(">I", d[4:8])[0]
    ons = []
    while i < len(d) and d[i:i + 4] == b"MTrk":
        tlen = struct.unpack(">I", d[i + 4:i + 8])[0]
        i += 8
        end = i + tlen
        tick = 0
        run = None
        while i < end:
            dt, i = vlq(d, i)
            tick += dt
            st = d[i]
            if st < 0x80:
                st = run
            else:
                i += 1
                if st < 0xF0:
                    run = st
            if st == 0xFF:
                i += 1
                ln, i = vlq(d, i)
                i += ln
            elif st in (0xF0, 0xF7):
                ln, i = vlq(d, i)
                i += ln
            else:
                hi = st & 0xF0
                n = 1 if hi in (0xC0, 0xD0) else 2
                a = d[i]
                b = d[i + 1] if n == 2 else 0
                i += n
                if hi == 0x90 and b > 0:
                    ons.append(tick)
        i = end

    assert len(ons) >= 16, f"only {len(ons)} note-ons (expected >= 16)"
    for k in range(16):
        assert ons[k] == k * 240, f"note {k} at tick {ons[k]}, expected {k * 240}"
    print(f"midi_layout_check: OK ({len(ons)} note-ons, TPQN {tpqn})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
