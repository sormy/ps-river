"""One run of ASTRO BOT Rescue Mission (CUSA12392, 1.04) on the host, for tests: its own user
folder (a copy of the real one, so the real save is never touched), run folder and scripted
input, so that several run side by side. It reads the game's memory and log to know
where the game is, instead of waiting fixed times.

    session = GameSession(1, game_pkg)
    session.start()
    session.enter_level("cave_canyons_half1")
    session.relocate({"hero_locator": (0.0, 0.3, 16.25)})
    session.restart_level()
"""

from __future__ import annotations

import math
import os
import re
import shutil
import struct
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
REAL_USER_DIR = Path.home() / ".local/share/shadPS4"
# Left out of each run's copy of the user folder: big, or its own.
NOT_COPIED = {"cache", "log", "rt_dump", "captures", "screenshots"}
DISPLAY = ":1"
PAGE = 4096
CHUNK_PAGES = 4096
GUEST_LOW, GUEST_HIGH = 0x100000000, 0x1000000000  # where the game's own memory lies

# The game's memory (CUSA12392 1.04).
# World 5 at -54 degrees to World 1 at 85 (each with its pitch), the dock ahead.
MAP_STEPS = "-54:13,-16:10,0,18:22,53:2,85:22"
GAME_POINTER = 0x2ECFE38  # from the main module's base: the game's main object
GAME_WORLD = 0x10  # its physics world; the world's +0x8 wraps the library
WORLD_INNER = 0x8
INNER_SCALE = 0x58  # float: the library's units in a metre
INNER_LIBRARY = 0x20
LIBRARY_BODY_COUNT = 0x1A0
LIBRARY_BODIES = 0x1A8
BODY_SIZE = 0xA0
BODY_MOTION = 0x1
BODY_KIND = 0x8  # what the body is, for collision filters
BODY_POSITION = 0x40
HEAD_FRONT_BODY = 0
HEAD_BODY = 1
HEAD_BACK_BODY = 2
AIM_STEPS = 30
AIMED = math.radians(4)
AIM_PULSE = 0.4  # seconds of the stick at most at a time
AIM_SPEED = 1.0  # radians a second the stick turns the view, about
HERO_BODY = 5
CONTROLLER_BODY = 7
MOTION_ACTIVE = 1
AT_ORIGIN = 0.01  # metres: a hero this near the origin is not placed yet
MOVES = 0.1  # metres Astro goes for a push of the stick once he can be played
FIRST_LEVEL = "world0_0_new_intro_1"  # World 1-1 (ROOFTOPS), the entry every level is entered by
FIRST_LEVEL_NAME = b"ROOFTOPS\x00"
SHORT_STRING = 15  # characters the game's strings keep inline
LEVEL_FILE = re.compile(r"levels/([^/]+)/lvx/design\.lvx")
TRANSLATE = re.compile(rb"<translate>([^<]*)</translate>")
LEVEL_OBJECT = re.compile(r"(<MayaNode>([^<]*)</MayaNode>\s*<translate>)([^<]*)(</translate>)")
LEVELS = "data/multi_platformer/levels"
PKG_TOOL = ROOT / ".build/out/pspkg"
MODULE_BASE = re.compile(r"Loading module eboot\.bin to (0x[0-9a-f]+)")

Vec3 = tuple[float, float, float]


class GameError(RuntimeError):
    pass


@dataclass(frozen=True)
class Body:
    motion: int
    kind: int
    position: Vec3


def moved_level(text: str, moves: dict[str, Vec3]) -> str:
    """A level's design file with objects moved: each named one (a MayaNode path's last part, or
    `name*` for all that start so) put at its place, in metres."""

    def place(found: re.Match[str]) -> str:
        name = found.group(2).rsplit("|", 1)[-1]
        for wanted, (x, y, z) in moves.items():
            if name == wanted or (wanted.endswith("*") and name.startswith(wanted[:-1])):
                return f"{found.group(1)}{x} {y} {z}{found.group(4)}"
        return found.group(0)

    return LEVEL_OBJECT.sub(place, text)


def wait_for(test: Callable[[], bool], timeout: float, what: str, step: float = 0.05) -> None:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if test():
            return
        time.sleep(step)
    raise GameError(f"timed out waiting for {what}")


@dataclass
class Press:
    start: float
    hold: float
    tokens: str


