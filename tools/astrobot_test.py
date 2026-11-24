"""Checks ASTRO BOT Rescue Mission in flat play against flat/CUSA12392.scenarios, running several
games side by side (tools/game_session.py), and writes a report with a picture of each.

    astrobot_test.py GAME.pkg [--jobs 6] [--only REGEX] [--out DIR]

Run on the host, while nobody plays: it starts its own games (their own saves) and stops them.
"""

from __future__ import annotations

import argparse
import math
import queue
import re
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from game_session import GameError, GameSession, Vec3

ROOT = Path(__file__).resolve().parent.parent
SCENARIOS = ROOT / "flat/CUSA12392.scenarios"
SETTINGS = ROOT / "flat/CUSA12392.env"
ARENA_ROOMS = "SHADPS4_VR_FLAT_ARENA_ROOMS"
SUNSHINE_LOG = Path.home() / ".config/sunshine/sunshine.log"
PLAY = re.compile(r"^play (\S+)$")
CHEST = re.compile(
    r"^open the gadget chest in (\S+) at (\S+) (\S+) (\S+) facing (\S+)(?:, then hook the ring at (\S+) (\S+) (\S+))?$"
)
SKIP = re.compile(r"^skip (.+?): (.+)$")
FOLLOWS_WITHIN = 3.0  # metres from Astro the camera stays behind him
STILL = 0.003  # metres Astro may move in a second and a half and still count as standing
VISIBLE_STEP = 0.0005  # metres the camera moves from one sample to the next, to count
TURNS_BACK = 4  # times in 2 s it may turn back (bobbing on water); a shake does so 13 times a second
TURNED = 0.2  # metres the stick swings the camera round him at least
TURNED_AT_SEAT = math.radians(10)  # or turns it where it is, at the player's seat
HELD = math.radians(3)  # in an arena (a boss's) the stick turns it no more than this
FRAME_STATS = "SHADPS4_FRAME_STATS"  # the emulator logs its frame rate every 5 s
FRAME_RATE = re.compile(r"frame stats at [0-9]+ s[^:]*: ([0-9.]+) fps")
SMOOTH = 40.0  # frames a second at least, over the last 5 s (the title runs at 60; streaming dips)
WALKED = 0.15  # metres Astro goes at least in a second and a half of the stick
WALK_TRIES = 8  # pushes of the stick, of 1.5 s each (a challenge counts down first)
IN_FRONT = 0.35  # metres from a gadget chest Astro stands to punch it
SEAT_BEHIND = 1.75  # metres from it the player's seat is put, behind Astro
SEAT_UP = 0.95  # metres above it (where the levels' seat stoppers stand)
CHEST_BODIES = 0.5  # metres around a chest its bodies lie
CLOSED_CHEST = 0x40010  # what a closed chest's bodies are; open, they are something else
PUNCHES = 3
CHEST_OPENS = 6.0  # seconds a chest takes to open once hit
STAYS_WITHIN = 2.0  # metres Astro may end up from where he stood (the punches step him forward)
BELOW_CHEST = 1.5  # metres under the chest he may end (one hangs above the floor; off a platform, further)
SEAT_NEAR = 3.0  # metres from the chest the seat (the controller) is, as in play
GADGET_FITTED = 8.0  # seconds from a chest opening to its gadget on the controller
ROPE_FLIES = 2.5  # seconds for the rope to reach a ring
RING_BODIES = 0.2  # metres around a ring its body lies
HOOKED_RING = 0x400000  # what a ring's body is while the rope holds on it (0 otherwise)
LOG_KEPT = 60  # lines of a failed game's log kept in the report
SETTLE = 2.0  # seconds for the camera to take up its place behind him
LANDING = 10.0  # seconds Astro takes at most to come to rest off the controller
CAMERA_SETTLES = 5.0  # seconds the camera takes at most to hold still after that
SAMPLE = 0.05


def distance(a: Vec3, b: Vec3) -> float:
    return math.dist(a, b)


@dataclass
class Result:
    scenario: str
    passed: list[str] = field(default_factory=list)
    failed: str = ""
    flaky: str = ""  # how it failed the first time, when it passed on its retry
    log: list[str] = field(default_factory=list)
    seconds: float = 0.0
    picture: Path | None = None


