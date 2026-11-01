#!/usr/bin/env python3
"""Package only the public preview and game-independent room relay."""

import argparse
import hashlib
import io
import pathlib
import tarfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
FILES = {
    "site/index.html": ROOT / "site/index.html",
    "site/room.html": ROOT / "site/room.html",
    "site/style.css": ROOT / "site/style.css",
    "site/room.js": ROOT / "site/room.js",
    "site/favicon.svg": ROOT / "site/favicon.svg",
    "site/room-client.js": ROOT / "engine/web/room-client.js",
    "relay/room_relay.py": ROOT / "engine/scripts/room_relay.py",
    "relay/requirements-relay.txt": ROOT / "engine/scripts/requirements-relay.txt",
}
MAX_FILE_BYTES = 1_000_000


def package(output: pathlib.Path) -> str:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz") as archive:
        for name, source in FILES.items():
            if not source.is_file() or source.is_symlink():
                raise ValueError(f"missing or unsafe release file: {source}")
            data = source.read_bytes()
            if len(data) > MAX_FILE_BYTES:
                raise ValueError(f"unexpectedly large release file: {source}")
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o644
            info.uid = info.gid = 0
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    return digest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    print(f"sha256={package(args.output)}")
    print(f"archive={args.output.resolve()}")
    print("members=" + ", ".join(FILES))


if __name__ == "__main__":
    main()
