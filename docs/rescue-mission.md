# ASTRO BOT Rescue Mission: game internals

What flat play relies on in the game. Addresses are for build 1.04, with the main module at
`0x800000000` ("base +" counts from there). Build 1.00 differs: see `known_title_builds.h`.

## Data files

- `data/multi_platformer/multi_platformer.xml` holds the game's settings (the update's copy
  wins). Groups used: `<VrPlayer>` (seat, head, player model), `<GoalBalloon>`, `<Controller>`,
  `<Respawn>`, `<Life>`.
- `data/multi_platformer/levels/<room>/lvx/design.lvx` lists a room's objects: position in
  metres, model, behaviour (`<LoxFile>`), and links to other objects. The game reads every
  room's file at boot, so a file being opened doesn't mean its room is loaded.
- Some rooms load over another one: orb, pause_menu, intro_animation, calibration and others.
- Flat play's data edits are laid over the game (`flat/<TITLE>.mods`); the game's files on disk
  never change.

## Memory and physics

- Settings live in singletons: VrPlayer at base + `0x2ed5dc8`, AppConfig at base + `0x2ecfd58`.
  A setting is a 0xa0-byte slot with its value at +0x90. A vector is three slots, y at +0x1f0.
- Physics is Sony Physics Effects. The game object is at base + `0x2ecfe38`. Its +0x10 points
  to the physics world being stepped right now, which changes as the game steps each room in
  turn, so flat play remembers the world the hero was found in.
- World +0x8 is the inner world (+0x58 its scale, 3 units a metre; +0x20 the library). The
  library has the body count at +0x1a0 and the bodies at +0x1a8.
- A body is 0xa0 bytes: motion type at +0x1, kind at +0x8, collision mask at +0xc, position at
  +0x40, velocity at +0x60. Kinds seen: 0x10 level, 0x20 goal rings, 0x800 breakables, 0x1000
  moving platforms, 0x200 the player's head.
- Bodies 0–2 are the player's head, which flat play places where the camera is. Body 5 is
  Astro; body 7 is the player's controller.
- Writing a body's position doesn't move Astro: the game copies his position in every frame.
- The sphere cast is at `0xcada50`. The physics step's call at `0xcb09cf` is hooked so the
  camera's casts run right after a step.

## Rooms and the player's seat

- Astro starts at a level's `hero_locator`, the seat at `vr_player_locator`. Moving the seat's
  locator in the file has no effect.
- The seat follows Astro forward, letting him get 1.1 m ahead and never going back. It halts
  at each `vr_player_stopper_N` until Astro walks through that stopper's trigger box.
- Trigger boxes start events: attacks, coins, respawn points, moving cranes, gadget pickups.
  `tools/scenario.py list GAME LEVEL` prints a level's triggers, stoppers and locators.

## Display

AppConfig's `Display/MainDisplay` (+0x6e8) picks the output: Auto 0, Mono 2, HmdStereo 12.

Mono is the game's own TV picture, 1080p at 60 fps and about half the GPU power of stereo, but
in levels it uses its own camera, so flat play can't steer it. Flat play keeps stereo and skips
drawing the eye that isn't shown.

## The player's body

The white shape behind Astro is the player's model (`<NewModel>`): a base, a neck and a head,
plus a cape that has colliders. Astro and the camera bumped into them. Flat play shrinks the
model and the cape, makes the base follow the head, and draws the model 5,000 units below,
past the far plane. A smaller drop still showed through glass floors.

The player's controller (`<Controller>`) is a physics object Astro can bump into, so flat play
makes its collision tiny. The game hides it when Astro walks away and shows it again when he
comes back. Flat play keeps it at the seat, aimed where the camera looks.

## World map and pause

- The map is a `MupVrMenu` object, made by `0xaf5550`, whose call flat play hooks. Its state
  is the u32 at +0x3810: 2 the planets, 3 flying to a world, 4 a world's levels, 5 leaving them.
- A world is picked by looking at its small floating island (within 18°) and pressing ✕. The
  test harness holds the view to fixed headings for each world (`tools/game_session.py`).
- The pause menu is a room loaded over the level. While it is open, the paused room's u32 at
  +0x195c is not 0. Rooms are made by `0xa32a50`, whose call flat play hooks to find them.

## Gameplay

- **Gadgets** (30 levels): a chest gives the hook shot, water gun, shuriken, machine gun or
  torch. Astro hits the chest, then the player's controller goes into it. Flat play's data
  edits make that happen at once, from any distance.
- **Bosses:** each is beaten with its gadget: gorilla and bird with the hook, octopus with the
  water gun, shark and the final boss with the machine gun, spider with shuriken.
- **Drill bees** (from beanstalk, World 2-1): a bee charges the player's head, then hangs dizzy
  0.5 m in front of it for 3 s. A head moving at 0.3 m/s or more knocks it, or Astro's punch if
  he can reach.
- **Headbutt boards:** every one can also be broken by throwing something at it. In World 1-2,
  only two need the head.
- **Look-at triggers** (23) start events by gaze; **chameleons** (20 levels) are found by
  looking at them.