@dataclass
class GameSession:
    number: int
    game: Path
    overlay: Path | None = None
    mods: Path | None = None
    # Levels to lay over the game with objects moved: {level: {object name: place}} (see moved_level).
    moves: dict[str, dict[str, Vec3]] = field(default_factory=dict)
    settings: dict[str, str] = field(default_factory=dict)  # more of the emulator's environment
    base: Path = field(init=False)
    pid: int = field(init=False, default=0)
    presses: list[Press] = field(init=False, default_factory=list)
    module_base: int = field(init=False, default=0)

    def __post_init__(self) -> None:
        self.base = Path(f"/tmp/ps-river-test/{self.number}")

    # --- running it ---

    @property
    def unit(self) -> str:
        return f"ps-river-test-{self.number}"

    @property
    def user_dir(self) -> Path:
        return self.base / "data/shadPS4"

    @property
    def log(self) -> Path:
        return self.user_dir / "log/shad_log.txt"

    @property
    def live_script(self) -> Path:
        return self.base / "live.script"

    def start(self) -> None:
        self.stop()
        if self.base.exists():
            shutil.rmtree(self.base)
        self.user_dir.mkdir(parents=True)
        for entry in REAL_USER_DIR.iterdir():
            if entry.name in NOT_COPIED:
                continue
            target = self.user_dir / entry.name
            if entry.is_dir():
                shutil.copytree(entry, target, symlinks=True)
            else:
                shutil.copy2(entry, target)
        overlay = self.overlay
        if self.moves:
            overlay = self.base / "overlay"
            for level, moves in self.moves.items():
                target = overlay / LEVELS / level / "lvx/design.lvx"
                target.parent.mkdir(parents=True)
                target.write_text(moved_level(self.level_file(level), moves))
        (self.base / "input.script").write_text("0 0\n")
        self.live_script.write_text("0 0\n")
        env = {
            "DISPLAY": DISPLAY,
            "XDG_DATA_HOME": str(self.base / "data"),
            "RUN_DIR": str(self.base / "run"),
            "FULLSCREEN": "false",
            "SHADPS4_INPUT_SCRIPT": str(self.base / "input.script"),
            "SHADPS4_LIVE_INPUT": str(self.live_script),
            "SHADPS4_SHOT_ON": str(self.base / "shot.request"),
            # The map's steps between worlds, which the way into a level counts on (play has them
            # off: flat/CUSA12392.env).
            "SHADPS4_VR_FLAT_MENU_ROOMS": "world_select",
            "SHADPS4_VR_FLAT_MENU_STEPS": MAP_STEPS,
            **self.settings,
        }
        if overlay:
            env["GAME_OVERLAY"] = str(overlay)
        if self.mods:
            env["GAME_MODS"] = str(self.mods)
        subprocess.run(["systemctl", "--user", "reset-failed", self.unit], capture_output=True)
        subprocess.run(
            ["systemd-run", "--user", "--unit", self.unit, f"--working-directory={ROOT}"]
            + [f"--setenv={k}={v}" for k, v in env.items()]
            + [str(ROOT / "play.sh"), str(self.game)],
            check=True,
            capture_output=True,
        )
        wait_for(self.find_process, 60, "the emulator to start")
        wait_for(lambda: self.read_module_base() != 0, 60, "the game to load")

    def level_file(self, level: str) -> str:
        """A level's design file, from the newest of the title's packages that has it."""
        packages = sorted(self.game.parent.glob("*.pkg"), key=lambda p: "update" not in p.name.lower())
        for package in packages:
            out = subprocess.run(
                [str(PKG_TOOL), "cat", str(package), f"{LEVELS}/{level}/lvx/design.lvx"], capture_output=True
            )
            if out.returncode == 0 and out.stdout:
                return out.stdout.decode(errors="replace")
        raise GameError(f"no package has {level}'s design file")

    def stop(self) -> None:
        subprocess.run(["systemctl", "--user", "stop", self.unit], capture_output=True)

    def alive(self) -> bool:
        return subprocess.run(["systemctl", "--user", "is-active", "-q", self.unit]).returncode == 0

    def find_process(self) -> bool:
        out = subprocess.run(
            ["systemctl", "--user", "show", "-p", "MainPID", "--value", self.unit], capture_output=True, text=True
        ).stdout.strip()
        if out.isdigit() and out != "0" and Path(f"/proc/{out}/exe").resolve().name == "shadps4":
            self.pid = int(out)
            return True
        return False

    def read_module_base(self) -> int:
        if self.log.exists():
            found = MODULE_BASE.search(self.log.read_text(errors="replace"))
            if found:
                self.module_base = int(found.group(1), 16)
        return self.module_base

    # --- input (scripted, by the emulator's clock) ---

    def clock(self) -> float:
        """Seconds since the emulator started: what its input script counts from."""
        start_ticks = int(Path(f"/proc/{self.pid}/stat").read_text().rsplit(")", 1)[1].split()[19])
        uptime = float(Path("/proc/uptime").read_text().split()[0])
        return uptime - start_ticks / os.sysconf("SC_CLK_TCK")

    def send(self, tokens: str, hold: float = 0.15, after: float = 0.05) -> None:
        """Holds what `tokens` say (buttons, sticks as lx=0..255, gesture=swipe) for `hold` s."""
        now = self.clock()
        self.presses = [p for p in self.presses if p.start + p.hold > now]
        self.presses.append(Press(now + after, hold, tokens))
        lines = ["0 0"] + [f"{p.start:.3f} {p.hold:.3f} {p.tokens}" for p in self.presses]
        self.live_script.write_text("\n".join(lines) + "\n")
        time.sleep(after + hold + 0.05)

    def press(self, button: str, hold: float = 0.15) -> None:
        self.send(button, hold)

    def walk(self, seconds: float, forward: bool = True) -> None:
        self.send("ly=0" if forward else "ly=255", seconds)

    # --- the game's memory ---

    def read(self, address: int, size: int) -> bytes:
        with open(f"/proc/{self.pid}/mem", "rb", 0) as mem:
            mem.seek(address)
            return mem.read(size)

    def u64(self, address: int) -> int:
        return struct.unpack("<Q", self.read(address, 8))[0]

    def world(self) -> int:
        try:
            return self.u64(self.u64(self.module_base + GAME_POINTER) + GAME_WORLD)
        except (OSError, struct.error, ValueError, OverflowError):
            return 0

    def body(self, index: int) -> tuple[int, Vec3]:
        """A body of the current world: its motion type and position in metres."""
        inner = self.u64(self.world() + WORLD_INNER)
        scale = struct.unpack("<f", self.read(inner + INNER_SCALE, 4))[0]
        at = self.u64(self.u64(inner + INNER_LIBRARY) + LIBRARY_BODIES) + index * BODY_SIZE
        motion = self.read(at + BODY_MOTION, 1)[0]
        x, y, z = struct.unpack("<3f", self.read(at + BODY_POSITION, 12))
        return motion, (x / scale, y / scale, z / scale)

    def bodies_near(self, point: Vec3, radius: float) -> dict[int, Body]:
        """The current world's bodies within `radius` metres of `point`, by index."""
        inner = self.u64(self.world() + WORLD_INNER)
        scale = struct.unpack("<f", self.read(inner + INNER_SCALE, 4))[0]
        library = self.u64(inner + INNER_LIBRARY)
        count = struct.unpack("<I", self.read(library + LIBRARY_BODY_COUNT, 4))[0]
        table = self.read(self.u64(library + LIBRARY_BODIES), count * BODY_SIZE)
        found: dict[int, Body] = {}
        for index in range(count):
            at = index * BODY_SIZE
            x, y, z = struct.unpack_from("<3f", table, at + BODY_POSITION)
            position = (x / scale, y / scale, z / scale)
            if math.dist(position, point) < radius:
                kind = struct.unpack_from("<I", table, at + BODY_KIND)[0]
                found[index] = Body(table[at + BODY_MOTION], kind, position)
        return found

    def hero(self) -> Vec3 | None:
        try:
            motion, at = self.body(HERO_BODY)
        except (OSError, struct.error, ValueError, OverflowError, ZeroDivisionError):
            return None
        # While a level starts he waits at its origin, not yet in play.
        return at if motion == MOTION_ACTIVE and math.hypot(*at) > AT_ORIGIN else None

    def controller(self) -> Vec3:
        return self.body(CONTROLLER_BODY)[1]

    def head(self) -> Vec3:
        """The player's head in the hero's world: flat play's camera."""
        return self.body(HEAD_BODY)[1]

    def facing(self) -> float:
        """Which way the head looks, as a heading in radians."""
        front, back = self.body(HEAD_FRONT_BODY)[1], self.body(HEAD_BACK_BODY)[1]
        return math.atan2(front[0] - back[0], front[2] - back[2])

    def looking(self) -> tuple[float, float]:
        """The head's heading and pitch (up positive), in radians."""
        front, back = self.body(HEAD_FRONT_BODY)[1], self.body(HEAD_BACK_BODY)[1]
        across = math.hypot(front[0] - back[0], front[2] - back[2])
        return math.atan2(front[0] - back[0], front[2] - back[2]), math.atan2(front[1] - back[1], across)

    def aim_at(self, point: Vec3) -> float:
        """Turns the camera with the right stick until it looks at `point` (the controller points
        where it looks); returns how far off it ended, in radians."""
        off = math.inf
        for _ in range(AIM_STEPS):
            head = self.head()
            heading = math.atan2(point[0] - head[0], point[2] - head[2])
            pitch = math.atan2(point[1] - head[1], math.hypot(point[0] - head[0], point[2] - head[2]))
            now_heading, now_pitch = self.looking()
            turn = math.remainder(heading - now_heading, math.tau)
            tilt = pitch - now_pitch
            off = math.hypot(turn, tilt)
            if off < AIMED:
                break
            # Stick right turns the view right (heading down); up looks up.
            if abs(turn) >= abs(tilt):
                self.send("rx=0" if turn > 0 else "rx=255", min(AIM_PULSE, abs(turn) / AIM_SPEED))
            else:
                self.send("ry=0" if tilt > 0 else "ry=255", min(AIM_PULSE, abs(tilt) / AIM_SPEED))
            time.sleep(0.3)
        return off

    def levels_opened(self) -> list[str]:
        return LEVEL_FILE.findall(self.log.read_text(errors="replace")) if self.log.exists() else []

    def resident(self) -> list[tuple[int, int]]:
        """The game's writable memory that is backed by pages: (start, size) runs."""
        runs: list[tuple[int, int]] = []
        with open(f"/proc/{self.pid}/maps") as maps, open(f"/proc/{self.pid}/pagemap", "rb") as pagemap:
            for line in maps:
                parts = line.split()
                start, end = (int(x, 16) for x in parts[0].split("-"))
                if start < GUEST_LOW or end > GUEST_HIGH or not parts[1].startswith("rw"):
                    continue
                for chunk in range(start, end, CHUNK_PAGES * PAGE):
                    count = min(CHUNK_PAGES, (end - chunk) // PAGE)
                    pagemap.seek(chunk // PAGE * 8)
                    entries = np.frombuffer(pagemap.read(count * 8), dtype=np.uint64)
                    present = np.flatnonzero((entries >> np.uint64(63)) & np.uint64(1))
                    if present.size == 0:
                        continue
                    for run in np.split(present, np.flatnonzero(np.diff(present) != 1) + 1):
                        runs.append((chunk + int(run[0]) * PAGE, int(run.size) * PAGE))
        return runs

    def scan(self, visit: Callable[[int, bytes], None]) -> None:
        with open(f"/proc/{self.pid}/mem", "rb", 0) as mem:
            for start, size in self.resident():
                try:
                    mem.seek(start)
                    visit(start, mem.read(size))
                except OSError:
                    continue

    def write(self, address: int, data: bytes) -> None:
        with open(f"/proc/{self.pid}/mem", "r+b", 0) as mem:
            mem.seek(address)
            mem.write(data)

    # --- the screen ---

    def screenshot(self, path: Path) -> None:
        """The game's picture, taken by the emulator (SHADPS4_SHOT_ON): its window shares the
        screen with the others' and may not be the one on it."""
        shots = self.user_dir / "screenshots"
        before = set(shots.glob("*.png")) if shots.exists() else set()
        (self.base / "shot.request").touch()
        wait_for(lambda: shots.exists() and bool(set(shots.glob("*.png")) - before), 10, "the screenshot")
        (taken,) = set(shots.glob("*.png")) - before
        wait_for(lambda: taken.stat().st_size > 0, 5, "the screenshot to be written")
        time.sleep(0.2)
        shutil.move(taken, path)

    # --- getting about ---

    def to_map(self) -> None:
        """From the start: past the title and the save screen to the world map, on World 1."""
        # The title takes Cross once it is up: until then nothing happens.
        wait_for(lambda: self.world() != 0, 120, "the game's first world")
        boot = set(self.worlds_seen(2.0))
        for _ in range(60):
            self.press("cross")
            if self.wait_world_change(boot, 3.0):
                break
        else:
            raise GameError("the title did not go on to the map")
        time.sleep(4.0)
        self.press("cross")
        time.sleep(8.0)
        for button in ("left", "left"):
            self.press(button)
            time.sleep(1.5)
        self.send("lx=0", 0.4)
        time.sleep(2.0)

    def worlds_seen(self, seconds: float) -> list[int]:
        seen: list[int] = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            w = self.world()
            if w and w not in seen:
                seen.append(w)
            time.sleep(0.05)
        return seen

    def wait_world_change(self, known: set[int], timeout: float) -> bool:
        try:
            wait_for(lambda: self.world() not in known and self.world() != 0, timeout, "a new world")
            return True
        except GameError:
            return False

    def enter_level(self, level: str) -> float:
        """From World 1 on the map: into `level` (any), by World 1-1's entry. Seconds taken."""
        began = time.monotonic()
        opened = self.levels_opened().count(level)
        if self.set_first_level(level, FIRST_LEVEL) == 0:
            raise GameError("World 1-1's entry was not found")
        try:
            known = set(self.worlds_seen(0.5))
            self.press("cross")
            self.wait_world_change(known, 8.0)
            time.sleep(1.0)
            for _ in range(10):
                self.press("cross")
                try:
                    wait_for(lambda: self.levels_opened().count(level) > opened, 6.0, f"{level} to open")
                    break
                except GameError:
                    continue
            else:
                raise GameError(f"{level} did not open")
        finally:
            self.set_first_level(FIRST_LEVEL, level)
        self.wait_in_play()
        return time.monotonic() - began

    def wait_in_play(self) -> None:
        """Until Astro can be played: past his arrival (on the controller, mostly), when the
        stick moves him. Cross lets him off."""
        wait_for(lambda: self.hero() is not None, 120, "Astro in the level")
        for _ in range(30):
            self.press("cross")
            time.sleep(0.5)
            before = self.hero()
            self.send("lx=0", 0.3)
            after = self.hero()
            if before and after and math.dist(before, after) > MOVES:
                return
        raise GameError("the stick never moved Astro")

    def restart_level(self) -> None:
        """The pause menu's Restart: the level from its start, as the level text in memory says."""
        before = self.hero()
        self.press("options")
        time.sleep(1.5)
        self.press("right")
        time.sleep(0.5)
        self.press("cross")
        wait_for(lambda: self.hero() != before, 30, "the level to restart")
        self.wait_in_play()

    def set_first_level(self, folder: str, current: str) -> int:
        """Points World 1-1's entries (the game's level list) from `current`'s folder to `folder`."""
        new, old = folder.encode(), current.encode()
        writes: list[tuple[int, bytes]] = []

        def visit(start: int, data: bytes) -> None:
            i = data.find(FIRST_LEVEL_NAME)
            while i != -1:
                if 0x28 <= i <= len(data) - 0x20:
                    name_size, name_cap = struct.unpack_from("<QQ", data, i + 0x10)
                    size, cap = struct.unpack_from("<QQ", data, i - 0x18)
                    if name_size == 8 and name_cap == SHORT_STRING and size == len(old) and len(new) <= cap:
                        at = start + i - 0x28
                        text = struct.unpack_from("<Q", data, i - 0x28)[0] if cap > SHORT_STRING else at
                        try:
                            if self.read(text, size) == old:
                                writes.append((text, new + b"\x00"))
                                writes.append((at + 0x10, struct.pack("<Q", len(new))))
                        except OSError:
                            pass
                i = data.find(FIRST_LEVEL_NAME, i + 1)

        self.scan(visit)
        for address, data in writes:
            self.write(address, data)
        return len(writes) // 2

    def relocate(self, moves: dict[str, Vec3]) -> int:
        """Moves level objects (hero_locator: Astro's start, vr_player_stopper_*: the seat's) in the
        level text the game keeps, for the next restart."""
        writes: list[tuple[int, bytes]] = []

        def visit(start: int, data: bytes) -> None:
            for name, (x, y, z) in moves.items():
                needle = f"|{name}</MayaNode>".encode()
                i = data.find(needle)
                while i != -1:
                    found = TRANSLATE.search(data, i, i + 200)
                    if found:
                        old = found.group(1)
                        new = f"{x} {y} {z}".encode()
                        if len(new) <= len(old):
                            writes.append((start + found.start(1), new.ljust(len(old))))
                    i = data.find(needle, i + 1)

        self.scan(visit)
        for address, data in writes:
            self.write(address, data)
        return len(writes)