def span(session: GameSession, read: Callable[[], Vec3], seconds: float) -> float:
    """How far apart the readings of `seconds` are at most."""
    seen: list[Vec3] = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        seen.append(read())
        time.sleep(SAMPLE)
    return max(distance(a, b) for a in seen for b in seen)


def reversals(session: GameSession, read: Callable[[], Vec3], seconds: float) -> int:
    """How often the readings turn back over `seconds`, by steps of a sample to the next that
    are big enough to see: a shake goes back and forth, a camera easing after Astro does not."""
    seen: list[Vec3] = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        seen.append(read())
        time.sleep(SAMPLE)
    turns = 0
    for axis in range(3):
        steps = [b[axis] - a[axis] for a, b in zip(seen, seen[1:])]
        steps = [step for step in steps if abs(step) >= VISIBLE_STEP]
        turns += sum(1 for a, b in zip(steps, steps[1:]) if (a > 0) != (b > 0))
    return turns


def settled(session: GameSession, read: Callable[[], Vec3], within: float, timeout: float) -> float:
    """The smallest span of a 1.5 s window seen until one is within `within`, or `timeout`."""
    least = math.inf
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        least = min(least, span(session, read, 1.5))
        if least < within:
            break
    return least


def check(result: Result, what: str, ok: bool, detail: str) -> None:
    if not ok:
        raise GameError(f"{what}: {detail}")
    result.passed.append(f"{what} ({detail})")


def arenas() -> set[str]:
    """The rooms the title's settings say are built round the seat (its bosses')."""
    for line in SETTINGS.read_text().splitlines():
        name, _, value = line.partition("=")
        if name == ARENA_ROOMS:
            return set(value.split(","))
    return set()


def play(session: GameSession, level: str, result: Result) -> None:
    session.to_map()
    took = session.enter_level(level)
    check(result, "loads", True, f"{took:.0f} s from the map")
    settled(session, lambda: session.hero() or (math.inf, math.inf, math.inf), STILL, LANDING)
    hero = session.hero()
    check(result, "Astro in play", hero is not None, f"at {hero}")
    assert hero is not None
    gap = distance(session.head(), hero)
    check(result, "camera behind him", gap < FOLLOWS_WITHIN, f"{gap:.2f} m off")
    time.sleep(CAMERA_SETTLES)
    turns = reversals(session, session.head, 2.0)
    check(result, "camera steady", turns <= TURNS_BACK, f"turns back {turns} times in 2 s")
    before, heading = session.head(), session.facing()
    session.send("rx=255", 1.0)
    swung = distance(session.head(), before)
    turned = abs(math.remainder(session.facing() - heading, math.tau))
    if level in arenas():
        check(result, "in a boss's arena the view holds", turned < HELD, f"{math.degrees(turned):.0f} degrees")
    else:
        check(
            result,
            "right stick turns it",
            swung > TURNED or turned > TURNED_AT_SEAT,
            f"{swung:.2f} m round him, {math.degrees(turned):.0f} degrees",
        )
    # Some levels hold him until they start (a challenge's countdown): pushing on, as one would.
    went, end = 0.0, session.hero()
    for _ in range(WALK_TRIES):
        start = session.hero()
        session.walk(1.5)
        end = session.hero()
        went = distance(start, end) if start and end else 0.0
        if went > WALKED:
            break
    check(result, "he walks", went > WALKED, f"{went:.2f} m")
    rates = FRAME_RATE.findall(session.log.read_text(errors="replace"))
    check(result, "smooth", bool(rates) and float(rates[-1]) >= SMOOTH, f"{rates[-1] if rates else 'no'} fps")
    time.sleep(SETTLE)
    gap = distance(session.head(), end) if end else math.inf
    check(result, "camera comes along", gap < FOLLOWS_WITHIN, f"{gap:.2f} m off")


