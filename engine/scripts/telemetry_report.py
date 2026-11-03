#!/usr/bin/env python3
"""Read-only MC3 telemetry summary for operators on the VPS."""

import argparse
import contextlib
import csv
import json
import sqlite3
import sys
import time
from collections import defaultdict
from pathlib import Path
from statistics import median


SAMPLE_COLUMNS = ("received_at", "session", "gpu", "browser", "os", "graphics", "visible", "frame",
                  "produced_fps", "screen_fps", "screen_p95_ms", "gs_work_ms",
                  "present_wait_ms", "gpu_estimate_ms", "disc_wait_ms", "audio_ms",
                  "heap_mib", "primitives_delta", "trigger", "window_ms", "emu_idle_pct",
                  "vu_idle_pct", "gs_idle_pct", "disc_fetches", "disc_ready_misses",
                  "disc_preload_hits", "long_task_count", "long_task_ms")


def load(db_path, hours):
    uri = db_path.resolve().as_uri() + "?mode=ro"
    with contextlib.closing(sqlite3.connect(uri, uri=True)) as db:
        db.row_factory = sqlite3.Row
        cutoff = int(time.time() - hours * 3600)
        samples = [dict(row) for row in db.execute(
            f"SELECT {','.join(SAMPLE_COLUMNS)} FROM events WHERE kind='sample' AND received_at >= ? ORDER BY received_at",
            (cutoff,))]
        errors = [dict(row) for row in db.execute(
            "SELECT received_at, session, kind, gpu, message FROM events "
            "WHERE kind IN ('error','halt') AND received_at >= ? ORDER BY received_at DESC LIMIT 20",
            (cutoff,))]
        sessions = db.execute("SELECT COUNT(DISTINCT session) FROM events WHERE received_at >= ?", (cutoff,)).fetchone()[0]
        halts = db.execute("SELECT COUNT(*) FROM events WHERE kind='halt' AND received_at >= ?", (cutoff,)).fetchone()[0]
    return samples, errors, sessions, halts


def summarize(samples, errors, sessions, halts, low_fps=35):
    visible = [row for row in samples if row["visible"] and row["screen_fps"] is not None]
    periodic = [row for row in visible if row["trigger"] == "periodic"]
    by_gpu = defaultdict(list)
    by_session = defaultdict(list)
    for row in periodic:
        by_gpu[(row["gpu"], row["graphics"])].append(row)
    for row in visible:
        by_session[row["session"]].append(row)
    gpu_rows = []
    for (gpu, graphics), rows in sorted(by_gpu.items(), key=lambda item: -len(item[1])):
        gpu_rows.append({"gpu": gpu, "graphics": graphics, "samples": len(rows),
                         "sessions": len({row["session"] for row in rows}),
                         "median_screen_fps": round(median(row["screen_fps"] for row in rows), 1)})
    problem_rows = []
    for session, rows in by_session.items():
        low = sum(row["screen_fps"] < low_fps for row in rows)
        stalls = sum((row["screen_p95_ms"] or 0) >= 100 for row in rows)
        disc_stalls = sum((row["disc_wait_ms"] or 0) >= 500 for row in rows)
        alerts = sum(row["trigger"] != "periodic" for row in rows)
        if low or stalls or disc_stalls or alerts:
            latest = rows[-1]
            problem_rows.append({"session": session[:8], "gpu": latest["gpu"], "graphics": latest["graphics"],
                                 "latest_screen_fps": latest["screen_fps"], "low_samples": low,
                                 "stutter_samples": stalls, "disc_stall_samples": disc_stalls,
                                 "alert_samples": alerts,
                                 "peak_gs_work_ms": max((row["gs_work_ms"] or 0) for row in rows),
                                 "peak_present_wait_ms": max((row["present_wait_ms"] or 0) for row in rows),
                                 "peak_disc_wait_ms": max((row["disc_wait_ms"] or 0) for row in rows),
                                 "last_seen": latest["received_at"]})
    problem_rows.sort(key=lambda row: (-row["low_samples"], -row["stutter_samples"],
                                       -row["disc_stall_samples"], -row["last_seen"]))
    return {"sessions": sessions, "samples": len(samples), "visible_samples": len(visible),
            "periodic_samples": len(periodic), "alert_samples": len(visible) - len(periodic),
            "low_fps_threshold": low_fps,
            "low_fps_samples": sum(row["screen_fps"] < low_fps for row in visible),
            "halt_events": halts,
            "error_events_shown": len(errors), "by_gpu": gpu_rows[:20], "problem_sessions": problem_rows[:20],
            "recent_errors": [{**row, "session": row["session"][:8]} for row in errors]}


def print_text(result, hours):
    print(f"MC3 telemetry · last {hours}h · sessions {result['sessions']} · samples {result['samples']}")
    print(f"Visible samples {result['visible_samples']} · alerts {result['alert_samples']} "
          f"· below {result['low_fps_threshold']:g} FPS {result['low_fps_samples']} "
          f"· halts {result['halt_events']} · recent errors {result['error_events_shown']}")
    print("\nGPU / graphics | periodic samples | sessions | median displayed FPS")
    for row in result["by_gpu"]:
        print(f"{row['gpu']} / {row['graphics']} | {row['samples']} | {row['sessions']} | {row['median_screen_fps']}")
    print("\nSessions needing inspection | low FPS | stutter | disc wait | alerts | latest FPS | peak GS/present/disc ms")
    for row in result["problem_sessions"]:
        print(f"{row['session']} {row['gpu']} / {row['graphics']} | {row['low_samples']} | "
              f"{row['stutter_samples']} | {row['disc_stall_samples']} | {row['alert_samples']} | "
              f"{row['latest_screen_fps']} | {row['peak_gs_work_ms']}/{row['peak_present_wait_ms']}/{row['peak_disc_wait_ms']}")
    print("\nRecent errors / halts")
    for row in result["recent_errors"]:
        print(f"{time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(row['received_at']))} "
              f"{row['session']} {row['kind']} {row['message'] or ''}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, default=Path("/var/lib/mc3-telemetry/telemetry.sqlite3"))
    parser.add_argument("--hours", type=int, default=24)
    parser.add_argument("--low-fps", type=float, default=35)
    parser.add_argument("--format", choices=("text", "json", "csv"), default="text")
    args = parser.parse_args()
    if not 1 <= args.hours <= 720:
        parser.error("hours must be 1..720")
    if not 1 <= args.low_fps <= 120:
        parser.error("low-fps must be 1..120")
    try:
        samples, errors, sessions, halts = load(args.db, args.hours)
    except sqlite3.Error as error:
        parser.error(f"cannot read telemetry database: {error}")
    if args.format == "csv":
        writer = csv.DictWriter(sys.stdout, fieldnames=SAMPLE_COLUMNS)
        writer.writeheader()
        writer.writerows(samples)
        return
    result = summarize(samples, errors, sessions, halts, args.low_fps)
    if args.format == "json":
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print_text(result, args.hours)


if __name__ == "__main__":
    main()
