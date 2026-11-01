#!/usr/bin/env python3
"""Summarize PS2_GPU_FASTPATH_TRACE batches for a first direct-render path."""

import csv
import sys
from collections import defaultdict


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("usage: gpu_fastpath_trace.py <trace.csv> [frame]", file=sys.stderr)
        return 2
    frame_filter = int(sys.argv[2]) if len(sys.argv) == 3 else None
    with open(sys.argv[1], newline="", encoding="utf-8") as source:
        rows = [{key: int(value) for key, value in row.items()} for row in csv.DictReader(source)]
    if frame_filter is not None:
        rows = [row for row in rows if row["frame"] == frame_filter]
    if not rows:
        print("no batches in selected range", file=sys.stderr)
        return 1

    total_prims = sum(row["prims"] for row in rows)
    whole = [row for row in rows if row["broad_prims"] == row["prims"]]
    whole_textured = [row for row in whole if row["textured_prims"] == row["prims"]]
    print(f"batches={len(rows)} primitives={total_prims} tiles={sum(row['tiles'] for row in rows)}")
    print(f"broad-whole={len(whole)} primitives={sum(row['prims'] for row in whole)}")
    print(f"broad-whole-textured={len(whole_textured)} primitives={sum(row['prims'] for row in whole_textured)}")
    for texture, label in ((19, "PSMT8"), (20, "PSMT4")):
        copy_modes = [
            row for row in whole_textured
            if row["fbpsm"] == 0 and row["zpsm"] == 58 and row["tex_psm"] == texture
            and row["blend_mode"] in (10, 42, 256)
            and row["ztest_mode"] == 2 and row["zwrite"] == 1
        ]
        print(
            f"copy-compatible-{label}={len(copy_modes)} batches, "
            f"{sum(row['prims'] for row in copy_modes)} primitives, "
            f"{sum(row['tiles'] for row in copy_modes)} tiles"
        )

    groups = defaultdict(lambda: [0, 0, 0])
    for row in whole_textured:
        key = (row["fbpsm"], row["zpsm"], row["tex_psm"], row["blend_mode"], row["ztest_mode"], row["zwrite"])
        group = groups[key]
        group[0] += 1
        group[1] += row["prims"]
        group[2] += row["tiles"]
    print("top complete textured modes (fb,z,texture,blend,ztest,zwrite : batches,primitives,tiles):")
    for key, values in sorted(groups.items(), key=lambda item: (item[1][1], item[1][2]), reverse=True)[:15]:
        print("  " + ",".join(map(str, key)) + " : " + ",".join(map(str, values)))

    print("top complete textured batches (frame,command,primitives,tiles,blend,ztest,texture):")
    for row in sorted(whole_textured, key=lambda item: (item["prims"], item["tiles"]), reverse=True)[:10]:
        print("  " + ",".join(str(row[key]) for key in ("frame", "command", "prims", "tiles", "blend_mode", "ztest_mode", "tex_psm")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
