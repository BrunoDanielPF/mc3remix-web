"""Prepare a local, byte-exact Multiplayer title overlay for the player's ISO."""

import argparse
from pathlib import Path

from dave_index import Image, entries, extract_entry
from disc_overlay import DiscOverlay, build_dave_overlay
from multiplayer_strings import rename_networking


ASSET = "fonts/mcstrings01.strtbl"


def prepare(iso, output_dir):
    iso = Path(iso).resolve()
    output_dir = Path(output_dir).resolve()
    manifest = output_dir / "multiplayer-strings-overlay.json"
    if manifest.exists():
        DiscOverlay([manifest], iso)
        return manifest
    image = Image(iso)
    matches = [row for row in entries(image, "ASSETS.DAT") if row["name"].lower() == ASSET]
    if len(matches) != 1:
        raise ValueError(f"expected one {ASSET} in ASSETS.DAT, found {len(matches)}")
    output_dir.mkdir(parents=True, exist_ok=True)
    original = output_dir / "mcstrings01-original.strtbl"
    edited = output_dir / "mcstrings01-multiplayer.strtbl"
    extract_entry(image, "ASSETS.DAT", matches[0], original)
    edited.write_bytes(rename_networking(original.read_bytes()))
    build_dave_overlay(iso, "ASSETS.DAT", ASSET, edited, manifest)
    DiscOverlay([manifest], iso)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso")
    parser.add_argument("output_dir")
    args = parser.parse_args()
    try:
        manifest = prepare(args.iso, args.output_dir)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"error: {exc}\n")
    print(f"local title overlay: {manifest}")


if __name__ == "__main__":
    main()