def chest_moves(chest: Vec3, facing: float) -> dict[str, Vec3]:
    """Astro's start in front of the chest (it faces him: +z turned by its heading) and the
    player's seat behind him: where it starts and every stopper that holds it."""
    front = (math.sin(facing), math.cos(facing))
    stand = (chest[0] + front[0] * IN_FRONT, chest[1] + 0.1, chest[2] + front[1] * IN_FRONT)
    seat = (chest[0] + front[0] * SEAT_BEHIND, chest[1] + SEAT_UP, chest[2] + front[1] * SEAT_BEHIND)
    return {"hero_locator": stand, "vr_player_locator*": seat, "vr_player_stopper*": seat}


def open_chest(session: GameSession, level: str, chest: Vec3, result: Result) -> None:
    """Astro in front of a gadget chest (the seat behind him), punching it: it opens where it is
    and he stays on his feet where he stood."""
    session.to_map()
    session.enter_level(level)
    settled(session, lambda: session.hero() or (math.inf, math.inf, math.inf), STILL, LANDING)
    closed = {i for i, b in session.bodies_near(chest, CHEST_BODIES).items() if b.kind == CLOSED_CHEST}
    check(result, "a closed chest there", bool(closed), f"{len(closed)} of its bodies")
    before = session.hero()
    seat = session.controller()
    check(result, "the seat by it", distance(seat, chest) < SEAT_NEAR, f"{distance(seat, chest):.2f} m off")
    for _ in range(PUNCHES):
        session.send("ly=0", 0.15)
        session.press("square", 0.2)
        time.sleep(0.6)
    time.sleep(CHEST_OPENS)
    now = session.bodies_near(chest, CHEST_BODIES + 1.0)
    still_closed = [i for i in closed if i in now and now[i].kind == CLOSED_CHEST]
    check(result, "it opens", not still_closed, f"{len(closed) - len(still_closed)} of {len(closed)} bodies open")
    after = session.hero()
    stayed = math.hypot(after[0] - before[0], after[2] - before[2]) if before and after else math.inf
    under = chest[1] - after[1] if after else math.inf
    check(
        result,
        "Astro stays",
        stayed < STAYS_WITHIN and under < BELOW_CHEST,
        f"{stayed:.2f} m off, {under:.2f} m under the chest",
    )


def hook_ring(session: GameSession, ring: Vec3, result: Result) -> None:
    """With the hook from the chest: the camera turned to the ring, a swipe forward (R1 on a
    gamepad): the rope holds on the ring."""
    time.sleep(GADGET_FITTED)
    off = session.aim_at(ring)
    check(result, "aimed at the ring", off < math.radians(10), f"{math.degrees(off):.0f} degrees off")
    session.send("gesture=swipe", 0.3)
    time.sleep(ROPE_FLIES)
    hooked = [b for b in session.bodies_near(ring, RING_BODIES).values() if b.kind == HOOKED_RING]
    check(result, "the rope holds on it", bool(hooked), f"{len(hooked)} hooked bodies there")


def worker(number: int, game: Path, jobs: queue.Queue[str], results: list[Result], out: Path) -> None:
    while True:
        try:
            scenario = jobs.get_nowait()
        except queue.Empty:
            return
        result = Result(scenario)
        if skipped := SKIP.match(scenario):
            result.passed.append(f"skipped: {skipped.group(2)}")
            results.append(result)
            print(f"skip {skipped.group(1)}: {skipped.group(2)}", flush=True)
            continue
        began = time.monotonic()
        session = GameSession(number, game, settings={FRAME_STATS: "1"})
        try:
            if found := PLAY.match(scenario):
                session.start()
                play(session, found.group(1), result)
            elif found := CHEST.match(scenario):
                level = found.group(1)
                x, y, z, facing = (float(v) for v in found.group(2, 3, 4, 5))
                session.moves = {level: chest_moves((x, y, z), math.radians(facing))}
                session.start()
                open_chest(session, level, (x, y, z), result)
                if found.group(6):
                    hook_ring(session, (float(found.group(6)), float(found.group(7)), float(found.group(8))), result)
            else:
                raise GameError("not a scenario this test knows")
        except (GameError, OSError, ValueError) as error:
            result.failed = str(error)
            if session.log.exists():
                result.log = session.log.read_text(errors="replace").splitlines()[-LOG_KEPT:]
        finally:
            result.seconds = time.monotonic() - began
            picture = out / f"{scenario.replace(' ', '_')}.png"
            try:
                if session.pid and session.alive():
                    session.screenshot(picture)
                    result.picture = picture
            except GameError:
                pass
            session.stop()
            results.append(result)
            mark = "FAIL" if result.failed else "ok  "
            print(
                f"{mark} {scenario} ({result.seconds:.0f} s){': ' + result.failed if result.failed else ''}", flush=True
            )


