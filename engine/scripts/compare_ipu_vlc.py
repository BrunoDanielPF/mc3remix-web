"""Compare local IPU DCT/CBP VLC tables against a downloaded PCSX2 mpeg2_vlc.h.

Usage: python engine/scripts/compare_ipu_vlc.py path/to/mpeg2_vlc.h
Run from the repository root. The reference file is intentionally not vendored.
"""

import argparse
import re
from pathlib import Path


def chunks(text):
    return ["{" + chunk + "}" for chunk in re.findall(r"\{\{(.*?)\}\}", text, re.S)]


def parse_pcsx2(path):
    text = Path(path).read_text()
    text = text[text.index("DCTtabSet DCT =") :]
    groups = []
    for chunk in chunks(text):
        groups.append([tuple(map(int, x)) for x in re.findall(r"\{(\d+),\s*(\d+),\s*(\d+)\}", chunk)])
    assert list(map(len, groups[:11])) == [12, 12, 60, 252, 8, 8, 16, 16, 16, 16, 16]
    return groups[:11]


def parse_local(path):
    text = Path(path).read_text()
    block = text.split("kDctTable0[] = {", 1)[1].split("};", 1)[0]
    return [tuple(int(y, 0) for y in x) for x in re.findall(r"\{(0x[0-9A-F]+|\d+),\s*(\d+),\s*(\d+),\s*(-?\d+)\}", block)]


def pcsx2(code, first, groups):
    if code >= 16384:
        tab = groups[0 if first else 1][(code >> 12) - 4]
    elif code >= 1024:
        tab = groups[2][(code >> 8) - 4]
    elif code >= 512:
        tab = groups[4][(code >> 6) - 8]
    elif code >= 256:
        tab = groups[6][(code >> 4) - 16]
    elif code >= 128:
        tab = groups[7][(code >> 3) - 16]
    elif code >= 64:
        tab = groups[8][(code >> 2) - 16]
    elif code >= 32:
        tab = groups[9][(code >> 1) - 16]
    elif code >= 16:
        tab = groups[10][code - 16]
    else:
        return None
    return tab


def local(code, first, entries):
    if first and code & 0x8000:
        return (0, 1, 1)
    if not first and code & 0xC000 == 0x8000:
        return (64, 0, 2)
    for v, length, run, level in entries:
        if code >> (16 - length) == v:
            return (65 if run == 255 else run, level, length)
    return None


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pcsx2_tables", type=Path)
args = parser.parse_args()

groups = parse_pcsx2(args.pcsx2_tables)
entries = parse_local("engine/runtime/src/ipu/tables.hpp")
for first in (True, False):
    diffs = []
    for code in range(65536):
        a, b = pcsx2(code, first, groups), local(code, first, entries)
        if a != b:
            diffs.append((code, a, b))
    print("first" if first else "next", "differences", len(diffs), "examples", diffs[:15])

text = args.pcsx2_tables.read_text()
cbp7 = [tuple(int(y, 0) for y in x) for x in re.findall(r"\{(0x[0-9a-f]+|\d+),\s*(\d+)\}", text.split("CBPtab CBP_7[] = {", 1)[1].split("};", 1)[0])]
cbp9 = [tuple(int(y, 0) for y in x) for x in re.findall(r"\{(0x[0-9a-f]+|\d+),\s*(\d+)\}", text.split("CBPtab CBP_9[] = {", 1)[1].split("};", 1)[0])]
assert len(cbp7) == 112 and len(cbp9) == 64
local_text = Path("engine/runtime/src/ipu/tables.hpp").read_text()
block = local_text.split("kCodedBlockPattern[] = {", 1)[1].split("};", 1)[0]
local_cbp = [tuple(int(y, 0) for y in x) for x in re.findall(r"\{(0x[0-9A-F]+|\d+),\s*(\d+),\s*(0x[0-9A-F]+|\d+)\}", block)]
diffs = []
for code in range(512):
    a = cbp7[(code >> 2) - 16] if code >= 64 else cbp9[code]
    b = next(((v, length) for bits, length, v in local_cbp if code >> (9 - length) == bits), (0, 0))
    if a != b:
        diffs.append((code, a, b))
print("cbp differences", len(diffs), "examples", diffs[:15])
