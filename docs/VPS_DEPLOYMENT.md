# VPS deployment: public preview and multiplayer relay

The current public release contains a **project preview** in `site/` and the **room relay**. Each player builds and serves the game locally with their own disc; the VPS forwards WebSocket packets and never receives an ISO, save file, generated game code, or rendered frame. The existing game page and generated WebAssembly are **not a public site bundle**. Do not describe the preview as a public playable game release.

## Live installation (1 November 2026)

- Public preview: `https://mc3-enhanced.brdanpe.tech/`; room test: `/room.html`; relay: `wss://mc3-enhanced.brdanpe.tech/v1/rooms/<32-hex-code>`.
- VPS: `179.199.141.253` (`srv1953421.hstgr.cloud`), SSH user `engenhalab-admin` on port 22. `PermitRootLogin no` remains in effect; the admin user already existed and has sudo access. The public key fingerprint is `SHA256:WoPkbkAjEZBgmyrXnhgERSI6unFQHqS91sYWBm2yeIE`. Do not put passwords or private keys in this repository.
- Active release: `/var/www/mc3-enhanced/releases/20261101T231439Z`, selected by `/var/www/mc3-enhanced/current`. The relay runs as `mc3-enhanced-relay.service` with a dynamic unprivileged user on `127.0.0.1:8621`.
- Nginx site: `/etc/nginx/sites-available/mc3-enhanced`, enabled by the matching symlink in `sites-enabled`. The repository templates are in `deploy/hostinger/`. Certbot issued a certificate under `/etc/letsencrypt/live/mc3-enhanced.brdanpe.tech/`; its renewal task is enabled.
- The uploaded allowlisted archive had SHA-256 `a0caba24c5305ef9e805c516c8d4c2e545c12fdaacba39e1577bf93f10cf6a00`. The VPS needed `python3.14-venv`, `python3-pip-whl`, and `python3-setuptools-whl`; installing them upgraded no existing packages and restarted no services.

Release verification: five relay unit tests passed locally; the VPS archive hash matched; Nginx syntax passed; HTTPS returned 200 for the homepage, room page, CSS, JS, favicon, and `/healthz`; `/disc.iso` returned 404. Two WSS clients joined one public room and exchanged binary packets in both directions. An unrelated origin received HTTP 403. The room UI also created a room in a browser. This does **not** verify two separate player machines, an in-game race over the public relay, or client FPS. Keep those as release gates for the playable build.

For the local game shell, set `relay-url=wss%3A%2F%2Fmc3-enhanced.brdanpe.tech` and `mode=multi` in its query string. The game shell still requires the opt-in virtual LAN probes described in [MULTIPLAYER_PLAN.md](MULTIPLAYER_PLAN.md).

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
