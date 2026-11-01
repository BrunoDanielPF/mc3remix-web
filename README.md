# Midnight Club 3: DUB Edition Remix — Web Port

An experimental browser port of the **PlayStation 2 NTSC-U** release of *Midnight Club 3: DUB Edition Remix*. The game runs locally through static recompilation of its PS2 executable to C++ and WebAssembly, with a PS2 runtime and WebGPU renderer. This project builds on [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp); [nfsu2-web](https://github.com/th0mk/nfsu2-web) is an architectural reference.

**Status:** A local build boots, reaches the original menus, and drives in San Diego Cruise. Two browser instances have started and driven in the same original LAN Ordered Race. This is a research prototype: multiplayer results, long sessions, sound and save persistence still need validation, and performance on integrated GPUs is below the playable target.

![Two-player Ordered Race in the browser](docs/evidence/web-ordered-race-guest-track.png)

## What works today

- **Game boot and driving:** the PS2 opening, title screen, memory card prompt, original menus, Arcade selection, and San Diego Cruise render in the browser.
- **Local execution:** the PS2 Emotion Engine executable is translated ahead of time; the browser runs the generated WebAssembly, emulates the remaining hardware, renders through WebGPU, and streams disc sectors on demand through HTTP Range requests.
- **Controls and saves:** keyboard, gamepad, and touch input; aspect ratio, resolution and fullscreen controls; a browser-backed virtual PS2 memory card with import and export controls. Long-session save and restore still need end-to-end validation.
- **Multiplayer prototype:** the web shell offers Solo and Multiplayer, a player name without an account, room creation and invitations. A WebSocket relay forwards binary packets, while an opt-in virtual LAN bridge connects them to the emulated PS2 network adapter. Two instances discovered each other through the game's original **NETWORKING → Host Game / Join Game** flow, entered the original lobby, and started **Ordered Race — Rites of Passage**. Both displayed positions **1/2** and **2/2**, moved independently, and rendered the remote car. Finishing the race and final results have not been verified.
- **Performance diagnostics:** reproducible save states, frame-window benchmarks, GPU/CPU counters, disc-read counters, visual captures, and opt-in renderer experiments. Selective disc preloading eliminated network reads in one Cruise test window but did not materially improve FPS; the graphics path remains the main optimization target.

The [porting plan](docs/PORTING_PLAN.md), [multiplayer log](docs/MULTIPLAYER_PLAN.md), [performance report](docs/PERFORMANCE.md), [performance checkpoint](docs/PERFORMANCE_CHECKPOINT.md), and [UI asset map](docs/UI_ASSETS.md) contain the test conditions and remaining work.

## Performance checkpoint

| Scene and environment | Observed display rate | Scope |
| --- | ---: | --- |
| San Diego Cruise, Edge on NVIDIA Ampere | **57.22 FPS** | 120-frame benchmark, 640 × 512, with `PS2_CHUNK_WRITES=1`; p95 frame interval was 49.9 ms. |
| Same Cruise state, Intel Gen12-LP | **about 3.1–3.4 FPS** | Browser measurements on this machine; below the 30 FPS playable floor. |
| Two-player Ordered Race, two Edge windows | **about 22–23 FPS** | Short diagnostic captures with both clients and race capture active; not a controlled release benchmark. |

The target is approximately **60 displayed FPS**, with **30 FPS** as the minimum playable rate. Each player's browser renders its own frames; a VPS can serve files and relay network packets, but its GPU cannot increase the client's FPS. These measurements do not establish campaign or broad hardware performance.

## Build and run locally

Use your own PS2 Remix disc image. The tested image has volume label `MC3REMIX`, boot executable `SLUS_213.55`, and size **8,539,963,392 bytes**. The ISO, translated game code, generated WebAssembly, saves, and build outputs are intentionally excluded from this repository.

Prerequisites: Linux or WSL, Git, CMake, Ninja, a C++20 compiler, Python 3, sufficient disk space, and a current Chrome or Edge with WebGPU and the required WebAssembly features. The first web build installs or uses the Emscripten SDK. See the [engine instructions](engine/README.md) for the full toolchain requirements.

```bash
git clone https://github.com/BrunoDanielPF/mc3remix-web.git
cd mc3remix-web
mkdir -p "$HOME/mc3remix-build"
cp -a engine/. "$HOME/mc3remix-build/"
cd "$HOME/mc3remix-build"
scripts/port.sh mc3remix "/absolute/path/to/your/MC3-Remix.iso" --frames 4200 --rounds 4
python3 scripts/serve.py build/web-mc3remix games/mc3remix/disc.iso
```

Open `http://127.0.0.1:8620/` and click **Start**. The test server binds to loopback because its `/disc.iso` route exposes the selected disc to anyone who can reach it. To rebuild after editing `engine/`, copy the source into the separate build directory again and run `scripts/build_web.sh mc3remix` there.

### Controls

| Action | Keyboard | PS2 input |
| --- | --- | --- |
| Accelerate / confirm | `W` / `Z` | Cross |
| Brake / reverse | `S` | Square |
| Steer | `A` / `D` | Left stick |
| Handbrake | `Space` or `Q` | R1 |
| Nitro | `E` or `2` | R2 |
| Change camera | `C` | Triangle |
| Headlights / accept race | `X` | Circle |
| Menu navigation | Arrow keys | D-pad |
| Pause / start | `Enter` | Start |
| Fullscreen | `F` | Browser control |

`Shift` maps to L1, `R` or `1` to L2, `V` to L3, and `N` or `Backspace` to Select. `I`/`J`/`K`/`L` also move the left stick. During the opening, press `Z` to advance; at the memory card prompt you can choose **Continue Without Saving**. The web shell also supports a standard gamepad and on-screen touch controls.

## Multiplayer development

The relay is separate from the game server and can be tested locally:

```bash
python3 -m pip install -r engine/scripts/requirements-relay.txt
python3 engine/scripts/room_relay.py
python3 -m unittest discover -s engine/tests -p test_room_relay.py -v
```

With the local game server running, open `http://127.0.0.1:8620/room-test.html` to test room transport. That page verifies packet relay, not a completed in-game race. The original LAN flow currently requires a diagnostic build and opt-in network probes; follow the exact conditions in the [multiplayer log](docs/MULTIPLAYER_PLAN.md). Multiplayer career events, the wider Arcade race catalogue, race completion, player identity inside the original menus, and cross-machine latency are still open work.

For a relay on another machine, open the **local** game page with `?relay-url=wss%3A%2F%2FYOUR_HOST&mode=multi`. The relay accepts WSS endpoints, or plain WS only on loopback. The relay must allow the browser origin `http://127.0.0.1:8620`; room invitations retain the selected relay URL. See the [VPS deployment runbook](docs/VPS_DEPLOYMENT.md) for the relay-only architecture and release gates.

## Hosting boundary

The current browser build expects the ISO at `/disc.iso` and contains WebAssembly translated from the proprietary game executable. **Do not publish the ISO, generated C++, or generated WebAssembly as part of this repository or a public VPS release.** A public playable release needs a client-side disc/build loading design and rights to distribute every hosted artifact. The public preview in `site/` and the relay contain no game assets; the relay forwards packets over HTTPS/WSS and does not render the game. The [VPS runbook](docs/VPS_DEPLOYMENT.md) and [porting plan](docs/PORTING_PLAN.md) record the release requirements.

## Repository layout

- `engine/` — PS2 recompilation pipeline, runtime, browser shell, relay, and tests.
- `site/` — public project preview and browser-based room transport test, with no game build.
- `deploy/` — allowlisted packaging for the preview and relay.
- `docs/` — porting decisions, test evidence, performance measurements, and multiplayer work.
- `main` — initial baseline; `develop` — ongoing integration branch.

The engine carries an [MIT license](engine/LICENSE) and its own third-party notices. The root project has no separate license yet. *Midnight Club*, *PlayStation*, and related names and assets belong to their respective owners. This is an independent research project and is not affiliated with or endorsed by Rockstar Games, Sony, or the original developers.
