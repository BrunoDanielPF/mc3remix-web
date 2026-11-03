# VPS deployment: playable site and multiplayer relay

The public site at [mc3-web.brdanpe.tech](https://mc3-web.brdanpe.tech/) has one primary **Play** entry point, a keyboard map, brief instructions, and an optional **Jogar com diagnóstico de travamentos** link. The game shell contains one **Jogar** button, a graphics-level selector, and optional LAN room setup before boot; players select the mode inside the original game menus. The public release does not include the standalone room-test page. The browser executes the generated WebAssembly and renders locally. Nginx serves static files and disc sectors; a loopback WebSocket service relays room packets.

The project owner supplied authorization to redistribute the game artifacts on this VPS. That authorization has not been independently verified and does not extend to third parties. Keep the ISO, generated code, WebAssembly, saves, and release archives out of Git.

## Live installation (3 November 2026)

- Domain: `mc3-web.brdanpe.tech`, A record to `179.199.141.253`; SSH user: `engenhalab-admin`, port 22, key-based access with passwordless sudo. Root SSH remains disabled.
- Active release: `/var/www/mc3-enhanced/releases/20261103T164717Z`, selected by `/var/www/mc3-enhanced/current`. The previous release `20261103T162254Z` remains available for rollback. A consistent database backup taken before the additive schema migration is stored at `/var/backups/mc3-telemetry/telemetry-20261103T164717Z.sqlite3` with root-only access. Older releases remain available.
- Disc: `/var/lib/mc3-enhanced/disc.iso`, 8,539,963,392 bytes, SHA-256 `de199f7e7f57b7cb603a129217f7482d50644fd66558450dfeb5612857b732f9`. Nginx maps only `/disc.iso` to this path and supports HTTP Range.
- Playable archive: `build/vps/mc3-web-telemetry2-20261103.tar.gz`, SHA-256 `1e6e68fcfe2737b5585f66af0760d2ecb646f25dc8109ee6af53089b9718ee75`; verified on the VPS before extraction. It contains the landing page, browser build from the existing transition-probe generated game with the graphics selector and adaptive telemetry, virtual LAN client, preconfigured virtual PS2 card, relay, telemetry collector and report, and pinned requirements. The disc is transferred separately.
- Nginx site: `/etc/nginx/sites-available/mc3-web`, enabled by its matching symlink; the former site's enabled symlink is removed. TLS is managed by Certbot under `/etc/letsencrypt/live/mc3-web.brdanpe.tech/` and expires on 1 February 2027 unless renewed.
- Relay: `mc3-enhanced-relay.service` runs unprivileged on `127.0.0.1:8621`, accepts the new HTTPS origin, and is proxied under `/v1/rooms/`. Do not expose port 8621 publicly.
- Telemetry: `mc3-telemetry.service` runs on `127.0.0.1:8622`, accepts bounded anonymous technical events from the game shell, and stores them in `/var/lib/mc3-telemetry/telemetry.sqlite3` outside release directories. Nginx disables access logging only on `/v1/telemetry` and limits the request rate and body size. The data is private to the VPS and removed after 30 days.

## What has been verified

- The second telemetry release passed schema-migration tests against a legacy table and a copy of the live SQLite database, preserving all 19 pre-activation events. The updated collector and relay remained active. Public HTTPS returned 200 for the shell, 206 for WebAssembly and disc Range requests, and 204 for both legacy and new alert telemetry events. WSS returned a relay welcome message. Both synthetic events were removed after the report confirmed one alert and one periodic sample. Existing real player samples remained in the report; no real-player alert from the adaptive build has yet been observed.
- On 3 November, the first telemetry release passed collector unit tests, JavaScript syntax check, WSL WebAssembly link, archive and gzip checks, a candidate collector ingest/report smoke test, and `nginx -t`. After activation, public HTTPS returned 200 for the page, 206 for WebAssembly and disc Range requests, 204 for an authorized telemetry sample, and 403 for an unauthorized Origin. The private report showed the synthetic sample, which was then removed. The relay returned a WSS welcome message. Later real player sessions confirmed sustained sample collection. The client-side opt-out has not yet been independently checked in a browser.
- On 3 November, `nginx -t` passed after the hostname switch, the relay restarted and stayed active, the new HTTPS page and game shell returned 200, the WebAssembly and disc returned 206 for 32-byte Range requests, and two WSS clients joined one room and exchanged a binary packet. The public shell contained the graphics selector and texture-filter setting. The former hostname's enabled site was removed and its A record no longer resolved through 1.1.1.1 after the DNS change. FPS and long-session browser play were not remeasured in this deployment.

- Archive and ISO SHA-256 matched between local and VPS copies; `nginx -t` passed; the relay service started.
- HTTPS returned 200 for the landing page, game shell, scripts, styles, and card seed. WebAssembly and game data use precompressed files. `/disc.iso` returned 206 for an HTTP Range request. `/room.html` returned 404. There is no public HTTP health endpoint; the relay is checked through its service status and WebSocket connections.
- Two independent WSS clients joined the public relay and exchanged a binary packet.
- The public landing page opened the new game shell with one **Jogar** button. Solo started with no room connected. Two Edge tabs joined the same room using the generated invite, both displayed **2 online**, and both started the game with that room active. The invite carried the card seed and virtual LAN flags. An earlier Solo smoke test visibly rendered the opening logo; the landing page also rendered with its versioned CSS in a browser that had cached the preview.
- The diagnostic link was visible on the public page. In Edge, the public diagnostic build loaded the disc, selected the NVIDIA adapter, and advanced past frame 100 without console errors. A local controlled frame-limit halt changed the button to **Baixar diagnóstico do travamento**; its downloaded text file contained the frame number, halt reason, emulator messages, and GPU information. The Arcade race crash has not yet been reproduced under the probe.
- The November 2 diagnostic file showed a static picture with the emulated frame counter still advancing. The new build records emulation-thread and disc progress during diagnostic sessions. Its optional `PS2_IPU_ERROR_IRQ=1` experiment is off by default; native A/B testing through frame 2600 produced identical execution logs apart from the five extra interrupt-request messages. It is not a confirmed fix.
- The new public release returned HTTP 200 for its page, script, WebAssembly, and data; the disc returned HTTP 206 for a 32-byte Range request. New and rollback release virtual environments both import the pinned `websockets==15.0.1`; Nginx syntax passed and the relay stayed active through the static release switch.
- A fresh public WSS connection received a `welcome` message with protocol version 1 and slot 1 after activation. An Edge diagnostic run loaded the NVIDIA adapter, started successfully, and showed the Rockstar logo; the enhanced log recorded disc, EE, GS, and IPU snapshots while the first boot ran slowly. This smoke test does not reproduce the user's later static loading image.
- A second Edge run did reproduce the static opening logo at 60 displayed FPS. The EE PC, GS primitive count and disc fetch count stopped changing while IPU BDEC waited for input after channel 4 DMA ended. An isolated native frame-2200 state reproduced it. The cause of the insufficient input and the relationship to the Arcade callback crash remain open; see the investigation document. A follow-up diagnostic build adds IPU input bytes and DMA channel/status fields to the snapshot. These measurements do not change game execution.
- The follow-up diagnostic archive matched its SHA-256 on the VPS before activation. Its gzip assets passed integrity checks, the release virtual environment imported `websockets==15.0.1`, and `nginx -t` passed. The static symlink switched atomically without restarting the unchanged relay, which remained active. Public HTTPS returned 200 for the page, 206 for a WebAssembly Range request, and 206 for a disc Range request. A native load of the recorded state through frame 2300 still reproduced the same IPU wait and 50,176 GS primitives, confirming the instrumented code did not mask it.

These checks do not establish a fix for the Arcade loading crash, sustained campaign FPS, a complete public race between two separate machines, race results, long-session saves, or performance on low-power GPUs. The crash analysis and capture procedure are in [LOADING_CRASH_INVESTIGATION.md](LOADING_CRASH_INVESTIGATION.md). The local two-instance Ordered Race evidence is documented in [MULTIPLAYER_PLAN.md](MULTIPLAYER_PLAN.md).

## Prepare a new release

Update `develop`, build the game in the separate Linux/WSL build checkout, and run the relay tests. The currently deployed variant includes the opt-in original NETWORKING menu and virtual LAN probes, plus `mc3_transition_probe.py` applied to the generated tree. A normal build without those patches will not provide the same in-game multiplayer path or Arcade crash trace.

```bash
python3 -m unittest discover -s engine/tests -p test_room_relay.py -v
python3 deploy/package_playable.py build/vps/mc3-enhanced-playable.tar.gz \
  --build /absolute/path/to/build/web-mc3remix-transition-probe \
  --card /absolute/path/to/card-valid-v8.ps2
sha256sum build/vps/mc3-enhanced-playable.tar.gz
tar -tzf build/vps/mc3-enhanced-playable.tar.gz
```

The packager has an explicit member list and rejects missing files, symlinks, and unexpectedly large members. Verify that no ISO, key, environment file, or generated C++ enters the archive. Transfer the ISO separately only when its hash or content changes.

Stage the archive in a new `/var/www/mc3-enhanced/releases/<UTC timestamp>` directory, verify its SHA-256 on the VPS, extract it, and create the relay virtual environment from the pinned requirements. Keep the shared disc and any persistent data outside release directories. Test the candidate on loopback before changing `current`.

The Nginx template is [nginx-tls.conf](../deploy/hostinger/nginx-tls.conf). It provides HTTPS, cross-origin isolation, WebAssembly and audio-worklet CSP allowances, JavaScript MIME for `virtual-lan.mjs`, static gzip, Range access to the disc, and WSS proxying. Before activating it, create the target A record, issue a certificate for the new hostname, and verify the site on loopback with the new Host header. Run `nginx -t` before every reload. Keep other sites' Nginx blocks and services untouched.

## Player telemetry

The shell enables anonymous technical telemetry by default and exposes an on/off control before starting and in the game bar. The choice is saved in browser local storage. Each page load uses a new random session ID; no account, player name, room code, memory card, full user agent, IP address, URL, or raw diagnostic log is sent. Every 30 seconds during play the shell sends displayed and produced FPS, displayed frame interval P95, GS and GPU timing estimates, emulator/VU/GS idle percentages, disc wait/fetch/ready-miss/preload counters, main-thread long-task totals, audio queue, heap size, graphics level, and coarse browser/OS/GPU labels. A five-second probe sends a short alert sample only for low FPS, stutter, disc wait, or a delayed timer; alerts are rate limited to one per 20 seconds per session. Start, categorized error, halt, and end events are also sent. The `?diagnostic=1` log remains local unless the player downloads and shares it separately.

Install `mc3-telemetry.service` from [mc3-telemetry.service](../deploy/hostinger/mc3-telemetry.service) when activating the release, enable it, and add the `/v1/telemetry` location from the Nginx template. The service has a private `StateDirectory`, binds only to loopback, accepts only the public game's Origin, rejects extra fields and free-text error messages, and removes events older than 30 days. Check `systemctl is-active mc3-telemetry` and `curl -fsS http://127.0.0.1:8622/healthz` on the VPS. A collector outage does not stop gameplay; the browser silently drops those samples.

Consult the private database over SSH with:

```bash
sudo python3 /var/www/mc3-enhanced/current/telemetry/telemetry_report.py --hours 24
sudo python3 /var/www/mc3-enhanced/current/telemetry/telemetry_report.py --hours 168 --format json
sudo python3 /var/www/mc3-enhanced/current/telemetry/telemetry_report.py --hours 24 --format csv > mc3-telemetry.csv
```

The text/JSON report groups visible periodic samples by GPU and graphics level, shows median displayed FPS, and highlights sessions with windows below 35 FPS, displayed frame interval P95 at least 100 ms, or at least 500 ms of disc wait. Short alert windows are counted separately so they do not skew the GPU medians. Use `--low-fps 45` to change the report threshold without changing collection. CSV exports individual samples, including their trigger and window duration, for deeper analysis. Session IDs are abbreviated in the summary. Treat FPS and GPU timing as client-reported estimates and compare repeated visible samples before attributing a bottleneck. The 256 MiB database ceiling makes the collector return 503 rather than fill the VPS disk; investigate and compact the database if this happens. The collector migrates the existing SQLite table by adding columns, retaining prior events; an older release can ignore the new columns after rollback.

## Activate, verify, and roll back

Point `/var/www/mc3-enhanced/current` to the new release atomically, restart only `mc3-enhanced-relay.service`, and reload Nginx after a successful syntax check. Verify public HTTPS assets, Range responses, two WSS clients in one room, Solo boot, and Multiplayer room creation and boot in a real browser. For a full multiplayer release gate, test two separate client machines in the original LAN lobby and a complete race, including disconnect behavior, latency, and FPS.

To roll back, restore `current` to the previous release and restore its matching Nginx configuration, then run `nginx -t`, reload Nginx, restart the relay, and repeat public smoke checks. The previous preview configuration does not expose `/disc.iso`; the disc file can remain in `/var/lib/mc3-enhanced`. A code rollback does not alter browser-side virtual memory cards.

## Player flow

Open the homepage and click **Play**. Press **Jogar** for Solo, career, or Arcade. For LAN, click **Conectar sala para LAN**, enter a name, and create or join a room before pressing **Jogar**. In the original menus, use **NETWORKING → LAN Play** to reach the in-game lobby. No account is required. Progress lives in a virtual PS2 card stored by the browser; players can export a backup from the game shell. Clearing site storage may erase locally stored progress.

To capture an Arcade loading crash, click **Jogar com diagnóstico de travamentos** on the homepage, play normally, and use **Baixar diagnóstico do travamento** in the game bar if it stops. The log remains in the browser tab and the downloaded file until the player chooses to share it; the page does not upload it automatically.
