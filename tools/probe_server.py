"""HTTP probe into a running emulated game, for reverse engineering: snapshots of its resident
memory kept in RAM, Cheat-Engine-style candidate filtering between snapshots, reads and writes,
key presses and screenshots. Linux only (/proc/<pid>/mem); run as root on the host, bound to
localhost, and reach it through an SSH tunnel.

    sudo python3 tools/probe_server.py --process '^/path/to/shadps4' --display :1
    ssh -L 8765:localhost:8765 host

Endpoints (JSON in and out):
    GET  /status                      process, resident size, snapshots, candidates
    POST /snapshot {name}             capture resident memory as float32
    POST /candidates/clear            forget the candidate set, keep the snapshots
    POST /candidates {rule, a, b, ...} start (no current set) or narrow the candidate set:
         rule: changed | unchanged | increased | decreased | delta (value, tolerance) | between (low, high)
    GET  /candidates?limit=N          current candidates with their values in every snapshot
    GET  /read?address=0x..&count=N&type=f32|u32|u64|bytes
    POST /write {address, type, values}
    POST /key {key, action: tap|down|up, hold_ms}
    GET  /screenshot                  PNG of the display
"""

import argparse
import json
import os
import struct
import subprocess
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import FileIO
from urllib.parse import parse_qs, urlparse

import numpy as np

PAGE = 4096
CHUNK_PAGES = 4096
GUEST_LOW, GUEST_HIGH = 0x100000000, 0x1000000000
TYPES = {"f32": ("f", 4), "u32": ("I", 4), "u64": ("Q", 8), "s32": ("i", 4)}
CHANGE_TOLERANCE, DELTA_TOLERANCE = 1e-4, 1e-3

# JSON objects in and out.
Reply = dict[str, object]
Body = dict[str, object]


def text(body: Body, key: str, default: str | None = None) -> str:
    value = body.get(key, default)
    if not isinstance(value, str):
        raise ValueError(f"{key}: a string expected")
    return value


def number(body: Body, key: str, default: float | None = None) -> float:
    value = body.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{key}: a number expected")
    return float(value)


def numbers(body: Body, key: str) -> list[int | float]:
    values = body.get(key)
    picked = [value for value in values if isinstance(value, (int, float))] if isinstance(values, list) else []
    if not isinstance(values, list) or len(picked) != len(values):
        raise ValueError(f"{key}: a list of numbers expected")
    return picked


@dataclass(frozen=True)
class Rule:
    """How snapshot a's values (to b's, when given) narrow the candidates."""

    kind: str
    a: str
    b: str | None
    tolerance: float | None
    value: float
    low: float
    high: float

    @staticmethod
    def read(body: Body) -> "Rule":
        return Rule(
            kind=text(body, "rule"),
            a=text(body, "a"),
            b=text(body, "b") if body.get("b") is not None else None,
            tolerance=number(body, "tolerance") if "tolerance" in body else None,
            value=number(body, "value", 0.0),
            low=number(body, "low", 0.0),
            high=number(body, "high", 0.0),
        )


