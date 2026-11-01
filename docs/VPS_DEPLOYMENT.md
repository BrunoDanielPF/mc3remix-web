# VPS deployment: public preview and multiplayer relay

The current public release contains a **project preview** in `site/` and the **room relay**. Each player builds and serves the game locally with their own disc; the VPS forwards WebSocket packets and never receives an ISO, save file, generated game code, or rendered frame. The existing game page and generated WebAssembly are **not a public site bundle**. Do not describe the preview as a public playable game release.

## Required deployment inputs

- DNS name `mc3-enhanced.brdanpe.tech` and the VPS IP or SSH alias, user, port and authorized key.
- A DNS record pointing that name at the verified VPS.
- A dedicated service user, release directory and TLS certificate for this project. Inspect existing Nginx and systemd state before choosing names or paths; do not reuse another site's configuration.
- Firewall access for HTTPS and an existing trusted SSH route. Keep the relay's TCP listener on `127.0.0.1`.

The supplied general VPS guide defines the operational sequence: inspect, stage a hashed artifact, verify the hash on the server, activate a versioned release, test Nginx and public HTTPS/WSS, then retain a rollback target. The exact server details are deliberately absent from that guide and must be supplied for this site.

Local preview validation on 1 November 2026: the homepage and room page rendered, two browser tabs joined the same relay room, both showed two pilots, and a binary test packet sent by the second tab arrived in the first. This validates the preview and local transport only; HTTPS, WSS through Nginx, and two separate computers remain untested.

## Local preparation

Run the relay tests and inspect the release content:

```bash
python3 -m pip install -r engine/scripts/requirements-relay.txt
python3 -m unittest discover -s engine/tests -p test_room_relay.py -v
git diff --check
```

Run `python3 deploy/package_preview.py build/vps/mc3-enhanced-preview.tar.gz` to produce an allowlisted archive of the static preview, browser room client, and relay server. Record the printed SHA-256 and inspect the archive members before upload. Reject any archive that contains `*.iso`, `*.ps2`, `*.wasm`, `*.data`, `games/`, `generated/`, `build/`, `.env`, or keys. The VPS should install the pinned dependency in a virtual environment inside the new release and run the service as a dedicated unprivileged user. Keep releases and service data separate.

## Service and proxy

Start the relay on loopback. The local web build defaults to `http://127.0.0.1:8620`, which is the browser's Origin even when two players are on different computers:

```bash
python3 relay/room_relay.py --host 127.0.0.1 --port 8621 --origin https://mc3-enhanced.brdanpe.tech --origin http://127.0.0.1:8620
```

If players deliberately use another local port or origin, add that exact origin with another `--origin` argument. Do not use a wildcard origin. A site hosted on HTTPS would need its own exact HTTPS origin. The relay has room and packet rate limits, but public abuse protection and operational monitoring still need review before broad exposure.

Configure a dedicated Nginx `server_name` for `mc3-enhanced.brdanpe.tech`. Serve `site/index.html`, `site/room.html`, `site/style.css`, `site/room.js`, and `site/room-client.js` from the active release. Terminate TLS there and proxy only `/v1/rooms/` to `http://127.0.0.1:8621`, preserving the WebSocket `Upgrade` and `Connection` headers and the request path. Run `nginx -t` before reload; never expose port 8621 publicly. Use the site's actual certificate and existing Nginx conventions rather than copying a certificate path from another project.

## Client connection and verification

Each player runs the local game build at `http://127.0.0.1:8620/` and sets `relay-url=wss://COMPLETE_DOMAIN` in the query string. The web shell includes this URL in room invitations. Plain `ws://` is accepted only for loopback relay tests. The game page still needs the experimental network probes described in [MULTIPLAYER_PLAN.md](MULTIPLAYER_PLAN.md); a relay connection alone does not prove an in-game race.

Before activation, verify the release hash on the VPS and test the new service on loopback. After activation, confirm the service is active, Nginx configuration is valid, both public pages and assets load over HTTPS with a trusted certificate, and two **separate client machines** can join the same room through WSS. Then test the original LAN lobby and a race while recording latency, disconnection behavior, and client FPS. Keep the previous release available; on failure, restore its symlink/service target and verify again. An HTTP 200 homepage is not a substitute for a WebSocket room test.

## Public playable site gate

The local shell currently reads `/disc.iso` by HTTP Range, and the generated WebAssembly includes translated proprietary game code. A public playable site requires an implemented and tested design for users to provide their own local disc and locally generated game build, plus the rights to distribute every hosted artifact. It also needs cross-origin isolation headers (`Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`), complete multiplayer and save validation, and acceptable FPS on target client hardware. Until these gates pass, publish only the game-independent preview and relay components.
