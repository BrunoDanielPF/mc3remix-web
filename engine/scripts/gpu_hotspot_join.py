#!/usr/bin/env python3
"""Join a WebGPU command profile to GS batch metadata by command index.

The profile and trace may use adjacent frame numbers: the profile labels the
presented frame, while the batch trace labels the GS drawing frame. Pass both
numbers explicitly and reject rows whose primitive/tile counts disagree.
"""

import argparse
import csv
from pathlib import Path


def load_profile(path: Path, frame: int) -> dict[int, tuple[int, int, int]]:
    result = {}
    with path.open(encoding="utf-8") as source:
        for line in source:
            fields = line.split()
            if len(fields) < 8 or not fields[0].isdigit() or int(fields[0]) != frame or fields[2] not in ("raster", "raster_sprite"):
                continue
            result[int(fields[1])] = (int(fields[3]), int(fields[6]), int(fields[7]))
    return result


def load_batches(path: Path, frame: int) -> dict[int, dict[str, int]]:
    with path.open(newline="", encoding="utf-8") as source:
        return {
            int(row["command"]): {key: int(value) for key, value in row.items()}
            for row in csv.DictReader(source)
            if int(row["frame"]) == frame
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path, help="PS2_GPU_PROFILE output")
    parser.add_argument("batches", type=Path, help="PS2_GPU_FASTPATH_TRACE CSV")
    parser.add_argument("--profile-frame", type=int, required=True)
    parser.add_argument("--trace-frame", type=int, required=True)
    parser.add_argument("--spillover-frame", type=int, help="trace frame for commands emitted after presentation")
    parser.add_argument("--rows", type=int, default=10)
    args = parser.parse_args()
    profile = load_profile(args.profile, args.profile_frame)
    batches = load_batches(args.batches, args.trace_frame)
    spillover = load_batches(args.batches, args.spillover_frame) if args.spillover_frame is not None else {}
    if not profile or not batches:
        parser.error("selected frame has no raster profile or batch trace rows")

    matched = []
    mismatched = []
    spillover_count = 0
    for command, (ns, prims, tiles) in profile.items():
        batch = batches.get(command)
        if batch is None:
            batch = spillover.get(command)
            if batch is not None:
                spillover_count += 1
        if batch is None or batch["prims"] != prims or batch["tiles"] != tiles:
            mismatched.append(command)
            continue
        matched.append((command, ns, batch))
    if mismatched:
        parser.error(f"{len(mismatched)} raster commands do not match batch metadata; first: {mismatched[:8]}")

    total_ns = sum(ns for _, ns, _ in matched)
    print(f"profile_frame={args.profile_frame} trace_frame={args.trace_frame} matched={len(matched)} spillover={spillover_count} raster_profile_ms={total_ns / 1e6:.2f}")
    print("command  ms  raster%  prims  tiles  entries  textured  broad  fb/z/tex  blend  ztest")
    for command, ns, batch in sorted(matched, key=lambda item: item[1], reverse=True)[:args.rows]:
        print(
            f"{command:7d} {ns / 1e6:6.2f} {ns * 100 / total_ns:7.1f}"
            f" {batch['prims']:6d} {batch['tiles']:6d} {batch['tile_entries']:8d}"
            f" {batch['textured_prims']:9d} {batch['broad_prims']:6d}"
            f" {batch['fbpsm']}/{batch['zpsm']}/{batch['tex_psm']:3d}"
            f" {batch['blend_mode']:6d} {batch['ztest_mode']:6d}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
