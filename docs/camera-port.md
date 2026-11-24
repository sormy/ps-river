# Flat camera

Flat play's camera (`FlatCamera`) behaves like the in-level camera of Astro Bot (PS5, 2024),
adapted to Rescue Mission. Its tuning values are that game's camera settings. Rescue Mission's
own internals are in `rescue-mission.md`.

## Reference

- Camera tuning: `data/prein/save_data/platformer.xml` (`<Platformer><Camera>`); the hero's in
  `hero.xml`.
- Camera areas: `data/prein/levels/<level>/level.lvx` (XML).
- Where the camera's pieces sit in the executable (base 0), for reference:

| What | Address |
| --- | --- |
| `Camera` settings singleton, filled at `0x5006d00` (a setting's value at +0x28) | `0xf311a80` |
| `Default` / `Rotation` settings loaders | `0x5007380` / about `0x21d3500` |
| settings copied into the camera object (fields +0xae0 to +0xed0) | `0x51c1b60` |
| per-frame update (helpers around `0x66f08d0`) | `0x66f4690` |
| `Normal` camera state's update | `0x5b3a3c0` |
| per-ray wall rule | `0x5b44c40` |
| push-out / surface keep-out / water clamp | `0x5b43c80` / `0x5b3a5a0` / `0x5b3b380` |
| extra radius | `0x5b3d660` |

## How the 2024 camera behaves

Lengths are in the game's units: Astro runs 1.1 a second. A filter F is the share kept every
1/60 s, applied as F^(60·dt).

- **Turning:** the stick input is filtered (0.86). Yaw turns 90°/s at full stick, pitch 80°/s,
  slowing over the last 20° before its limits of -40° and 80°.
- **Auto-turn:** while the hero runs across the view, the yaw turns up to 60°/s towards behind
  them. Pushing sideways without moving, or running at the camera, turns nothing.
- **Pitch rest:** 0.5 s after the camera stick is let go, the pitch eases back to its rest, but
  only while the hero moves.
- **Framing:** looking further down moves the camera further out and widens the view. Radius,
  field of view and the orbit point's offset are mixed from near to far values by
  ((pitch + 40) / 120)^0.9.
- **Following:** the camera follows the hero a frame ahead. Its height follows where the hero
  lands, not their jumps: a cast down finds the ground.
- **Walls:** four thin casts from the orbit point out past the camera. The camera is pulled in
  at once to what they leave clear, and let out slowly, only while the player plays.

### What flat play does differently

Flat play does the above. The differences:

- Flat play decides "on the ground" from the cast down. The 2024 game reads the hero's own state.
- Its stick dead zone is round, not per axis; its follow filters are applied directly.
- Not done yet: the per-ray wall rule, push-out, surface keep-out, the water clamp, and fading
  what is in the way (vines have no collision, so the camera passes into them).
- Only in flat play: placing the game's world around the player's seat, the easing from the
  seat, menus at the seat, and the lean.

Flat play's settings are in units of the hero's size. In Rescue Mission Astro runs 3 world units
a second at 3 units a metre, so `SHADPS4_VR_FLAT_SCALE` is 0.9.

The casts use the game's own sphere cast. They run on the game's thread right after its physics
step, so the camera gets its answers one frame late.

## Camera areas in the 2024 levels

The 2024 levels change the camera's settings inside box-shaped areas: 1,602 of them in 98 of 169
levels. Each of those levels has one area covering all of it, usually with radius 0.4 to 2.0,
field of view 60° to 50°, and a rest pitch of 20°. Flat play uses these values as its defaults,
since Rescue Mission has no areas of its own.

The smaller areas follow no rule that could be found from what they contain: designers placed
them by hand. The ones tied to something a level holds:

- **Bosses:** the camera turns towards the boss and holds its yaw there, so Astro and the boss
  are in view together. It may also stand 1 unit further out.
- **Side-scrolling (2D):** 334 areas hold the view side-on to a stretch.
- **Water:** a fixed radius of 1 and a pitch of 10°. Not used in flat play yet.
- **Starts:** the defaults, with a pitch per level.

## Flat play's situations

**Boss arenas** (`SHADPS4_VR_FLAT_ARENA_ROOMS`). Rescue Mission's bosses face the player's seat.
So in a boss room the camera turns to the seat's direction over 1 s and holds it there. It stands
1 unit further out; the pitch stays free.

**Side-on stretches.** Rescue Mission's levels are built to be seen from the seat, so a stretch
run across the player's view is seen side-on along the line from the seat to the hero. After
Astro has run across that line for 0.5 s, the view eases onto it and stops turning after him.
Running along the line for 1 s ends it, and so does turning the camera yourself, until the next
stretch.

**Open spaces.** Rescue Mission has no areas, so flat play measures the space instead: eight rays
3 units out, from 1 unit above the hero. The more of them that run free, the further out the
camera may go, up to 1 unit more.

**The catapult at a level's end.** While Astro waits in the catapult with the ring balloon near,
the camera goes to the seat and faces the middle of the rings' swing. The controller aims where
the camera looks, so a shot only needs timing. The view holds until Astro has landed.

**Falls.** As the 2024 game does, the camera follows a falling hero across but holds its height
and looks down at them. After a fall of more than 8 units, the picture fades to black with the
game's "bot not yet rescued" icon over it (`SHADPS4_VR_FLAT_FADE_PICTURE_FROM`,
`tools/gnf_picture.py`). It fades back in once the hero is back, with the camera behind them
and facing the seat's direction.

**Gadgets.** Gadgets aim where the camera looks. With `SHADPS4_VR_FLAT_GYRO_AIM=1`, tilting the
controller turns the aim from there, and moving the camera with the stick re-centres it. Holding
L2 puts the camera behind the controller instead of Astro, for aiming.

## Moments made for a headset

| In VR | In flat play |
| --- | --- |
| Headbutting bees, wasps and some boards | the game's own scripting starts most; an automatic headbutt is still open |
| Leaning to dodge what is thrown at the head | D-pad left/right leans 0.3 m while held |
| Looking to aim or to pick a world on the map | the camera's direction is the look |
| Pointing the controller with a gadget | the camera's direction, plus gyro aim |
| Blowing into the microphone | clicking the touchpad blows |

R3 and an L3 seat view were tried and removed, because children pressed them by accident.

## Dead ends in Rescue Mission

Rescue Mission's engine once drew a separate picture for people watching the TV. These were
checked as a source for a flat camera, and none of them is one:

- **The separate TV picture:** writing 1 to `[base + 0x2ed54a8] + 0x434` makes the game send
  720p frames to the social screen. They are a spectator view while someone plays in the
  headset, not a camera to play with.
- **Mono display:** the game's own flat picture, but in levels it uses the game's camera, not
  flat play's. It is opt-in (`SHADPS4_VR_FLAT_MONO`; see `rescue-mission.md`).
- **Others:** `0x800c68c70` only switches the video output mode. `TvPlayer` settings belong to a
  debug panel. A debug camera exists (`DebugCamera.cpp`). Command-line switches: `-room`,
  `-sequence`, `-viewer`.
