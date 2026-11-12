#!/usr/bin/env python3
"""Create a separate ISO from a checked, byte-exact overlay manifest."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def materialize(source, manifest_path, destination):
    source = Path(source).resolve(strict=True)
    manifest_path = Path(manifest_path).resolve(strict=True)
    destination = Path(destination).absolute()
    if destination.exists() or destination.is_symlink() or destination.resolve() == source:
        raise ValueError("destination must be a new file distinct from the source ISO")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("format") != "iso-byte-overlay-v1":
        raise ValueError("unsupported overlay format")
    if manifest.get("disc_size") != source.stat().st_size:
        raise ValueError("source ISO size does not match the overlay")
    patches = manifest.get("patches")
    if not isinstance(patches, list) or not 0 < len(patches) <= 1024:
        raise ValueError("overlay must contain 1 to 1024 patches")

    verified = []
    with source.open("rb") as disc:
        for patch in patches:
            offset, length = patch["offset"], patch["length"]
            name = patch["data_file"]
            if (not isinstance(offset, int) or not isinstance(length, int) or
                    offset < 0 or length <= 0 or offset + length > manifest["disc_size"] or
                    not isinstance(name, str) or Path(name).name != name):
                raise ValueError("invalid overlay patch bounds or payload name")
            payload = (manifest_path.parent / name).read_bytes()
            if len(payload) != length:
                raise ValueError("overlay payload length mismatch")
            disc.seek(offset)
            if digest(disc.read(length)) != patch["original_sha256"]:
                raise ValueError("source ISO patch bytes do not match the overlay")
            verified.append((offset, offset + length, payload))
    verified.sort()
    if any(left[1] > right[0] for left, right in zip(verified, verified[1:])):
        raise ValueError("overlay patches overlap")

    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=destination.name + ".", suffix=".tmp",
                                            dir=destination.parent)
    os.close(descriptor)
    try:
        shutil.copyfile(source, temporary)
        with open(temporary, "r+b") as output:
            for offset, _, payload in verified:
                output.seek(offset)
                output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o644)
        os.link(temporary, destination)  # atomic publication without replacing another ISO
    finally:
        Path(temporary).unlink(missing_ok=True)
    return destination.stat().st_size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        size = materialize(args.source, args.manifest, args.destination)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"materialized {args.destination}: {size} bytes; source ISO unchanged")


if __name__ == "__main__":
    main()