@dataclass
class Probe:
    process_pattern: str
    display: str
    snapshots: dict[str, list[np.ndarray]] = field(default_factory=dict)
    runs: list[tuple[int, int]] = field(default_factory=list)
    candidates: np.ndarray | None = None  # guest addresses
    lock: threading.Lock = field(default_factory=threading.Lock)

    def pid(self) -> int:
        out = subprocess.check_output(["pgrep", "-f", self.process_pattern])
        return int(out.split()[0])

    def resident_runs(self) -> list[tuple[int, int]]:
        pid = self.pid()
        runs = []
        with open(f"/proc/{pid}/maps") as maps, open(f"/proc/{pid}/pagemap", "rb") as pagemap:
            for line in maps:
                parts = line.split()
                start, end = (int(x, 16) for x in parts[0].split("-"))
                if start < GUEST_LOW or end > GUEST_HIGH or not parts[1].startswith("rw"):
                    continue
                for chunk in range(start, end, CHUNK_PAGES * PAGE):
                    count = min(CHUNK_PAGES, (end - chunk) // PAGE)
                    pagemap.seek(chunk // PAGE * 8)
                    entries = np.frombuffer(pagemap.read(count * 8), dtype=np.uint64)
                    idx = np.flatnonzero((entries >> np.uint64(63)) & np.uint64(1))
                    for run in np.split(idx, np.flatnonzero(np.diff(idx) != 1) + 1) if idx.size else []:
                        runs.append((chunk + int(run[0]) * PAGE, int(run.size) * PAGE))
        return runs

    def mem(self, mode: str = "r") -> FileIO:
        return FileIO(f"/proc/{self.pid()}/mem", mode)

    def snapshot(self, name: str) -> Reply:
        with self.lock:
            if not self.snapshots:
                self.runs = self.resident_runs()
            arrays = []
            with self.mem() as mem:
                for start, length in self.runs:
                    mem.seek(start)
                    data = mem.read(length)
                    arrays.append(np.frombuffer(data.ljust(length, b"\0"), dtype=np.float32).copy())
            self.snapshots[name] = arrays
            return {"name": name, "bytes": sum(l for _, l in self.runs)}

    def _values(self, name: str, addresses: np.ndarray) -> np.ndarray:
        starts = np.array([s for s, _ in self.runs], dtype=np.uint64)
        which = np.searchsorted(starts, addresses, side="right") - 1
        out = np.empty(addresses.size, dtype=np.float32)
        arrays = self.snapshots[name]
        for run in np.unique(which):
            sel = which == run
            out[sel] = arrays[run][((addresses[sel] - starts[run]) // 4).astype(np.int64)]
        return out

    def narrow(self, rule: Rule) -> Reply:
        with self.lock:
            a, b = rule.a, rule.b
            if self.candidates is None:
                picked = []
                for (start, _), x, y in zip(
                    self.runs, self.snapshots[a], self.snapshots[b] if b else self.snapshots[a]
                ):
                    mask = self._mask(rule, x, y)
                    picked.append(start + np.flatnonzero(mask).astype(np.uint64) * 4)
                self.candidates = np.concatenate(picked) if picked else np.empty(0, np.uint64)
            else:
                x = self._values(a, self.candidates)
                y = self._values(b, self.candidates) if b else x
                self.candidates = self.candidates[self._mask(rule, x, y)]
            return {"candidates": int(self.candidates.size)}

    @staticmethod
    def _mask(rule: Rule, x: np.ndarray, y: np.ndarray) -> np.ndarray:
        finite = np.isfinite(x) & np.isfinite(y) & (np.abs(x) < 1e6) & (np.abs(y) < 1e6)
        kind = rule.kind
        change = CHANGE_TOLERANCE if rule.tolerance is None else rule.tolerance
        with np.errstate(invalid="ignore"):
            if kind == "changed":
                return finite & (np.abs(y - x) > change)
            if kind == "unchanged":
                return finite & (np.abs(y - x) <= change)
            if kind == "increased":
                return finite & (y - x > change)
            if kind == "decreased":
                return finite & (x - y > change)
            if kind == "delta":
                delta = DELTA_TOLERANCE if rule.tolerance is None else rule.tolerance
                return finite & (np.abs((y - x) - rule.value) <= delta)
            if kind == "between":
                return finite & (x >= rule.low) & (x <= rule.high)
        raise ValueError(f"unknown rule {kind}")

    def list_candidates(self, limit: int) -> Reply:
        with self.lock:
            if self.candidates is None:
                return {"candidates": []}
            picked = self.candidates[:limit]
            values = {n: self._values(n, picked).tolist() for n in self.snapshots}
            return {
                "total": int(self.candidates.size),
                "candidates": [
                    {"address": hex(int(a)), **{n: values[n][i] for n in values}} for i, a in enumerate(picked)
                ],
            }

    def read(self, address: int, count: int, kind: str) -> Reply:
        with self.mem() as mem:
            if kind == "bytes":
                mem.seek(address)
                return {"hex": mem.read(count).hex()}
            code, size = TYPES[kind]
            mem.seek(address)
            return {"values": list(struct.unpack(f"<{count}{code}", mem.read(count * size)))}

    def write(self, address: int, kind: str, values: list[int | float]) -> Reply:
        code, _ = TYPES[kind]
        with self.mem("r+") as mem:
            mem.seek(address)
            mem.write(struct.pack(f"<{len(values)}{code}", *values))
        return {"written": len(values)}

    def key(self, name: str, action: str, hold_ms: int) -> Reply:
        env = {**os.environ, "DISPLAY": self.display}
        if action == "tap" and hold_ms:
            subprocess.run(["xdotool", "keydown", name], env=env, check=True)
            threading.Event().wait(hold_ms / 1000)
            subprocess.run(["xdotool", "keyup", name], env=env, check=True)
        else:
            subprocess.run(
                ["xdotool", {"tap": "key", "down": "keydown", "up": "keyup"}[action], name], env=env, check=True
            )
        return {"key": name, "action": action}

    def screenshot(self) -> bytes:
        env = {**os.environ, "DISPLAY": self.display}
        return subprocess.check_output(["import", "-window", "root", "png:-"], env=env)


def make_handler(probe: Probe):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, body: bytes, content_type: str = "application/json", status: int = 200) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj: Reply, status: int = 200) -> None:
            self._send(json.dumps(obj).encode(), status=status)

        def _body(self) -> Body:
            length = int(self.headers.get("Content-Length") or 0)
            loaded: object = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(loaded, dict):
                raise ValueError("a JSON object expected")
            return {str(key): value for key, value in loaded.items()}

        def do_GET(self) -> None:
            url = urlparse(self.path)
            query = {k: v[0] for k, v in parse_qs(url.query).items()}
            try:
                if url.path == "/status":
                    self._json(
                        {
                            "pid": probe.pid(),
                            "resident_bytes": sum(l for _, l in probe.runs),
                            "snapshots": list(probe.snapshots),
                            "candidates": None if probe.candidates is None else int(probe.candidates.size),
                        }
                    )
                elif url.path == "/candidates":
                    self._json(probe.list_candidates(int(query.get("limit", 100))))
                elif url.path == "/read":
                    self._json(
                        probe.read(int(query["address"], 0), int(query.get("count", 1)), query.get("type", "f32"))
                    )
                elif url.path == "/screenshot":
                    self._send(probe.screenshot(), "image/png")
                else:
                    self._json({"error": "not found"}, 404)
            except Exception as error:  # report to the client instead of dropping the connection
                self._json({"error": repr(error)}, 500)

        def do_POST(self) -> None:
            url = urlparse(self.path)
            try:
                body = self._body()
                if url.path == "/snapshot":
                    self._json(probe.snapshot(text(body, "name")))
                elif url.path == "/candidates":
                    self._json(probe.narrow(Rule.read(body)))
                elif url.path == "/candidates/clear":
                    probe.candidates = None
                    self._json({"candidates": None})
                elif url.path == "/reset":
                    probe.snapshots.clear()
                    probe.candidates = None
                    probe.runs = []
                    self._json({"reset": True})
                elif url.path == "/write":
                    self._json(
                        probe.write(int(text(body, "address"), 0), text(body, "type", "f32"), numbers(body, "values"))
                    )
                elif url.path == "/key":
                    self._json(
                        probe.key(text(body, "key"), text(body, "action", "tap"), int(number(body, "hold_ms", 0)))
                    )
                else:
                    self._json({"error": "not found"}, 404)
            except Exception as error:
                self._json({"error": repr(error)}, 500)

        def log_message(self, format: str, *args: object) -> None:
            pass

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--process", required=True, help="pgrep -f pattern of the emulator")
    parser.add_argument("--display", default=":1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    probe = Probe(args.process, args.display)
    ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(probe)).serve_forever()


if __name__ == "__main__":
    main()
