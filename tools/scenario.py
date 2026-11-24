"""Test scenarios: a level's design file with the hero starting elsewhere, to be laid over the
game (play.sh's GAME_OVERLAY), so a moment deep in a level (its goal, a headbutt board) can be
reached without playing through it.

    scenario.py GAME LEVEL X Y Z OVERLAY [--safe] [NAME@X,Y,Z ...]
    scenario.py list GAME LEVEL

GAME is the unpacked game folder (its -UPDATE folder is preferred when it has the level), LEVEL
a folder under data/multi_platformer/levels, X Y Z the hero's start in the level's metres.
NAME@X,Y,Z moves a level object as well. The player's seat follows the hero along the level
but is held at each vr_player_stopper_* until the hero walks through its trigger box
(tb_stopper_*_off): move that box to the hero's start to have the seat come along.
--safe also writes OVERLAY.mods, edits for play.sh's GAME_MODS that keep the hero from being
hurt (invincible after his first spawn or knock-down) and from running out of lives. `list` prints a level's invisible triggers, stoppers and locators.
"""

import re
import sys
from pathlib import Path

LEVELS = Path("data/multi_platformer/levels")
SETTINGS = "data/multi_platformer/multi_platformer.xml"
FOREVER = "1000000.000000"
SAFE = {"InvincibleTimeAfterRespawn": FOREVER, "InvincibleTimeAfterKnockDown": FOREVER, "LifeNum": "99"}
OBJECT = re.compile(r"<MayaNode>([^<]*)</MayaNode>\s*<translate>([^<]*)</translate>")
STAGING = re.compile(r"\|(tb_[^|]*|vr_player_[^|]*|hero_locator)$")
Vec = tuple[float, float, float]


def locator(name: str) -> re.Pattern[str]:
    return re.compile(rf"(<MayaNode>[^<]*\|{name}</MayaNode>\s*<translate>)[^<]*(</translate>)")


HERO_START = locator("hero_locator")


def game_file(game: Path, relative: Path) -> Path:
    update = game.with_name(game.name + "-UPDATE") / relative
    return update if update.is_file() else game / relative


def design_file(game: Path, level: str) -> Path:
    return game_file(game, LEVELS / level / "lvx" / "design.lvx")


def safe_edits() -> str:
    """Edits in play.sh's GAME_MODS form (laid on top of the title's own edits)."""
    return "".join(f"{SETTINGS} | {tag} | {value}\n" for tag, value in SAFE.items())


def staging(design: str) -> list[tuple[str, str]]:
    """The level's invisible triggers, the player's seat stoppers and the start locators."""
    return [(name.split("|")[-1], place) for name, place in OBJECT.findall(design) if STAGING.search(name)]


def moved(design: str, start: re.Pattern[str], at: Vec) -> str:
    text, count = start.subn(lambda m: f"{m[1]}{at[0]} {at[1]} {at[2]}{m[2]}", design)
    if count != 1:
        raise ValueError(f"expected one {start.pattern}, found {count}")
    return text


def main(argv: list[str]) -> None:
    if len(argv) == 4 and argv[1] == "list":
        for name, place in staging(design_file(Path(argv[2]), argv[3]).read_text(encoding="utf-8")):
            print(f"{name:40} {' '.join(f'{float(v):8.2f}' for v in place.split()[:3])}")
        return
    if len(argv) < 7:
        sys.exit(__doc__)
    game, level, overlay = Path(argv[1]), argv[2], Path(argv[6])
    at: Vec = (float(argv[3]), float(argv[4]), float(argv[5]))
    source = design_file(game, level)
    target = overlay / LEVELS / level / "lvx" / "design.lvx"
    target.parent.mkdir(parents=True, exist_ok=True)
    design = moved(source.read_text(encoding="utf-8"), HERO_START, at)
    if "--safe" in argv[7:]:
        edits = overlay.with_name(overlay.name + ".mods")
        edits.write_text(safe_edits(), encoding="utf-8")
        print(f"the hero cannot be hurt or run out of lives with GAME_MODS={edits}")
    for move in (a for a in argv[7:] if a != "--safe"):
        name, _, place = move.partition("@")
        x, y, z = (float(v) for v in place.split(","))
        design = moved(design, locator(re.escape(name)), (x, y, z))
    target.write_text(design, encoding="utf-8")
    print(f"{level}: the hero starts at {at} ({target})")


if __name__ == "__main__":
    main(sys.argv)
