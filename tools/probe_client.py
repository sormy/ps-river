"""Client helpers for tools/probe_server.py (run on the host, next to the server), plus lookups
in ASTRO BOT Rescue Mission 1.04's physics world (docs/rescue-mission.md)."""

import json
import urllib.request
from http.client import HTTPResponse

URL = "http://localhost:8765"
BASE = 0x800000000
GAME_POINTER = 0x2ECFE38
BODY_SIZE, BODY_POSITION = 0xA0, 0x40


Reply = dict[str, object]


def reply(response: HTTPResponse) -> Reply:
    loaded: object = json.load(response)
    if not isinstance(loaded, dict):
        raise ValueError(f"not a JSON object: {loaded!r}")
    return {str(key): value for key, value in loaded.items()}


def get(path: str) -> Reply:
    return reply(urllib.request.urlopen(URL + path))


def post(path: str, body: Reply) -> Reply:
    request = urllib.request.Request(URL + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    return reply(urllib.request.urlopen(request))


def read(address: int, kind: str, count: int = 1) -> list[int | float]:
    values = get(f"/read?address={address:#x}&type={kind}&count={count}")["values"]
    numbers = [value for value in values if isinstance(value, (int, float))] if isinstance(values, list) else []
    if len(numbers) != count:
        raise ValueError(f"{count} values expected at {address:#x}: {values!r}")
    return numbers


def u64(address: int) -> int:
    return int(read(address, "u64")[0])


def u32(address: int) -> int:
    return int(read(address, "u32")[0])


def f32(address: int, count: int = 1) -> list[float]:
    return [float(value) for value in read(address, "f32", count)]


def bodies_base() -> tuple[int, int]:
    """The current physics world's bodies (it flips between rooms' worlds) and their count."""
    game = u64(BASE + GAME_POINTER)
    inner = u64(u64(game + 0x10) + 0x8)
    library = u64(inner + 0x20)
    return u64(library + 0x1A8), u32(library + 0x1A0)


def body_position(index: int) -> list[float]:
    bodies, _ = bodies_base()
    return f32(bodies + index * BODY_SIZE + BODY_POSITION, 3)
