# ps-river

Play a PS4 game on a TV without a headset, even a PlayStation VR-only one. The game runs in an
emulator on a headless Linux PC and streams to the TV over the home network. You play with a
normal controller.

```
Linux PC (GPU, emulator, Sunshine)  ──LAN──▶  Moonlight (Apple TV / Mac / …)  ◀──BT──  controller
```

The example throughout is ASTRO BOT Rescue Mission (`CUSA12392`).

## The pieces

[Sunshine](https://github.com/LizardByte/Sunshine) streams the PC's screen and takes the
controller back; [Moonlight](https://moonlight-stream.org) is the app on the TV side. The
emulator is [shadPS4](https://github.com/shadps4-emu/shadPS4), at a commit pinned in
`build.sh`, with these patches (`patches/shadps4/`):

| Patch | From | Adds |
| --- | --- | --- |
| 0001 | [shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64) | the port's general fixes (libraries, video output, renderer), without its ARM64 parts |
| 0002 | [AstroQuest](https://github.com/bigmak94/AstroQuest) | PSVR emulation and Astro Bot's fixes |
| 0003 | ours | games played straight from pkg files |
| 0004 | ours | flat play: a VR game on a normal screen |
| 0005 | ours | flat play draws only the eye it shows |
| 0006 | ours | screenshots on request, for the tests |

Licensed GPL-2.0-or-later, like shadPS4 (`LICENSE`). Also here: `play.sh` starts a game, `flat/` holds per-game settings, `host/` sets up streaming,
and `tools/pkg` reads pkg files. Design notes are in `docs/`.

## 1. Dump the game

You need a decrypted dump of a game you own: a folder with `eboot.bin`, `sce_sys/` and
`sce_module/`, or a pkg built from one.

- Files copied off a PS4 or PS5 drive are encrypted and won't run. Store pkgs are too. A dump
  comes from a jailbroken console.
- Updates and DLC are separate dumps. Put an update next to the game as `<TITLE>-UPDATE`.
- The VR fixes cover Astro Bot `CUSA12392` and `CUSA12431`, versions 1.00 and 1.04. For other
  games, check the [shadPS4 compatibility list](https://github.com/shadps4-compatibility/shadps4-game-compatibility).

## 2. Prepare the Linux host

Any x86-64 Linux with a Vulkan 1.3 GPU. The scripts assume Debian 13 and an NVIDIA card.

```sh
sudo apt install git rsync cmake ninja-build clang lld libc++-dev libc++abi-dev \
  build-essential libx11-dev libxext-dev libwayland-dev libdecor-0-dev libxkbcommon-dev \
  libxcursor-dev libxi-dev libxss-dev libxtst-dev libxrandr-dev libxfixes-dev \
  libasound2-dev libpulse-dev libopenal-dev libudev-dev uuid-dev libvulkan-dev libgl-dev
sudo mkdir -p /opt/ps-river && sudo chown "$USER" /opt/ps-river
rsync -a --exclude .build --exclude games --exclude .git ./ host:/opt/ps-river/   # from your machine
```

## 3. Build the emulator

```sh
cd /opt/ps-river && ./build.sh
```

This fetches shadPS4 into `.build/src`, applies the patches (one commit each), builds
`.build/out/shadps4` and runs the unit tests. Every run starts clean. The first one downloads
about 1 GB. macOS can't build this fork, so build on the host.

To change a patch, edit `.build/src`, fold the change into that patch's commit, and write the
patches back:

```sh
git -C .build/src commit -a --fixup=<the patch's commit>
git -C .build/src -c sequence.editor=: rebase -q -i --autosquash <UPSTREAM_COMMIT>
tools/export_patches.sh
```

To move to a newer upstream, change `UPSTREAM_COMMIT` and rebase the patches, 0001 first.
Expect most conflicts in the renderer.

## 4. Put the game on the host

Play straight from the pkg, or unpack it into `games/`:

```sh
./unpack.sh games/game.pkg                        # -> games/CUSA12392
./unpack.sh games/update.pkg CUSA12392-UPDATE
```

- Only pkgs built from a dump open, not store pkgs.
- `unpack.sh` checks that every file the pkg lists is really there. A copy missing files
  crashes the game much later, often at the first logo.
- A pkg on a network share is cached locally, up to 100 GB, so later starts read nothing over
  the network.
- The pkg reader also works on its own, on Linux or macOS: `tools/pkg/build.sh` builds
  `pspkg`, which lists and extracts pkgs.

## 5. Run it on the host

```sh
./play.sh games/game.pkg            # flat, from the pkg (an update next to it is used too)
./play.sh games/CUSA12392           # flat, from an unpacked folder
FLAT=0 ./play.sh games/CUSA12392    # in a headset
```

### What flat play does

- The game is told a PSVR is connected and worn, so its checks pass. The screen shows one eye.
- In a level, the camera follows Astro like a normal platformer camera. It pulls back in open
  spaces, turns side-on on side-scrolling stretches, keeps a boss in view, watches a fall from
  above, and fades to black before a respawn. Details: `docs/camera-port.md`.
- In menus, the camera stays at the player's seat and the right stick looks around. On the
  world map, look at a world and press ✕.

| Controller | Keyboard | Does |
| --- | --- | --- |
| Left stick | WASD | move |
| Right stick | IJKL | turn the camera (levels), look around (menus) |
| ✕ | N | jump |
| R1 | U | swipe forward on the touchpad |
| Touchpad click | | the touchpad; also blows on a dandelion in front of you |
| L2, held | | camera behind the controller, for aiming gadgets |
| Tilt the controller | | fine-aim gadgets (experimental, see `GYRO_AIM`) |
| D-pad left/right | arrows | lean to dodge; step between worlds or levels on the maps |
| OPTIONS | Enter | pause |

### Settings

Each game's settings are in `flat/<TITLE>.env`, one `SHADPS4_*=value` per line. A variable
already set in the environment wins. Changes to the game's own data files go in
`flat/<TITLE>.mods` as `file | tag | value` lines; they apply in flat play only, and the game
itself is never changed.

| `SHADPS4_VR_FLAT_…` | Default | Meaning |
| --- | --- | --- |
| `FOLLOW` | 1 | 0 keeps the camera at the seat in levels too |
| `SCALE` | 1 | size of the hero, in metres, that the camera's distances are measured in |
| `PITCH` | 13 | how far the camera looks down at rest, in degrees |
| `SWING` | 90 | right-stick turn speed, in degrees a second |
| `AUTO_TURN` | 60 | how fast it turns behind a hero running across the view (0: never) |
| `ZOOM` | 1 | how much the picture is enlarged at the widest view |
| `GYRO_AIM` | 0 | 1: tilting the controller fine-aims gadgets |
| `ONE_EYE` | 1 | 0: draw the hidden eye too (about 60% more GPU power) |
| `MONO` | 0 | 1: the game's own 1080p flat picture, with its own camera |

The room lists (`ARENA_ROOMS`, `OVERLAY_ROOMS`, `MENU_ROOMS`, `MENU_STEPS`) tell the camera
about a game's boss rooms, overlays and maps. They are explained where they are read, in
patch 0004's `core/vr/flat_mode.h`.

A non-VR game needs none of this: flat play does nothing there. Following the hero needs code
for that game that finds him; without it, the camera stays at the seat.

### Testing

```sh
python3 tools/astrobot_test.py games/game.pkg --jobs 6
```

This plays each level listed in `flat/CUSA12392.scenarios`: it enters the level from the world
map and checks that the camera follows Astro, turns with the stick, and the game runs smoothly.
Several games run side by side, each with its own copy of the save. The report, with a
screenshot of each level, is `/tmp/ps-river-test/report/report.md`.

It won't start while a game is running or someone is streaming (`--force` overrides that);
`--only PATTERN` runs only matching scenarios.

## 6. Make the host a streaming box

```sh
./host/setup.sh   # run again after adding a game
```

It sets up the following, all starting at boot with nobody logged in:
- a virtual monitor on the NVIDIA GPU, switched to the client's resolution for each stream, up
  to 4K;
- PipeWire for sound;
- access for Sunshine to create a virtual keyboard, mouse and DualSense;
- Sunshine with NVENC, and one app per game in `games/`;
- NvFBC capture, for better performance: frames go from the GPU straight to the encoder, with no
  copy through the CPU. GeForce drivers keep NvFBC for professional cards, so `setup.sh` patches
  the driver with [nvidia-patch](https://github.com/keylase/nvidia-patch) (the original is backed
  up).

Then open `https://<host>:47990` and set the web login.

In Moonlight, choose 4K: the game then draws each eye wider for near-4K detail. 4K needs about
twice the bitrate of 1080p, so a cable helps. Also turn on YUV 4:4:4 if the client offers it;
without it some reds show green patches.

- `host/xorg.conf` expects the GPU at `PCI:1:0:0`. Check yours with
  `nvidia-smi --query-gpu=pci.bus_id --format=csv`.
- AMD and Intel GPUs need a different virtual display and the VAAPI encoder.
- Sunshine's own service waits for a desktop login, which this host never has, so `setup.sh`
  replaces it with `ps-river-sunshine`.

## 7. Pair a client

Install Moonlight on the client and add the host by its IP. Moonlight shows a PIN: enter it in
Sunshine's web page under *PIN*.

- An **Apple TV** works best. It passes the whole DualSense through: buttons, touchpad and
  motion. Pair the controller in *Settings → Remotes and Devices* by holding PS + Create until
  it flashes. A DualSense remembers one console at a time; plug it into the PS5 by USB to get
  it back.
- A retail **PS5 can't be the client**: it has no Moonlight.

Then pick the game in Moonlight and play. Quitting it in Moonlight stops the emulator.

## 8. Check latency

Stream the Desktop app with Moonlight's performance overlay on. When the stream ends, Moonlight
logs its video stats. For an end-to-end check, `tools/latency_clock.py` shows a millisecond clock
on the host: photograph it next to the stream. Measured with both ends on Wi-Fi, 1080p at 60 fps, 40 Mbps:

| Host encode | Network | Decode | Render | Frames dropped |
| --- | --- | --- | --- | --- |
| 2.2 ms | 5 ms | 2.0 ms | 0.5 ms | 0.3% |

That adds about 20–30 ms to the game in all: fine for a platformer. Wi-Fi shows up as a brief
freeze now and then, not as lag; a cable at either end helps most. Turn on the TV's game mode:
its picture processing can add more lag than the whole stream.
