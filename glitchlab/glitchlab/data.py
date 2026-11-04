"""Loaders for everything the overlay/recorder saves.

All of these are read-only parsers of files that already exist on the user's PC (or copies of them):

* recorder run logs        speedrun_data/<map>_<date>_<time>.txt
* map entity text          map_ents_0.txt                (saved by the overlay at mission start)
* brush / wall export      speedrun_data/brushes_<map>.txt  (overlay v1.17+)
* full collision dump      speedrun_data/collision_full_<map>.bin (overlay v1.24+)
* movement settings        speedrun_data/movement_<map>.txt  (overlay v1.24+)

Nothing here talks to the game.
"""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

# Script-string ids the overlay writes as keys in map_ents_0.txt (checked against known triggers in recovery).
KEY_CLASSNAME = "171"
KEY_ORIGIN = "742"
KEY_MODEL = "672"
KEY_TARGETNAME = "1191"
KEY_SCRIPT_FLAG = "31182"        # flag the trigger sets (or clears, for *_flag_clear)
KEY_SCRIPT_FLAG_TRUE = "31188"   # trigger is switched off (parked 10000 down) until this flag is set
KEY_SCRIPT_FLAG_FALSE = "31185"  # trigger is switched off once this flag is set
PARK_OFFSET = 10000.0


# ---------------------------------------------------------------------------------------------- run logs
@dataclass
class Sample:
    t: float          # log time (seconds since the log started)
    x: float
    y: float
    z: float
    pitch: float
    yaw: float
    flags: int = 0    # movement flags word (hex in the log)
    stance: int = 0
    view_height: float = 60.0
    game_ms: int | None = None   # game clock (ms); goes backwards on a checkpoint reload


@dataclass
class TriggerInfo:
    slot: int
    cls: str
    name: str
    flags: str
    mid: tuple[float, float, float]
    size: tuple[float, float, float] | None


@dataclass
class RunLog:
    path: Path
    map: str = ""
    tool_version: str = ""
    samples: list[Sample] = field(default_factory=list)
    triggers: dict[int, TriggerInfo] = field(default_factory=dict)   # TRIG lines at start
    events: list[tuple[float, str, str]] = field(default_factory=list)  # (t, kind, rest of line)
    reloads: list[float] = field(default_factory=list)

    def notifies(self, name: str) -> list[float]:
        return [t for t, k, rest in self.events if k == "SNOTIFY" and rest.split()[0] == name]


_TRIG_RE = re.compile(
    r"#(\d+) (\S+) name=(\S+)(?: flags=(.*?))? at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)(?: size (\d+) (\d+) (\d+))?")


def parse_trigger_rest(rest: str) -> TriggerInfo | None:
    m = _TRIG_RE.match(rest)
    if not m:
        return None
    slot, cls, name, flags, x, y, z, sx, sy, sz = m.groups()
    size = (float(sx), float(sy), float(sz)) if sx else None
    return TriggerInfo(int(slot), cls, name, flags or "", (float(x), float(y), float(z)), size)


def load_run_log(path: str | Path) -> RunLog:
    path = Path(path)
    log = RunLog(path)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ", 2)
            if len(parts) < 2:
                continue
            try:
                t = float(parts[0])
            except ValueError:
                continue
            kind = parts[1]
            rest = parts[2] if len(parts) > 2 else ""
            if kind == "P":
                v = rest.split()
                if len(v) < 5:
                    continue
                try:
                    s = Sample(t, float(v[0]), float(v[1]), float(v[2]), float(v[3]), float(v[4]))
                    if len(v) >= 8:
                        s.flags = int(v[5], 16)
                        s.stance = int(v[6])
                        s.view_height = float(v[7])
                    if len(v) >= 9:
                        s.game_ms = int(v[8])
                except ValueError:
                    continue
                if s.x == 0.0 and s.y == 0.0 and s.z == 0.0:
                    continue  # reload moment, game reports zeros
                log.samples.append(s)
            elif kind == "RUN":
                m = re.search(r"tool version (\S+)\s+map (\S+)", rest)
                if m:
                    log.tool_version, log.map = m.group(1), m.group(2)
            elif kind == "TRIG":
                ti = parse_trigger_rest(rest)
                if ti:
                    log.triggers[ti.slot] = ti
            elif kind == "RELOAD":
                log.reloads.append(t)
                log.events.append((t, kind, rest))
            elif kind not in ("COLUMNS",):
                log.events.append((t, kind, rest))
    return log