def report(results: list[Result], out: Path, seconds: float) -> Path:
    failed = [r for r in results if r.failed]
    flaky = [r for r in results if r.flaky]
    lines = [
        f"# Flat play checks: {len(results) - len(failed)} of {len(results)} passed"
        f" ({len(flaky)} only on a retry) in {seconds / 60:.1f} min",
        "",
    ]
    for r in sorted(results, key=lambda r: (not r.failed, not r.flaky, r.scenario)):
        mark = "FAIL" if r.failed else "flaky" if r.flaky else "ok"
        lines.append(f"## {mark}: {r.scenario} ({r.seconds:.0f} s)")
        lines += [f"- {p}" for p in r.passed]
        if r.flaky:
            lines.append(f"- failed the first time: {r.flaky}")
        if r.failed:
            lines.append(f"- **failed: {r.failed}**")
            if r.log:
                lines += ["", "```", *r.log, "```"]
        if r.picture:
            lines.append(f"\n![{r.scenario}]({r.picture.name})")
        lines.append("")
    path = out / "report.md"
    path.write_text("\n".join(lines))
    return path


def someone_playing() -> str:
    """Why the host is in use, or "": a game that is not a test's, or a stream client connected."""
    for status in Path("/proc").glob("[0-9]*/environ"):
        try:
            exe = (status.parent / "exe").resolve().name
            environ = status.read_bytes()
        except OSError:
            continue
        if exe == "shadps4" and b"XDG_DATA_HOME=/tmp/ps-river-test/" not in environ:
            return f"a game is running (pid {status.parent.name})"
    if SUNSHINE_LOG.exists():
        events = re.findall(r"CLIENT (CONNECTED|DISCONNECTED)", SUNSHINE_LOG.read_text(errors="replace"))
        if events and events[-1] == "CONNECTED":
            return "a stream client is connected"
    return ""


def main() -> None:
    parser = argparse.ArgumentParser(description="Checks Rescue Mission in flat play against its scenarios.")
    parser.add_argument("game", type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--only", default="")
    parser.add_argument("--out", type=Path, default=Path("/tmp/ps-river-test/report"))
    parser.add_argument("--force", action="store_true", help="run even while someone plays")
    args = parser.parse_args()
    if (busy := someone_playing()) and not args.force:
        raise SystemExit(f"not now: {busy}")
    scenarios = [
        line.strip()
        for line in SCENARIOS.read_text().splitlines()
        if line.strip() and not line.startswith("#") and re.search(args.only, line)
    ]
    args.out.mkdir(parents=True, exist_ok=True)
    began = time.monotonic()
    results = run(scenarios, args.game, args.jobs, args.out)
    # Six games at once now and then miss a moment; one that fails twice is a finding.
    first = {r.scenario: r for r in results if r.failed}
    if first:
        print(f"retrying {len(first)}", flush=True)
        for again in run(list(first), args.game, args.jobs, args.out):
            if not again.failed:
                again.flaky = first[again.scenario].failed
            results[results.index(first[again.scenario])] = again
    print(f"report: {report(results, args.out, time.monotonic() - began)}")


def run(scenarios: list[str], game: Path, jobs_at_once: int, out: Path) -> list[Result]:
    jobs: queue.Queue[str] = queue.Queue()
    for scenario in scenarios:
        jobs.put(scenario)
    results: list[Result] = []
    threads = [
        threading.Thread(target=worker, args=(n + 1, game, jobs, results, out))
        for n in range(min(jobs_at_once, len(scenarios)))
    ]
    for thread in threads:
        thread.start()
        time.sleep(2.0)  # starts spread out: each copies a user folder and loads the same files
    for thread in threads:
        thread.join()
    return results


if __name__ == "__main__":
    main()
