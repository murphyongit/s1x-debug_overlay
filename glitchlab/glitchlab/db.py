"""SQLite store for missions, experiments, observations, discoveries, verification runs, bookmarks and routes.

Rules (from the project brief):
* every experiment has a unique id and is never edited in place - a changed input sequence is a NEW
  experiment with parent_id pointing at the old one;
* negative results are kept;
* discoveries move through stages: candidate -> recorded -> reproduced -> validated -> speedrun_viable.
"""
from __future__ import annotations

import json
import sqlite3
import time
import uuid
from pathlib import Path

STAGES = ("candidate", "recorded", "reproduced", "validated", "speedrun_viable", "rejected")
CATEGORIES = ("movement", "collision_oob", "sequence_break", "checkpoint", "script_trigger", "route")

SCHEMA = """
CREATE TABLE IF NOT EXISTS missions (
    map TEXT PRIMARY KEY,           -- game file name, e.g. recovery
    display_name TEXT,
    notes TEXT
);
CREATE TABLE IF NOT EXISTS experiments (
    id TEXT PRIMARY KEY,
    parent_id TEXT REFERENCES experiments(id),
    created REAL NOT NULL,
    map TEXT REFERENCES missions(map),
    source TEXT NOT NULL,           -- 'recorder_log', 'static_miner', 'simulator', 'lab'
    category TEXT,
    hypothesis TEXT,
    game_version TEXT,
    tool_version TEXT,
    start_conditions TEXT,          -- JSON
    inputs TEXT,                    -- JSON (input sequence + timing) or NULL
    outcome TEXT,                   -- 'positive', 'negative', 'inconclusive', 'error', or NULL while pending
    novelty REAL,
    anomaly REAL,
    speedrun_utility REAL,
    notes TEXT
);
CREATE TABLE IF NOT EXISTS observations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    experiment_id TEXT NOT NULL REFERENCES experiments(id),
    t REAL,
    kind TEXT NOT NULL,             -- 'position', 'event', 'trigger', 'flag', 'metric', ...
    data TEXT                       -- JSON
);
CREATE TABLE IF NOT EXISTS discoveries (
    id TEXT PRIMARY KEY,
    created REAL NOT NULL,
    map TEXT REFERENCES missions(map),
    category TEXT NOT NULL,
    title TEXT NOT NULL,
    stage TEXT NOT NULL DEFAULT 'candidate',
    score REAL,                     -- priority score from the miner that found it
    evidence TEXT,                  -- JSON: what in the data supports it
    test_plan TEXT,                 -- how to test it in game
    location TEXT,                  -- JSON [x, y, z] when it has one
    fingerprint TEXT UNIQUE,        -- stable key so re-running a miner doesn't duplicate it
    notes TEXT
);
CREATE TABLE IF NOT EXISTS discovery_experiments (
    discovery_id TEXT NOT NULL REFERENCES discoveries(id),
    experiment_id TEXT NOT NULL REFERENCES experiments(id),
    PRIMARY KEY (discovery_id, experiment_id)
);
CREATE TABLE IF NOT EXISTS verification_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    discovery_id TEXT NOT NULL REFERENCES discoveries(id),
    experiment_id TEXT REFERENCES experiments(id),
    created REAL NOT NULL,
    reproduced INTEGER,             -- 1 yes, 0 no, NULL unknown
    notes TEXT
);
CREATE TABLE IF NOT EXISTS bookmarks (
    id TEXT PRIMARY KEY,
    created REAL NOT NULL,
    map TEXT REFERENCES missions(map),
    label TEXT,
    position TEXT,                  -- JSON [x, y, z]
    angles TEXT,                    -- JSON [pitch, yaw]
    velocity TEXT,                  -- JSON [vx, vy, vz] or NULL
    replay_experiment_id TEXT REFERENCES experiments(id),
    replay_until REAL,
    notes TEXT
);
CREATE TABLE IF NOT EXISTS routes (
    id TEXT PRIMARY KEY,
    created REAL NOT NULL,
    map TEXT REFERENCES missions(map),
    name TEXT,
    baseline INTEGER DEFAULT 0,
    duration REAL,
    points TEXT,                    -- JSON list of [t, x, y, z]
    source_experiment_id TEXT REFERENCES experiments(id),
    notes TEXT
);
CREATE INDEX IF NOT EXISTS idx_obs_exp ON observations(experiment_id);
CREATE INDEX IF NOT EXISTS idx_disc_map ON discoveries(map, stage);
"""


