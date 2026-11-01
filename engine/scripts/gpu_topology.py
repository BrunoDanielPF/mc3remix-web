"""Summarize a PS2_GPU_PROFILE command stream and simple fusion ceilings.

Usage: python3 scripts/gpu_topology.py games/mc3remix/gpu-cruise.txt
"""

import argparse
from collections import Counter, defaultdict
from statistics import mean


def fusion_savings(commands, max_upload_groups):
    """Upper bound for replacing each eligible contiguous run by one dispatch."""
    saved = 0
    run = 0
    for pipeline, groups in [*commands, ("end", 0)]:
        eligible = pipeline == "load_clut" or (pipeline == "upload" and groups <= max_upload_groups)
        if eligible:
            run += 1
        else:
            saved += max(0, run - 1)
            run = 0
    return saved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", help="file written by PS2_GPU_PROFILE=from,to,file")
    args = parser.parse_args()

    frames = defaultdict(list)
    with open(args.profile, encoding="utf-8") as profile:
        for line_number, line in enumerate(profile, 1):
            parts = line.split()
            if not parts:
                continue
            if len(parts) < 5:
                parser.error(f"malformed row {line_number}: expected at least 5 fields")
            try:
                frame = int(parts[0])
                groups = int(parts[4])
            except ValueError:
                parser.error(f"malformed row {line_number}: frame and workgroups must be integers")
            frames[frame].append((parts[2], groups))

    if not frames:
        parser.error("profile contains no commands")

    totals = [len(commands) for commands in frames.values()]
    counts = Counter(pipeline for commands in frames.values() for pipeline, _ in commands)
    print(f"{len(frames)} presented frames; dispatches/frame mean {mean(totals):.1f}, min {min(totals)}, max {max(totals)}")
    for pipeline, count in counts.most_common():
        print(f"  {pipeline:12s} {count / len(frames):6.1f}/frame")

    uploads = [groups for commands in frames.values() for pipeline, groups in commands if pipeline == "upload"]
    for limit in (1, 4, 16, 1024):
        eligible = sum(groups <= limit for groups in uploads) / len(frames)
        saved = mean(fusion_savings(commands, limit) for commands in frames.values())
        print(f"upload <= {limit:4d} workgroups + CLUT: {eligible:5.1f} eligible uploads/frame; "
              f"at most {saved:5.1f} dispatches saved/frame; at least {mean(totals) - saved:6.1f} remain")
    print("These are command-count ceilings, not predicted FPS; GPU work and ordering costs remain.")


if __name__ == "__main__":
    main()