# ---------------------------------------------------------------------------------------------- map entities
@dataclass
class MapEntity:
    index: int
    keys: dict[str, str]

    @property
    def classname(self) -> str:
        return self.keys.get(KEY_CLASSNAME, "")

    @property
    def origin(self) -> tuple[float, float, float] | None:
        o = self.keys.get(KEY_ORIGIN)
        if not o:
            return None
        v = o.split()
        try:
            return float(v[0]), float(v[1]), float(v[2])
        except (ValueError, IndexError):
            return None

    @property
    def script_flag(self) -> str:
        return self.keys.get(KEY_SCRIPT_FLAG, "")

    @property
    def flag_true(self) -> str:
        return self.keys.get(KEY_SCRIPT_FLAG_TRUE, "")

    @property
    def flag_false(self) -> str:
        return self.keys.get(KEY_SCRIPT_FLAG_FALSE, "")

    @property
    def targetname(self) -> str:
        return self.keys.get(KEY_TARGETNAME, "")


def load_map_ents(path: str | Path) -> list[MapEntity]:
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    out: list[MapEntity] = []
    for i, block in enumerate(text.split("{")[1:]):
        keys: dict[str, str] = {}
        for line in block.split("}")[0].splitlines():
            m = re.match(r'\s*(\S+) "(.*)"\s*$', line)
            if m:
                keys[m.group(1)] = m.group(2)
        if keys:
            out.append(MapEntity(i, keys))
    return out


# ---------------------------------------------------------------------------------------------- brush export (text)
@dataclass
class WallPiece:
    cls: int                 # 1 invisible wall, 2 mantle, 3 visible solid (bounds only), 4 brush-model collision
    contents: int
    lo: tuple[float, float, float]
    hi: tuple[float, float, float]
    faces: list[list[tuple[float, float, float]]] = field(default_factory=list)
    edges: list[tuple[tuple[float, float, float], tuple[float, float, float]]] = field(default_factory=list)
    model: bool = False


def load_brushes_txt(path: str | Path) -> list[WallPiece]:
    out: list[WallPiece] = []
    cur: WallPiece | None = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("B "):
                p = line.split()
                cur = WallPiece(int(p[1]), int(p[2], 16), tuple(map(float, p[3:6])), tuple(map(float, p[6:9])),
                                model=line.rstrip().endswith("model"))
                out.append(cur)
            elif line.startswith("F ") and cur is not None:
                v = list(map(float, line.split()[1:]))
                cur.faces.append([tuple(v[i:i + 3]) for i in range(0, len(v) - 2, 3)])
            elif line.startswith("E ") and cur is not None:
                v = list(map(float, line.split()[1:]))
                cur.edges.append((tuple(v[0:3]), tuple(v[3:6])))
    return out


# ---------------------------------------------------------------------------------------------- full collision (binary)
@dataclass
class Material:
    index: int
    name: str
    contents: int
    surface_flags: int


@dataclass
class FullCollision:
    materials: list[Material]
    # brush faces: (contents, material index or -1, polygon corners)
    brush_faces: list[tuple[int, int, list[tuple[float, float, float]]]]
    brush_count: int
    # triangles: (material index, contents, model index or -1, 3 corners)
    triangles: list[tuple[int, int, int, tuple[tuple[float, float, float], ...]]]
    # one entry per brush: (contents, index of its first face in brush_faces, number of faces)
    brushes: list[tuple[int, int, int]] = field(default_factory=list)


def load_collision_full(path: str | Path) -> FullCollision:
    """Reads the overlay's collision_full_<map>.bin (format written by core.cpp v1.24+)."""
    data = Path(path).read_bytes()
    if data[:6] != b"AWCOL1":
        raise ValueError(f"{path}: not an AWCOL1 file")
    off = 8
    (mcount,) = struct.unpack_from("<I", data, off)
    off += 4
    mats = []
    for i in range(mcount):
        contents, sflags = struct.unpack_from("<II", data, off)
        name = data[off + 8: off + 72].split(b"\0", 1)[0].decode("ascii", "replace")
        mats.append(Material(i, name, contents, sflags))
        off += 72
    faces, tris, brushes, ranges = [], [], 0, []
    while off < len(data):
        tag = data[off]
        off += 1
        if tag == ord("E"):
            break
        if tag == ord("B"):
            contents, nf = struct.unpack_from("<IH", data, off)
            off += 6
            brushes += 1
            ranges.append((contents, len(faces), nf))
            for _ in range(nf):
                mat, k = struct.unpack_from("<HB", data, off)
                off += 3
                pts = [struct.unpack_from("<3f", data, off + 12 * j) for j in range(k)]
                off += 12 * k
                faces.append((contents, -1 if mat == 0xFFFF else mat, pts))
        elif tag == ord("T"):
            mat, contents, model = struct.unpack_from("<HIh", data, off)
            off += 8
            pts = tuple(struct.unpack_from("<3f", data, off + 12 * j) for j in range(3))
            off += 36
            tris.append((mat, contents, model, pts))
        else:
            raise ValueError(f"{path}: unknown record tag {tag!r} at byte {off - 1}")
    return FullCollision(mats, faces, brushes, tris, ranges)


def load_movement(path: str | Path) -> dict[str, float]:
    out: dict[str, float] = {}
    for line in Path(path).read_text(errors="replace").splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] == "float":
            try:
                out[p[0]] = float(p[2])
            except ValueError:
                pass
    return out
