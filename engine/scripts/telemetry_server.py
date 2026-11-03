#!/usr/bin/env python3
"""Small, loopback-only collector for anonymous MC3 performance telemetry."""

import argparse
import contextlib
import json
import math
import os
import re
import sqlite3
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


SESSION = re.compile(r"^[0-9a-f]{32}$")
BUILD = re.compile(r"^[a-zA-Z0-9._-]{1,40}$")
GPU = re.compile(r"^[\w .()+,:-]{1,80}$")
BROWSER = re.compile(r"^(?:edge|chrome|firefox|safari|other)(?:/[0-9]{1,3})?$")
KINDS = {"start", "sample", "error", "halt", "end"}
ERROR_CATEGORIES = {"memory-error", "gpu-device-lost", "graphics-error",
                    "disc-or-network-error", "runtime-error", "unclassified-error",
                    "memory-allocation", "wasm-memory-access"}
TRIGGERS = {"periodic", "low-fps", "stutter", "disc-stall", "timer-delay"}
GRAPHICS = {"performance", "standard", "quality", "maximum"}
SYSTEMS = {"windows", "linux", "macos", "android", "ios", "other"}
FIELDS = {
    "frame": (int, 0, 10**12),
    "producedFrames": (int, 0, 10000),
    "shownFrames": (int, 0, 10000),
    "producedFps": (float, 0, 240),
    "screenFps": (float, 0, 240),
    "screenP95Ms": (float, 0, 60000),
    "gsWorkMs": (float, 0, 60000),
    "presentWaitMs": (float, 0, 60000),
    "gpuEstimateMs": (float, 0, 60000),
    "discWaitMs": (float, 0, 60000),
    "audioMs": (float, 0, 60000),
    "heapMiB": (float, 0, 16384),
    "primitivesDelta": (int, 0, 10**12),
    "windowMs": (float, 0, 120000),
    "emuIdlePct": (float, 0, 100),
    "vuIdlePct": (float, 0, 100),
    "gsIdlePct": (float, 0, 100),
    "discFetches": (int, 0, 10000),
    "discReadyMisses": (int, 0, 10000),
    "discPreloadHits": (int, 0, 10000),
    "longTaskCount": (int, 0, 10000),
    "longTaskMs": (float, 0, 120000),
}
BASE_KEYS = {"v", "session", "seq", "kind", "build", "gpu", "browser", "os", "graphics", "visible"}
ALL_KEYS = BASE_KEYS | FIELDS.keys() | {"message", "trigger"}
DB_COLUMNS = {
    "frame": "frame", "producedFrames": "produced_frames", "shownFrames": "shown_frames",
    "producedFps": "produced_fps", "screenFps": "screen_fps", "screenP95Ms": "screen_p95_ms",
    "gsWorkMs": "gs_work_ms", "presentWaitMs": "present_wait_ms", "gpuEstimateMs": "gpu_estimate_ms",
    "discWaitMs": "disc_wait_ms", "audioMs": "audio_ms", "heapMiB": "heap_mib",
    "primitivesDelta": "primitives_delta",
    "windowMs": "window_ms", "emuIdlePct": "emu_idle_pct", "vuIdlePct": "vu_idle_pct",
    "gsIdlePct": "gs_idle_pct", "discFetches": "disc_fetches",
    "discReadyMisses": "disc_ready_misses", "discPreloadHits": "disc_preload_hits",
    "longTaskCount": "long_task_count", "longTaskMs": "long_task_ms",
}
MIGRATION_COLUMNS = {
    "trigger": "TEXT NOT NULL DEFAULT 'periodic'", "window_ms": "REAL",
    "emu_idle_pct": "REAL", "vu_idle_pct": "REAL", "gs_idle_pct": "REAL",
    "disc_fetches": "INTEGER", "disc_ready_misses": "INTEGER", "disc_preload_hits": "INTEGER",
    "long_task_count": "INTEGER", "long_task_ms": "REAL",
}


def clean_message(value):
    if not isinstance(value, str):
        raise ValueError("invalid message")
    value = re.sub(r"[\x00-\x1f\x7f]", " ", value)
    value = re.sub(r"(?:https?|wss?)://\S+", "[url]", value, flags=re.I)
    value = re.sub(r"\b[0-9a-f]{32}\b", "[code]", value, flags=re.I)
    value = re.sub(r"\b[^\s@]+@[^\s@]+\.[^\s@]+\b", "[email]", value)
    return value[:240]


def validate(payload):
    if not isinstance(payload, dict) or not BASE_KEYS <= payload.keys() or payload.keys() - ALL_KEYS:
        raise ValueError("invalid fields")
    if type(payload["v"]) is not int or payload["v"] != 1:
        raise ValueError("invalid version")
    if not isinstance(payload["session"], str) or not SESSION.fullmatch(payload["session"]):
        raise ValueError("invalid session")
    if type(payload["seq"]) is not int or not 0 <= payload["seq"] <= 10**9:
        raise ValueError("invalid sequence")
    if (not isinstance(payload["kind"], str) or payload["kind"] not in KINDS
            or not isinstance(payload["graphics"], str) or payload["graphics"] not in GRAPHICS
            or not isinstance(payload["os"], str) or payload["os"] not in SYSTEMS):
        raise ValueError("invalid category")
    if not isinstance(payload["build"], str) or not BUILD.fullmatch(payload["build"]):
        raise ValueError("invalid build")
    if not isinstance(payload["gpu"], str) or not GPU.fullmatch(payload["gpu"]):
        raise ValueError("invalid gpu")
    if not isinstance(payload["browser"], str) or not BROWSER.fullmatch(payload["browser"]):
        raise ValueError("invalid browser")
    if type(payload["visible"]) is not bool:
        raise ValueError("invalid visibility")
    for key, (kind, low, high) in FIELDS.items():
        if key not in payload or payload[key] is None:
            continue
        value = payload[key]
        if kind is int:
            if type(value) is not int:
                raise ValueError("invalid number")
        elif type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError("invalid number")
        if not low <= value <= high:
            raise ValueError("number outside range")
    if payload["kind"] == "sample" and ("producedFps" not in payload or "screenFps" not in payload):
        raise ValueError("sample lacks fps")
    if "trigger" in payload:
        if (payload["kind"] != "sample" or not isinstance(payload["trigger"], str)
                or payload["trigger"] not in TRIGGERS):
            raise ValueError("invalid sample trigger")
    if "message" in payload:
        if payload["kind"] not in {"error", "halt"}:
            raise ValueError("message only on errors")
        payload["message"] = clean_message(payload["message"])
        if payload["message"] not in ERROR_CATEGORIES:
            raise ValueError("invalid error category")
    return payload


