> [!WARNING]
> **This is n64hub/THPS64Host-web, a fork, not THPS64Host.** Its `main` branch adds web support: the host built into a
> web page (WebGPU, through [N64Recomp-browser](https://github.com/n64hub/N64Recomp-browser)) by `host/tools/build_web.py`
> and `host/cmake/thps_host.cmake`'s Emscripten parts, every source change behind `__EMSCRIPTEN__`. Everything on that
> branch past upstream's commits was written by AI (Claude). It is not made, endorsed or supported by THPS64Recomp:
> nothing here is offered upstream, and please don't take issues with this fork to them. The page it builds is
> [THPS2-64Recomp-web](https://github.com/n64hub/THPS2-64Recomp-web)'s.

# THPS64Host

The shared host of the THPS64Recomp ports:
[THPS1 64: Recompiled](https://github.com/THPS64Recomp/THPS1-64Recomp),
[THPS2 64: Recompiled](https://github.com/THPS64Recomp/THPS2-64Recomp) and
[THPS3 64: Recompiled](https://github.com/THPS64Recomp/THPS3-64Recomp). It holds the engine that runs the N64 games' code
after [N64Recomp](https://github.com/N64Recomp/N64Recomp) has translated it, on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and [RT64](https://github.com/rt64/rt64).

This repository isn't a game by itself, and there's nothing to download here. To play, go to one of the game repositories.
This README is for people changing the host.

**This repository contains no game data and no game code.** Everything made from a game dump is generated on your machine,
under git-ignored `build/` folders.

## What's here

```
host/src/        the engine: start-up, hooks, input, audio, Controller Pak, widescreen, HUD placement, frame interpolation,
                 ROM checks, versions, the first-start build, the split game library, cheats
host/src/ui/     the in-game settings menu (RmlUi); its documents, styles and shaders are in host/ui/
host/cmake/      thps_host.cmake: builds <game>_recomp from a game's profile
host/game-lib/   the game library's own code (the split build)
host/patches/    our patches to the pinned N64ModernRuntime and RT64, and their register (README.md)
host/tools/      build_host.py, setup_runtime.py, gen.py, package_split.py, scan_bundle.py, test.sh, check.sh and the other
                 shared host tools
host/tests/      the test harness (run.py, harness.py and the shared sessions)
host/packaging/  macOS and Windows packaging files
firstrun/        the overlay module the first start uses (THPS2 and THPS3)
*.py, games.toml the per-game tools (unpacking, symbols, version tables) and the facts table of the three games; TOOLS.md
tools/check.sh   the checks of this repository and of the games cloned next to it
```

A game repository mounts this repository at `decomp/common`, so the same files are at `decomp/common/host/src/`,
`decomp/common/host/tools/` and so on there.

## How the games use it

Each game repository has this repository as a git submodule at `decomp/common`, pinned to a release tag. The game keeps only its
**profile**: `game/game.toml` (names, config folder, wording, flags), its hook config, the version tables, the recipes, the
symbol files and `tests/scenarios.py`. The game's `CMakeLists.txt` sets the game's family and includes
`host/cmake/thps_host.cmake`; its `tools/` and `tests/` hold short wrappers that run the shared tools here for that game.

So a change here reaches all three games, and a change that only one game needs usually belongs in that game's profile.

## Changing the host

1. Branch here and make the change.
2. Clone the three games next to this repository, then run `tools/check.sh` here. It builds each game against your branch,
   as CI does, and its last line names the games it checked:

   ```sh
   git clone --recursive https://github.com/THPS64Recomp/THPS1-64Recomp ../THPS1-64Recomp
   git clone --recursive https://github.com/THPS64Recomp/THPS2-64Recomp ../THPS2-64Recomp
   git clone --recursive https://github.com/THPS64Recomp/THPS3-64Recomp ../THPS3-64Recomp
   tools/check.sh
   ```

   The games share one runtime folder, `build/runtime` here (`THPS_RUNTIME_DIR` sets another).
   To try your branch inside one game's checkout instead, fetch it into the submodule. Don't commit that pointer:

   ```sh
   git -C decomp/common fetch ../../../THPS64Host <branch>   # the path counts from decomp/common; an absolute path works too
   git -C decomp/common checkout FETCH_HEAD
   ```
3. If you have your own dumps, also run the tests for the area you changed in each game the change can reach (if you're not
   sure, that's all three); see the games' `BUILDING.md`. If you don't, `tools/check.sh` with the games cloned beside this
   repository is enough: a maintainer runs the full suites before the merge.
4. Add a line under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md). If a game has to change something to keep working
   (a renamed `game.toml` key, a new hook, a new symbol), say so under a **Games must** heading.
5. Open a pull request. Say how the change was made and which tests you ran. CI builds the `main` branch of all three games
   against your branch, with no ROM, so a build break in any game shows up before anything is tagged.
6. After the merge, a maintainer tags a release, and each game moves its submodule to the new tag in a small pull request
   of its own.

## Versioning

- Releases are tagged `vX.Y.Z` (semantic versioning): a patch release for fixes, a minor release for features, and a major
  release for anything a game must adapt to.
- Games pin a **tag**, never a branch or an untagged commit. Their CI checks this.
- All three games move to a new host release together, even if only one needed the change, so they stay on the same host.

## Upstream pins and patches

N64ModernRuntime and RT64 are pinned to exact commits in `host/tools/setup_runtime.py`, and N64Recomp in `host/tools/build_host.py`.
`setup_runtime.py` fetches the pins, checks them, and applies our patches in a fixed order.

[host/patches/README.md](host/patches/README.md) is the register: every patch we carry, why, the test that guards it, and the host
code that depends on RT64 internals. Read it before moving a pin, and follow its checklist. Always recreate the runtime
folder from the pins; don't patch an existing tree by hand. We move a pin only when there's a reason: a fix, a feature or
a security issue.

N64Recomp, N64ModernRuntime and RT64 don't accept contributions written with LLMs. Don't send our patches or AI-written
changes to them. Upstream bugs go up as reports written by a maintainer, with a small repro.

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) says what belongs here and what belongs in a game, and what happens to your pull request.
The rules are the same as in the game repositories: AI-assisted and hand-written contributions are both welcome if you say
how the change was made and include or run the tests, and you're responsible for what you submit. Never commit ROMs or
anything made from them. Bugs that players see go to the game's repository first; we move them here if the cause is in
the host.

How the checks work, who runs which, and how releases are made: [MAINTAINING.md](MAINTAINING.md).

## AI use

As in the games, AI was used as a tool to assist development of the code, the docs and the testing, with human
direction, play-testing and debugging throughout. Each game's README says how its releases are tested.

## License and legal

GPL-3.0-only ([LICENSE](LICENSE)). The host links N64ModernRuntime (GPL-3.0). Parts of the RT64 setup, the settings menu's
renderer and styles, and the platform code are adapted from Zelda64Recomp (GPL-3.0); the games' `NOTICE.md` and
`docs/THIRD_PARTY.md` list what came from where.

This is an unofficial fan project. It isn't affiliated with or endorsed by Activision, Neversoft, Edge of Reality,
Tony Hawk or Nintendo, or by the N64Recomp, N64ModernRuntime and RT64 projects. *Tony Hawk's Pro Skater* is a trademark of
Activision; Nintendo 64 is a trademark of Nintendo.

## Recorded rides for tests

`--ride-record FILE` records the input read from port 1 for the next course. THPS2/3 start at the first gameplay input read
after the goals sheet; THPS1 starts when the course is loaded. `--ride-play FILE` uses the boundary declared in the file. The control channel also accepts `ride rec FILE`, `ride play FILE`, `ride stop` and `ride` (status).

Experimental configured timing is opt-in for the audited THPS2 US static and split builds:
`--ride-step 1/30 --ride-record FILE`, or `ride rec-step 1/30 FILE`. Arm before entering a new course while no selected
skater exists. A present skater, unaudited version/region, LiveRecomp or PAL override produces an explicit error.
A new v3 file declares the selected version/class and configured base step; playback uses its own declaration.
The experimental `timing=frame-time-v2` policy chooses both the float1/30 output and its two integer tick units.
Earlier experimental v3 files declaring `frame-time-v1` are refused; use their original experimental engine to reproduce them.
Legacy v1/v2 files and ordinary play retain their existing timing. `--ride-step` with legacy playback is refused.
The original game still applies its Slow-Nic modifiers. Stop reports `stop_pending=1` until the next writer, keeping
any already configured input in the recording. That next writer uses ordinary timing; the following input boundary
publishes the file and can hold for capture. Quitting before that drain does not publish it. Status reports writer/restoration/integer-output
counts and errors. This private experimental path still needs its fresh record/replay and lifecycle validation gates.

A retry starts a new course. Playback ends at EOF; `ride` reports the consumed and total read counts.

Ride files contain only controller inputs. Version 2 stores the game family, buttons, full-precision stick floats and pad
presence, plus the start boundary. The loader also accepts the prototype's three-column version 1 files. It rejects empty, malformed, truncated or
wrong-game version 2 files. Keep captures and recorded fixtures under ignored `build/` folders until they have been reviewed.

Set `RIDE_STEP=1/30` only when creating a new configured canonical recording; playback obtains timing from the file.
Configured canonical recording requires the default composited capture fence.

The harness steps `("ride_arm", FILE)` before entering a course and `("ride_end", SHOT_NAME)` after it play the complete
stream, check EOF and take an end screenshot. End a canonical ride with the game paused. The opt-in `ride-canonical`
session in each game records a moving route with tricks and grind attempts, or replays `RIDE_FILE` when `RIDE_MODE=play`.
Select it with `--session ride-canonical` or `--only ride_canonical`. It arms after course selection so an attract demo cannot
be recorded by mistake. Recording asserts a substantial input stream and observed air; THPS1 also requires an observed grind.
Set `RIDE_REFERENCE` to a recording's end PNG to require pixel-exact replay against it.

Canonical tests use a completed composited capture: `ride hold 1` arms a fence at the input read after the complete recording
or playback. Capture starts before EOF; the harness waits for the submitted graphics/present IDs and keeps their final completed image, then `ride hold 0`
releases it. The hold times out with an explicit status after 20 seconds and is released on quit. `RIDE_CAPTURE=native`
selects the older native screenshot path for diagnosis. Ordinary checkpoint screenshots keep their existing path.
`THPS_RIDE_TRACE=1` logs per-input physics/run/timescale transitions locally; those diagnostics never enter a ride file.

Virtual stick commands take integer SDL axes (`-32768..32767`). After `vpad attach`, press a button to claim port 1 before
riding: stick motion alone intentionally does not claim a controller. Check the `Virtual Controller takes port 1` log line.