- **Dandelions** (3: beanstalk, cave_canyons_half2, the collection room) are the only things
  that need a blow. The microphone otherwise only makes bubbles underwater.
- **Level end:** Astro stomps the goal pump, and the rescued bots raise a balloon of gold rings
  that swings 2–4 m up. Astro sits in a catapult on the controller. A touch pulled back
  0.4–0.65 of the pad for at least 0.25 s fires him, but only when aimed within 50° of the
  rings. What ends the level after the shot hasn't been seen yet.

## From IGN's walkthrough

- Hook shot: swipe up on the touchpad at a ring to fire, down to pull. Jolting the controller up
  flings Astro off the rope; raising it too high lets the rope go.
- Water gun: press the touchpad like a button, aimed by the controller.
- Gorilla (World 1): jump its shockwaves, punch its teeth, then pull each tooth with the hook.
- In menus, the item the head points at is chosen with ✕.

## Testing tools

- `tools/probe_server.py` (root, on the host): reads and writes guest memory, narrows down
  values by how they change, sends keys, takes screenshots. `tools/probe_client.py` has helpers
  for it, such as finding Astro's body.
- `tools/scenario.py`: a level with Astro's start or objects moved. The seat doesn't move with
  him, so it isn't valid for camera tests. Useful spots: 1-1's goal (0, 0.3, -31.0), Vertigo-Go's
  area D roof (4.6, 1.6, -58.5).
- Ghidra scripts are in `tools/ghidra/`.
- A gameplay fix counts as verified only after an in-game run with screenshots.

## Moments that need the player's body, by level

From every level's design file: what needs the player's controller (gadget chests), head
(boards, bees, starfish, soccer balls), gaze (look-at triggers) or the torch's light (switches).
Levels with none are left out.

| level | chest | boards | starfish | bees | look-at | soccer | switches | gadgets, prompts |
|---|---|---|---|---|---|---|---|---|
| beanstalk | 1 |  |  | 3 |  |  |  | WaterGun |
| bird_boss_test_01 | 1 |  |  |  |  |  |  | HookRope |
| cave_canyons_half1 | 1 | 2 |  |  | 5 |  |  | HookRope |
| cave_canyons_half2 | 1 |  |  |  | 1 |  |  | HookRope |
| cave_graveyard | 1 | 4 |  |  | 1 |  | 15 | TorchLight |
| cave_trap_1 |  | 2 |  |  | 1 |  |  |  |
| cave_trap_2 |  | 2 |  |  |  |  |  | GadgetSlingShot |
| challenge_boss_bird | 1 |  |  |  |  |  |  | HookRope |
| challenge_boss_gorilla | 1 |  |  |  |  |  |  | HookRope |
| challenge_boss_octopus | 1 |  |  |  |  |  |  | WaterGun |
| challenge_boss_shark | 2 |  |  |  |  |  |  | Machinegun |
| challenge_boss_spider | 1 |  |  |  |  |  |  | Shuriken |
| challenge_credits | 1 |  |  |  |  |  |  | Machinegun |
| challenge_lava2 | 1 |  |  |  |  |  |  | WaterGun |
| challenge_light | 1 |  |  |  |  |  |  | TorchLight |
| challenge_machinegun | 1 |  |  | 18 |  |  |  | Machinegun |
| challenge_rope_pull_02 | 1 |  |  |  |  |  |  | HookRope |
| challenge_shuriken_shooting | 1 |  |  |  |  |  |  | Shuriken |
| challenge_tatami | 1 |  |  |  |  |  |  | Shuriken |
| challenge_water_shooting | 1 |  |  |  |  |  |  | WaterGun |
| city_funfair_remake | 1 |  |  |  |  | 2 |  | Machinegun |
| collection_room |  | 3 | 3 |  | 7 |  |  |  |
| final_boss | 1 |  |  |  |  |  |  | Machinegun |
| forest_castle | 1 | 2 |  |  | 7 |  |  | HookRope |
| heaven | 1 | 7 |  |  |  | 3 |  | WaterGun |
| mine_coaster_04 |  | 2 |  | 3 |  |  |  |  |
| ninja_stage | 1 |  |  |  |  |  |  | Shuriken |
| outro_animation |  |  |  |  | 1 |  |  |  |
| rope_boss_test_01 | 1 |  |  |  |  |  |  | HookRope, GadgetRopeFlick |
| sea_whale_split1 |  |  | 6 |  |  | 5 |  | SoccerHeading |
| sea_whale_split2 |  |  | 2 |  |  | 3 |  |  |
| shark_boss_01 | 2 |  |  |  |  |  |  | Machinegun |
| spider_boss_test_01 | 1 |  |  |  |  |  |  | Shuriken |
| volcano | 1 | 3 |  |  |  |  |  | WaterGun |
| water_boss_final | 1 |  |  |  |  |  |  | WaterGun |
| world0_0_new_intro_2 |  | 9 |  |  |  |  |  | GadgetSlingShot, VrPlayerHeadbutt |
| world3_1_Indy_cave_split1 | 1 | 4 |  |  |  |  |  | Shuriken |
| world3_1_Indy_cave_split2 | 1 |  |  |  |  |  |  | Shuriken |
