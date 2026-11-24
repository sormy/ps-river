"""Writes Sunshine's apps.json: the desktop, and every game in a folder as its own app, named
after its title (param.sfo), played with play.sh (which sets the monitor to the client's
resolution). A game is a pkg (played straight from it, its update found by play.sh) or an
unpacked folder; a title that is both is played from its pkg.

    python3 host/sunshine_apps.py <games folder> <ps-river root> > apps.json
"""

import json
import struct
import subprocess
import sys
from pathlib import Path

SFO_MAGIC = b"\0PSF"
SFO_ENTRY = 16
SFO_ENTRIES_AT = 20


def sfo_value(path: Path, wanted: str) -> str | None:
    data = path.read_bytes()
    if data[:4] != SFO_MAGIC:
        return None
    keys, values, count = struct.unpack_from("<III", data, 8)
    for i in range(count):
        key_at, _, length, _, value_at = struct.unpack_from("<HHIII", data, SFO_ENTRIES_AT + i * SFO_ENTRY)
        key = data[keys + key_at : data.index(b"\0", keys + key_at)].decode()
        if key == wanted:
            return data[values + value_at : values + value_at + length].rstrip(b"\0").decode()
    return None


def pkg_value(tool: Path, package: Path, key: str) -> str | None:
    result = subprocess.run([str(tool), "sfo", str(package), key], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def folders(folder: Path) -> dict[str, tuple[str, Path]]:
    """Unpacked games by title id: (title, folder)."""
    found = {}
    for game in sorted(folder.iterdir()):
        sfo = game / "sce_sys" / "param.sfo"
        if game.name.endswith(("-UPDATE", "-mods")) or not (game / "eboot.bin").is_file() or not sfo.is_file():
            continue
        title, title_id = sfo_value(sfo, "TITLE"), sfo_value(sfo, "TITLE_ID")
        if title is not None and title_id is not None:
            found[title_id] = (title, game)
    return found


def packages(folder: Path, tool: Path) -> dict[str, tuple[str, Path]]:
    """Games (not updates: CATEGORY gd) in pkgs by title id: (title, pkg)."""
    found = {}
    if not tool.is_file():
        return found
    for package in sorted(folder.glob("*.pkg")):
        if pkg_value(tool, package, "CATEGORY") != "gd":
            continue
        title, title_id = pkg_value(tool, package, "TITLE"), pkg_value(tool, package, "TITLE_ID")
        if title is not None and title_id is not None:
            found[title_id] = (title, package)
    return found


def main() -> None:
    folder, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    games = folders(folder) | packages(folder, root / ".build" / "out" / "pspkg")
    apps = [{"name": "Desktop", "image-path": "desktop.png"}]
    for title, game in sorted(games.values()):
        apps.append({"name": title, "cmd": f"{root / 'play.sh'} {game}", "auto-detach": "false"})
    json.dump({"env": {}, "apps": apps}, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
