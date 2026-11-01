#!/usr/bin/env python3
"""Package the hosted game page and relay; transfer the ISO separately."""

import argparse
import gzip
import hashlib
import io
import pathlib
import tarfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
WEB_FILES = ("ps2web.html", "ps2web.js", "ps2web.wasm", "ps2web.data",
             "ps2web.js.gz", "ps2web.wasm.gz", "ps2web.data.gz")


def package(output: pathlib.Path, build: pathlib.Path, card: pathlib.Path) -> str:
    files = {
        "site/index.html": ROOT / "site/index.html",
        "site/style.css": ROOT / "site/style.css",
        "site/favicon.svg": ROOT / "site/favicon.svg",
        "site/room-client.js": ROOT / "engine/web/room-client.js",
        "site/virtual-lan.mjs": ROOT / "engine/web/virtual-lan.mjs",
        "site/card-valid-v8.ps2": card,
        "relay/room_relay.py": ROOT / "engine/scripts/room_relay.py",
        "relay/requirements-relay.txt": ROOT / "engine/scripts/requirements-relay.txt",
    }
    files.update({f"site/{name}": build / name for name in WEB_FILES})
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                for name, source in sorted(files.items()):
                    if not source.is_file() or source.is_symlink():
                        raise ValueError(f"missing or unsafe release file: {source}")
                    data = source.read_bytes()
                    if len(data) > 100_000_000:
                        raise ValueError(f"unexpectedly large release file: {source}")
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    info.mode = 0o644
                    info.uid = info.gid = 0
                    info.mtime = int(source.stat().st_mtime)
                    archive.addfile(info, io.BytesIO(data))
    return hashlib.sha256(output.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--card", type=pathlib.Path, required=True)
    args = parser.parse_args()
    print(f"sha256={package(args.output, args.build, args.card)}")
    print(f"archive={args.output.resolve()}")
    print("ISO is not part of this archive")


if __name__ == "__main__":
    main()