def connect_db(path):
    db = sqlite3.connect(path, timeout=3)
    db.execute("PRAGMA journal_mode=WAL")
    db.execute("PRAGMA busy_timeout=3000")
    db.execute("""CREATE TABLE IF NOT EXISTS events (
        id INTEGER PRIMARY KEY, received_at INTEGER NOT NULL, session TEXT NOT NULL,
        seq INTEGER NOT NULL, kind TEXT NOT NULL, build TEXT NOT NULL, gpu TEXT NOT NULL,
        browser TEXT NOT NULL, os TEXT NOT NULL, graphics TEXT NOT NULL, visible INTEGER NOT NULL,
        frame INTEGER, produced_frames INTEGER, shown_frames INTEGER,
        produced_fps REAL, screen_fps REAL, screen_p95_ms REAL,
        gs_work_ms REAL, present_wait_ms REAL, gpu_estimate_ms REAL,
        disc_wait_ms REAL, audio_ms REAL, heap_mib REAL, primitives_delta INTEGER,
        message TEXT, trigger TEXT NOT NULL DEFAULT 'periodic', window_ms REAL,
        emu_idle_pct REAL, vu_idle_pct REAL, gs_idle_pct REAL,
        disc_fetches INTEGER, disc_ready_misses INTEGER, disc_preload_hits INTEGER,
        long_task_count INTEGER, long_task_ms REAL,
        UNIQUE(session, seq))""")
    existing = {row[1] for row in db.execute("PRAGMA table_info(events)")}
    for column, column_type in MIGRATION_COLUMNS.items():
        if column not in existing:
            db.execute(f"ALTER TABLE events ADD COLUMN {column} {column_type}")
    db.execute("CREATE INDEX IF NOT EXISTS events_time ON events(received_at)")
    db.execute("CREATE INDEX IF NOT EXISTS events_session ON events(session, received_at)")
    db.commit()
    return db


def insert_event(db, payload, now=None):
    payload = validate(payload)
    columns = ["received_at", "session", "seq", "kind", "build", "gpu", "browser", "os", "graphics", "visible"]
    values = [int(time.time()) if now is None else now]
    values += [payload[key] for key in ("session", "seq", "kind", "build", "gpu", "browser", "os", "graphics")]
    values += [int(payload["visible"])]
    columns += list(DB_COLUMNS.values()) + ["message", "trigger"]
    values += [payload.get(key) for key in DB_COLUMNS] + [payload.get("message"), payload.get("trigger", "periodic")]
    placeholders = ",".join("?" for _ in columns)
    db.execute(f"INSERT OR IGNORE INTO events ({','.join(columns)}) VALUES ({placeholders})", values)
    db.commit()


class TelemetryHandler(BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        # No request URL, IP address, headers or payload in service logs.
        pass

    def respond(self, status, content=b""):
        self.send_response(status)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        if content:
            self.wfile.write(content)

    def do_GET(self):
        if self.path == "/healthz":
            self.respond(200, b"ok\n")
        else:
            self.respond(404)

    def do_POST(self):
        self.connection.settimeout(3)
        if self.path != "/v1/telemetry":
            self.respond(404)
            return
        if self.headers.get("Origin") != self.server.allowed_origin:
            self.respond(403)
            return
        if self.headers.get_content_type() != "application/json" or self.headers.get("Transfer-Encoding"):
            self.respond(415)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if not 0 < length <= 4096:
            self.respond(413)
            return
        if self.server.db_path.stat().st_size > 256 * 1024 * 1024:
            self.respond(503)
            return
        try:
            payload = json.loads(self.rfile.read(length))
            with contextlib.closing(connect_db(self.server.db_path)) as db:
                with db:
                    insert_event(db, payload)
                    if time.monotonic() - self.server.last_cleanup > 3600:
                        db.execute("DELETE FROM events WHERE received_at < ?", (int(time.time()) - 30 * 86400,))
                        self.server.last_cleanup = time.monotonic()
        except (ValueError, UnicodeError, json.JSONDecodeError):
            self.respond(400)
            return
        except (sqlite3.Error, OSError):
            self.respond(503)
            return
        self.respond(204)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8622)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--origin", default="https://mc3-web.brdanpe.tech")
    args = parser.parse_args()
    if args.host not in ("127.0.0.1", "::1"):
        parser.error("telemetry must bind to loopback")
    os.umask(0o077)
    args.db.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.closing(connect_db(args.db)) as db:
        with db:
            db.execute("DELETE FROM events WHERE received_at < ?", (int(time.time()) - 30 * 86400,))
    server = HTTPServer((args.host, args.port), TelemetryHandler)
    server.db_path = args.db
    server.allowed_origin = args.origin
    server.last_cleanup = time.monotonic()
    print(f"telemetry collector on {args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
