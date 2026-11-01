"""Build a conservative GPU-resource dependency graph from PS2_GPU_ACCESS_PROFILE.

Usage: python3 scripts/gpu_access_graph.py access-cruise.txt --frame 6351 --json graph.json
Legacy seven-column traces contain only VRAM. Current traces also include CLUT,
decoded texture and scratch access for upload/CLUT/decode/snapshot/raster commands.
Other commands retain their original order until their resources are annotated.
"""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

RESOURCE_PAGES = {"vram": (8192, 512), "clut": (2048, None),
                  "decoded": (4096, None), "scratch": (4096, None)}


def pages(field, page_bytes, wrap_count):
    result = set()
    if field == "-":
        return result
    for pair in field.split(";"):
        lo, hi = (int(value) for value in pair.split(":"))
        if hi <= lo:
            continue
        first = lo // page_bytes
        count = (hi + page_bytes - 1) // page_bytes - first
        if wrap_count:
            count = min(count, wrap_count)
        result.update(((first + offset) % wrap_count if wrap_count else first + offset) for offset in range(count))
    return result


def read_commands(path, selected_frame):
    commands = []
    with open(path, encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            fields = line.split()
            if not fields:
                continue
            if len(fields) not in (7, 13):
                raise ValueError(f"row {number}: expected 7 or 13 fields, got {len(fields)}")
            frame, index = int(fields[0]), int(fields[1])
            if frame == selected_frame:
                access = {}
                for position, (resource, (page_bytes, wrap_count)) in enumerate(RESOURCE_PAGES.items()):
                    offset = 5 + position * 2
                    if offset >= len(fields):
                        access[resource] = {"reads": set(), "writes": set()}
                    else:
                        access[resource] = {"reads": pages(fields[offset], page_bytes, wrap_count),
                                            "writes": pages(fields[offset + 1], page_bytes, wrap_count)}
                commands.append({"index": index, "pipeline": fields[2], "workgroups": int(fields[3]) * int(fields[4]),
                                 "access": access})
    return commands


def build_graph(commands):
    last_writer = {}
    readers = defaultdict(set)
    edges = defaultdict(set)
    for position, command in enumerate(commands):
        for resource, access in command["access"].items():
            for page in access["reads"]:
                key = (resource, page)
                if key in last_writer:
                    edges[(last_writer[key], position)].add((resource, "RAW"))
                readers[key].add(position)
            for page in access["writes"]:
                key = (resource, page)
                if key in last_writer:
                    edges[(last_writer[key], position)].add((resource, "WAW"))
                for reader in readers[key]:
                    if reader != position:
                        edges[(reader, position)].add((resource, "WAR"))
                readers[key].clear()
                last_writer[key] = position
    return edges


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--json", type=Path, help="write nodes and VRAM hazard edges to this file")
    args = parser.parse_args()

    commands = read_commands(args.profile, args.frame)
    if not commands:
        parser.error(f"no commands found for frame {args.frame}")
    edges = build_graph(commands)
    kind_counts = Counter(kind for hazards in edges.values() for kind in hazards)
    pair_counts = Counter((commands[src]["pipeline"], commands[dst]["pipeline"], resource, kind)
                          for (src, dst), hazards in edges.items() for resource, kind in hazards)
    no_access = Counter(command["pipeline"] for command in commands
                        if not any(a["reads"] or a["writes"] for a in command["access"].values()))
    print(f"frame {args.frame}: {len(commands)} commands, {len(edges)} resource hazard edges")
    print("hazards:", ", ".join(f"{resource}.{kind}={kind_counts[resource, kind]}"
                                for resource in RESOURCE_PAGES for kind in ("RAW", "WAR", "WAW") if kind_counts[resource, kind]))
    print("commands without annotated resource access:", dict(no_access))
    print("most frequent pipeline dependencies:")
    for (source, target, resource, kind), count in pair_counts.most_common(12):
        print(f"  {source:12s} -> {target:12s} {resource}.{kind}: {count}")

    if args.json:
        result = {
            "frame": args.frame,
            "scope": "Conservative page ranges; unannotated commands retain original order",
            "nodes": [{"position": position, "commandIndex": command["index"], "pipeline": command["pipeline"],
                       "workgroups": command["workgroups"],
                       "access": {resource: {"readPages": sorted(access["reads"]),
                                             "writePages": sorted(access["writes"])}
                                  for resource, access in command["access"].items()}}
                      for position, command in enumerate(commands)],
            "edges": [{"from": src, "to": dst,
                       "hazards": [{"resource": resource, "kind": kind} for resource, kind in sorted(hazards)]}
                      for (src, dst), hazards in sorted(edges.items())],
        }
        args.json.write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")
        print(f"graph written to {args.json}")


if __name__ == "__main__":
    main()
