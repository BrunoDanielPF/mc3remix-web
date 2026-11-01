"""Measure optimistic command parallelism in gpu_access_graph.py JSON output.

The graph has page-granularity hazards only. CPU transfers, submissions and
unmodeled GS state can impose additional ordering, so these numbers are lower
bounds on serialization rather than a safe execution schedule.
"""

import argparse
from collections import Counter
import json
from pathlib import Path


def summarize(graph, resources=None):
    nodes = graph["nodes"]
    depth = [1] * len(nodes)
    parent = [-1] * len(nodes)
    parent_hazards = [[] for _ in nodes]
    for edge in sorted(graph["edges"], key=lambda item: item["from"]):
        relevant = [item for item in edge["hazards"] if resources is None or item["resource"] in resources]
        if not relevant:
            continue
        source, target = edge["from"], edge["to"]
        candidate = depth[source] + 1
        if candidate > depth[target]:
            depth[target] = candidate
            parent[target] = source
            parent_hazards[target] = relevant
    levels = Counter(depth)
    tail = max(range(len(nodes)), key=lambda index: depth[index])
    path = []
    path_hazards = Counter()
    while tail >= 0:
        path.append(nodes[tail]["pipeline"])
        path_hazards.update((item["resource"], item["kind"]) for item in parent_hazards[tail])
        tail = parent[tail]
    return levels, Counter(path), path_hazards


def raster_gaps(graph, resources=None):
    nodes = graph["nodes"]
    successors = [[] for _ in nodes]
    predecessors = [[] for _ in nodes]
    for edge in graph["edges"]:
        if resources is not None and not any(hazard["resource"] in resources for hazard in edge["hazards"]):
            continue
        source, target = edge["from"], edge["to"]
        successors[source].append(target)
        predecessors[target].append(source)
    rasters = [index for index, node in enumerate(nodes) if node["pipeline"] == "raster"]
    adjacent = blocked = 0
    blockers = Counter()
    for first, second in zip(rasters, rasters[1:]):
        if second == first + 1:
            adjacent += 1
            continue
        downstream = {first}
        stack = [first]
        while stack:
            for target in successors[stack.pop()]:
                if target < second and target not in downstream:
                    downstream.add(target)
                    stack.append(target)
        upstream = {second}
        stack = [second]
        while stack:
            for source in predecessors[stack.pop()]:
                if source > first and source not in upstream:
                    upstream.add(source)
                    stack.append(source)
        required = (downstream & upstream) - {first, second}
        if required:
            blocked += 1
            blockers.update(nodes[index]["pipeline"] for index in required)
    return len(rasters), adjacent, blocked, blockers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graph", type=Path, help="JSON from gpu_access_graph.py")
    parser.add_argument("--resources", help="comma-separated resource names; default is all")
    args = parser.parse_args()
    graph = json.loads(args.graph.read_text(encoding="utf-8"))
    resources = set(args.resources.split(",")) if args.resources else None
    levels, path, hazards = summarize(graph, resources)
    print(f"frame {graph['frame']}: {len(graph['nodes'])} commands, "
          f"{len(graph['edges'])} hazard pairs, {max(levels)} dependent levels")
    print(f"layer width: median {sorted(levels.values())[len(levels) // 2]}, "
          f"maximum {max(levels.values())}")
    print("critical path: " + ", ".join(f"{name}={count}" for name, count in path.most_common()))
    print("path hazards: " + ", ".join(f"{resource}.{kind}={count}" for (resource, kind), count in hazards.most_common()))
    rasters, adjacent, blocked, blockers = raster_gaps(graph, resources)
    print(f"raster gaps: {rasters} raster commands, {adjacent} adjacent pairs, "
          f"{blocked} pairs with required intermediate commands, "
          f"{max(0, rasters - 1 - blocked)} optimistic movable pairs")
    print("required intermediary types: " + ", ".join(f"{name}={count}" for name, count in blockers.most_common()))
    print("optimistic page-level bound only; preserve CPU transfer and submission order")


if __name__ == "__main__":
    main()
