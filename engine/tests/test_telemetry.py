"""Collector boundary and report checks using an isolated SQLite database."""

import contextlib
import http.client
import json
import sqlite3
import tempfile
import threading
import unittest
from http.server import HTTPServer
from pathlib import Path

from engine.scripts.telemetry_report import load, summarize
from engine.scripts.telemetry_server import TelemetryHandler, connect_db, validate


ORIGIN = "https://mc3-web.brdanpe.tech"


def sample(**changes):
    event = {"v": 1, "session": "a" * 32, "seq": 1, "kind": "sample", "build": "test",
             "gpu": "GPU NVIDIA Pascal", "browser": "chrome/130", "os": "windows",
             "graphics": "performance", "visible": True, "frame": 100,
             "producedFps": 30.0, "screenFps": 24.0, "screenP95Ms": 120.0}
    event.update(changes)
    return event


class TelemetryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.db_path = Path(self.temp.name) / "telemetry.sqlite3"
        with contextlib.closing(connect_db(self.db_path)):
            pass
        self.server = HTTPServer(("127.0.0.1", 0), TelemetryHandler)
        self.server.db_path = self.db_path
        self.server.allowed_origin = ORIGIN
        self.server.last_cleanup = 0
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def post(self, event, origin=ORIGIN):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        body = json.dumps(event).encode()
        connection.request("POST", "/v1/telemetry", body,
                           {"Origin": origin, "Content-Type": "application/json"})
        response = connection.getresponse()
        status = response.status
        response.read()
        connection.close()
        return status

    def test_ingest_dedup_and_visible_report(self):
        self.assertEqual(self.post(sample()), 204)
        self.assertEqual(self.post(sample()), 204)
        self.assertEqual(self.post(sample(seq=2, visible=False, screenFps=3.0)), 204)
        self.assertEqual(self.post(sample(seq=3, kind="halt", message="gpu-device-lost")), 204)
        self.assertEqual(self.post(sample(seq=4, trigger="low-fps", windowMs=5000,
                                          screenFps=28.0, emuIdlePct=4.0,
                                          discFetches=2, discReadyMisses=1,
                                          longTaskCount=1, longTaskMs=120.0)), 204)
        samples, errors, sessions, halts = load(self.db_path, 24)
        result = summarize(samples, errors, sessions, halts)
        self.assertEqual((result["sessions"], result["samples"], result["visible_samples"]), (1, 3, 2))
        self.assertEqual((result["low_fps_samples"], result["halt_events"]), (2, 1))
        self.assertEqual((result["periodic_samples"], result["alert_samples"]), (1, 1))
        self.assertEqual(result["by_gpu"][0]["median_screen_fps"], 24.0)
        self.assertEqual(result["problem_sessions"][0]["stutter_samples"], 2)
        self.assertEqual(samples[-1]["long_task_ms"], 120.0)

    def test_rejects_foreign_origin_extra_fields_and_free_text(self):
        self.assertEqual(self.post(sample(), "https://other.example"), 403)
        self.assertEqual(self.post(sample(room="SECRET")), 400)
        self.assertEqual(self.post(sample(kind="error", message="My name is Alice")), 400)
        self.assertEqual(self.post(sample(trigger=[])), 400)
        with self.assertRaises(ValueError):
            validate(sample(screenFps=float("nan")))
        samples, errors, sessions, halts = load(self.db_path, 24)
        self.assertEqual((samples, errors, sessions, halts), ([], [], 0, 0))

    def test_migrates_existing_events_without_losing_samples(self):
        legacy = Path(self.temp.name) / "legacy.sqlite3"
        with contextlib.closing(sqlite3.connect(legacy)) as db:
            db.execute("""CREATE TABLE events (
                id INTEGER PRIMARY KEY, received_at INTEGER NOT NULL, session TEXT NOT NULL,
                seq INTEGER NOT NULL, kind TEXT NOT NULL, build TEXT NOT NULL, gpu TEXT NOT NULL,
                browser TEXT NOT NULL, os TEXT NOT NULL, graphics TEXT NOT NULL, visible INTEGER NOT NULL,
                frame INTEGER, produced_frames INTEGER, shown_frames INTEGER,
                produced_fps REAL, screen_fps REAL, screen_p95_ms REAL,
                gs_work_ms REAL, present_wait_ms REAL, gpu_estimate_ms REAL,
                disc_wait_ms REAL, audio_ms REAL, heap_mib REAL, primitives_delta INTEGER,
                message TEXT, UNIQUE(session, seq))""")
            db.execute("""INSERT INTO events
                (received_at,session,seq,kind,build,gpu,browser,os,graphics,visible,screen_fps,produced_fps)
                VALUES (strftime('%s','now'),?,?,?,?,?,?,?,?,?,?,?)""",
                       ("b" * 32, 1, "sample", "old", "GPU NVIDIA Pascal", "chrome/130",
                        "windows", "performance", 1, 29.0, 30.0))
            db.commit()
        with contextlib.closing(connect_db(legacy)) as db:
            row = db.execute("SELECT trigger,screen_fps FROM events").fetchone()
        self.assertEqual(row, ("periodic", 29.0))
        samples, errors, sessions, halts = load(legacy, 24)
        self.assertEqual((len(samples), sessions, samples[0]["trigger"]), (1, 1, "periodic"))


if __name__ == "__main__":
    unittest.main()