def _id() -> str:
    return uuid.uuid4().hex


class Store:
    def __init__(self, path: str | Path = ":memory:"):
        self.path = str(path)
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self.con = sqlite3.connect(self.path)
        self.con.execute("PRAGMA foreign_keys = ON")
        self.con.executescript(SCHEMA)

    def close(self) -> None:
        self.con.close()

    # missions
    def ensure_mission(self, map_name: str, display_name: str | None = None) -> None:
        self.con.execute("INSERT OR IGNORE INTO missions(map, display_name) VALUES (?, ?)", (map_name, display_name))
        if display_name:
            self.con.execute("UPDATE missions SET display_name=? WHERE map=?", (display_name, map_name))
        self.con.commit()

    # experiments
    def add_experiment(self, map_name: str, source: str, *, parent_id: str | None = None, category: str | None = None,
                       hypothesis: str | None = None, game_version: str | None = None, tool_version: str | None = None,
                       start_conditions: dict | None = None, inputs: object | None = None, outcome: str | None = None,
                       novelty: float | None = None, anomaly: float | None = None, speedrun_utility: float | None = None,
                       notes: str | None = None) -> str:
        if category is not None and category not in CATEGORIES:
            raise ValueError(f"unknown category {category}")
        self.ensure_mission(map_name)
        eid = _id()
        self.con.execute(
            "INSERT INTO experiments VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (eid, parent_id, time.time(), map_name, source, category, hypothesis, game_version, tool_version,
             json.dumps(start_conditions) if start_conditions is not None else None,
             json.dumps(inputs) if inputs is not None else None, outcome, novelty, anomaly, speedrun_utility, notes))
        self.con.commit()
        return eid

    def derive_experiment(self, parent_id: str, **changes) -> str:
        """A new experiment based on an old one (the old one is never modified)."""
        row = self.con.execute("SELECT map, source, category, hypothesis, game_version, tool_version, start_conditions, "
                               "inputs FROM experiments WHERE id=?", (parent_id,)).fetchone()
        if row is None:
            raise KeyError(parent_id)
        base = dict(map_name=row[0], source=row[1], category=row[2], hypothesis=row[3], game_version=row[4],
                    tool_version=row[5], start_conditions=json.loads(row[6]) if row[6] else None,
                    inputs=json.loads(row[7]) if row[7] else None)
        base.update(changes)
        m = base.pop("map_name")
        s = base.pop("source")
        return self.add_experiment(m, s, parent_id=parent_id, **base)

    def set_outcome(self, experiment_id: str, outcome: str, notes: str | None = None) -> None:
        """Outcome is filled in once (pending -> result); inputs and conditions stay untouched."""
        cur = self.con.execute("SELECT outcome FROM experiments WHERE id=?", (experiment_id,)).fetchone()
        if cur is None:
            raise KeyError(experiment_id)
        if cur[0] is not None:
            raise ValueError("experiment already has an outcome; derive a new experiment instead")
        self.con.execute("UPDATE experiments SET outcome=?, notes=COALESCE(?, notes) WHERE id=?",
                         (outcome, notes, experiment_id))
        self.con.commit()

    def add_observations(self, experiment_id: str, rows: list[tuple[float | None, str, object]]) -> None:
        self.con.executemany("INSERT INTO observations(experiment_id, t, kind, data) VALUES (?,?,?,?)",
                             [(experiment_id, t, k, json.dumps(d)) for t, k, d in rows])
        self.con.commit()

    # discoveries
    def upsert_discovery(self, map_name: str, category: str, title: str, fingerprint: str, *, score: float | None = None,
                         evidence: object | None = None, test_plan: str | None = None,
                         location: list[float] | None = None, notes: str | None = None) -> str:
        if category not in CATEGORIES:
            raise ValueError(f"unknown category {category}")
        self.ensure_mission(map_name)
        row = self.con.execute("SELECT id FROM discoveries WHERE fingerprint=?", (fingerprint,)).fetchone()
        if row:
            self.con.execute("UPDATE discoveries SET title=?, score=?, evidence=?, test_plan=?, location=? WHERE id=?",
                             (title, score, json.dumps(evidence), test_plan,
                              json.dumps(location) if location else None, row[0]))
            self.con.commit()
            return row[0]
        did = _id()
        self.con.execute("INSERT INTO discoveries VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                         (did, time.time(), map_name, category, title, "candidate", score, json.dumps(evidence),
                          test_plan, json.dumps(location) if location else None, fingerprint, notes))
        self.con.commit()
        return did

    def set_stage(self, discovery_id: str, stage: str) -> None:
        if stage not in STAGES:
            raise ValueError(stage)
        self.con.execute("UPDATE discoveries SET stage=? WHERE id=?", (stage, discovery_id))
        self.con.commit()

    def link(self, discovery_id: str, experiment_id: str) -> None:
        self.con.execute("INSERT OR IGNORE INTO discovery_experiments VALUES (?,?)", (discovery_id, experiment_id))
        self.con.commit()

    def add_verification(self, discovery_id: str, reproduced: bool | None, experiment_id: str | None = None,
                         notes: str | None = None) -> None:
        self.con.execute("INSERT INTO verification_runs(discovery_id, experiment_id, created, reproduced, notes) "
                         "VALUES (?,?,?,?,?)", (discovery_id, experiment_id, time.time(),
                                                None if reproduced is None else int(reproduced), notes))
        self.con.commit()

    def discoveries(self, map_name: str | None = None) -> list[dict]:
        q = "SELECT id, map, category, title, stage, score, evidence, test_plan, location FROM discoveries"
        args: tuple = ()
        if map_name:
            q += " WHERE map=?"
            args = (map_name,)
        q += " ORDER BY score DESC"
        cols = ("id", "map", "category", "title", "stage", "score", "evidence", "test_plan", "location")
        out = []
        for r in self.con.execute(q, args):
            d = dict(zip(cols, r))
            d["evidence"] = json.loads(d["evidence"]) if d["evidence"] else None
            d["location"] = json.loads(d["location"]) if d["location"] else None
            out.append(d)
        return out

    # bookmarks and routes
    def add_bookmark(self, map_name: str, label: str, position, angles=None, velocity=None,
                     replay_experiment_id: str | None = None, replay_until: float | None = None,
                     notes: str | None = None) -> str:
        self.ensure_mission(map_name)
        bid = _id()
        self.con.execute("INSERT INTO bookmarks VALUES (?,?,?,?,?,?,?,?,?,?)",
                         (bid, time.time(), map_name, label, json.dumps(list(position)),
                          json.dumps(list(angles)) if angles else None, json.dumps(list(velocity)) if velocity else None,
                          replay_experiment_id, replay_until, notes))
        self.con.commit()
        return bid

    def add_route(self, map_name: str, name: str, points: list, *, baseline: bool = False,
                  source_experiment_id: str | None = None, notes: str | None = None) -> str:
        self.ensure_mission(map_name)
        rid = _id()
        duration = points[-1][0] - points[0][0] if points else None
        self.con.execute("INSERT INTO routes VALUES (?,?,?,?,?,?,?,?,?)",
                         (rid, time.time(), map_name, name, int(baseline), duration, json.dumps(points),
                          source_experiment_id, notes))
        self.con.commit()
        return rid
