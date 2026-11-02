# VPS deployment: playable site and multiplayer relay

The public site at [mc3-enhanced.brdanpe.tech](https://mc3-enhanced.brdanpe.tech/) has a single **Play** entry point, a keyboard map, and brief instructions. The game shell contains one **Jogar** button and optional LAN room setup before boot; players select the mode inside the original game menus. The public release does not include the standalone room-test page. The browser executes the generated WebAssembly and renders locally. Nginx serves static files and disc sectors; a loopback WebSocket service relays room packets.

The project owner supplied authorization to redistribute the game artifacts on this VPS. That authorization has not been independently verified and does not extend to third parties. Keep the ISO, generated code, WebAssembly, saves, and release archives out of Git.

## Live installation (2 November 2026)

- Domain: `mc3-enhanced.brdanpe.tech`; VPS: `179.199.141.253`; SSH user: `engenhalab-admin`, port 22, key-based access with passwordless sudo. Root SSH remains disabled.
- Active release: `/var/www/mc3-enhanced/releases/20261102T134434Z`, selected by `/var/www/mc3-enhanced/current`. The previous playable release `20261101T234333Z` remains available for immediate rollback; older releases are retained.
- Disc: `/var/lib/mc3-enhanced/disc.iso`, 8,539,963,392 bytes, SHA-256 `de199f7e7f57b7cb603a129217f7482d50644fd66558450dfeb5612857b732f9`. Nginx maps only `/disc.iso` to this path and supports HTTP Range.
- Playable archive: `build/vps/mc3-enhanced-playable.tar.gz`, SHA-256 `0de553d605df297e756bd196a2dd2b00d9c6f03e91f8dbeb38bdbaef44cdf1ea`; verified on the VPS before extraction. It contains the landing page, browser build, virtual LAN client, preconfigured virtual PS2 card, relay, and pinned requirements. The disc is transferred separately.
- Nginx site: `/etc/nginx/sites-available/mc3-enhanced`, enabled by its matching symlink. TLS is managed by Certbot under `/etc/letsencrypt/live/mc3-enhanced.brdanpe.tech/`.
- Relay: `mc3-enhanced-relay.service` runs unprivileged on `127.0.0.1:8621`. Nginx proxies `/v1/rooms/` to it over loopback. Do not expose port 8621 publicly.

## What has been verified

- Archive and ISO SHA-256 matched between local and VPS copies; `nginx -t` passed; the relay service started.
- HTTPS returned 200 for the landing page, game shell, scripts, styles, card seed, and health endpoint. WebAssembly and game data use precompressed files. `/disc.iso` returned 206 for an HTTP Range request. `/room.html` returned 404.
- Two independent WSS clients joined the public relay and exchanged a binary packet.
- The public landing page opened the new game shell with one **Jogar** button. Solo started with no room connected. Two Edge tabs joined the same room using the generated invite, both displayed **2 online**, and both started the game with that room active. The invite carried the card seed and virtual LAN flags. An earlier Solo smoke test visibly rendered the opening logo; the landing page also rendered with its versioned CSS in a browser that had cached the preview.

These checks do not establish sustained campaign FPS, a complete public race between two separate machines, race results, long-session saves, or performance on low-power GPUs. The local two-instance Ordered Race evidence is documented in [MULTIPLAYER_PLAN.md](MULTIPLAYER_PLAN.md).

## Prepare a new release

Update `develop`, build the game in the separate Linux/WSL build checkout, and run the relay tests. The currently deployed variant includes the opt-in original NETWORKING menu and virtual LAN probes. A normal build without those patches will not provide the same in-game multiplayer path.

```bash
python3 -m unittest discover -s engine/tests -p test_room_relay.py -v
python3 deploy/package_playable.py build/vps/mc3-enhanced-playable.tar.gz \
  --build /absolute/path/to/build/web-mc3remix-menu-probe \
  --card /absolute/path/to/card-valid-v8.ps2
sha256sum build/vps/mc3-enhanced-playable.tar.gz
tar -tzf build/vps/mc3-enhanced-playable.tar.gz
```

The packager has an explicit member list and rejects missing files, symlinks, and unexpectedly large members. Verify that no ISO, key, environment file, or generated C++ enters the archive. Transfer the ISO separately only when its hash or content changes.

Stage the archive in a new `/var/www/mc3-enhanced/releases/<UTC timestamp>` directory, verify its SHA-256 on the VPS, extract it, and create the relay virtual environment from the pinned requirements. Keep the shared disc and any persistent data outside release directories. Test the candidate on loopback before changing `current`.

The Nginx template is [nginx-tls.conf](../deploy/hostinger/nginx-tls.conf). It provides HTTPS, cross-origin isolation, WebAssembly and audio-worklet CSP allowances, JavaScript MIME for `virtual-lan.mjs`, static gzip, Range access to the disc, and WSS proxying. Run `nginx -t` before every reload. Keep other sites' Nginx blocks and services untouched.

## Activate, verify, and roll back

Point `/var/www/mc3-enhanced/current` to the new release atomically, restart only `mc3-enhanced-relay.service`, and reload Nginx after a successful syntax check. Verify public HTTPS assets, Range responses, two WSS clients in one room, Solo boot, and Multiplayer room creation and boot in a real browser. For a full multiplayer release gate, test two separate client machines in the original LAN lobby and a complete race, including disconnect behavior, latency, and FPS.

To roll back, restore `current` to the previous release and restore its matching Nginx configuration, then run `nginx -t`, reload Nginx, restart the relay, and repeat public smoke checks. The previous preview configuration does not expose `/disc.iso`; the disc file can remain in `/var/lib/mc3-enhanced`. A code rollback does not alter browser-side virtual memory cards.

## Player flow

Open the homepage and click **Play**. Press **Jogar** for Solo, career, or Arcade. For LAN, click **Conectar sala para LAN**, enter a name, and create or join a room before pressing **Jogar**. In the original menus, use **NETWORKING → LAN Play** to reach the in-game lobby. No account is required. Progress lives in a virtual PS2 card stored by the browser; players can export a backup from the game shell. Clearing site storage may erase locally stored progress.
