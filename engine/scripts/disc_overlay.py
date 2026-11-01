"""Optional, local-only byte overlays for a disc served by HTTP Range.

The source ISO is never modified. A manifest binds each replacement to the
original disc size and SHA-256 of the bytes it replaces.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import zlib

from dave_index import entries
from disc import Image, SECTOR


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class DiscOverlay:
    def __init__(self, manifest_paths, disc_path):
        if isinstance(manifest_paths, (str, Path)):
            manifest_paths = [manifest_paths]
        if not manifest_paths:
            raise ValueError("disc overlay needs at least one manifest")
        disc_path = Path(disc_path)
        size = disc_path.stat().st_size
        self.patches = []
        self.reported_offsets = set()
        with disc_path.open("rb") as disc:
            for path in manifest_paths:
                manifest_path = Path(path)
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                if manifest.get("format") != "iso-byte-overlay-v1":
                    raise ValueError("unsupported disc overlay manifest")
                if manifest.get("disc_size") != size:
                    raise ValueError("disc overlay was built for a different ISO size")
                rows = manifest.get("patches")
                if not isinstance(rows, list) or not 0 < len(rows) <= 1024:
                    raise ValueError("disc overlay must contain 1 to 1024 patches")
                for row in rows:
                    offset = row["offset"]
                    length = row["length"]
                    if not isinstance(offset, int) or not isinstance(length, int) or offset < 0 or length <= 0 or offset + length > size:
                        raise ValueError("disc overlay patch is outside the ISO")
                    if not isinstance(row["data_file"], str) or Path(row["data_file"]).name != row["data_file"]:
                        raise ValueError("disc overlay data_file must be a local filename")
                    payload_path = manifest_path.parent / row["data_file"]
                    payload = payload_path.read_bytes()
                    if len(payload) != length:
                        raise ValueError("disc overlay payload length mismatch")
                    disc.seek(offset)
                    if sha256(disc.read(length)) != row["original_sha256"]:
                        raise ValueError("disc overlay source bytes do not match the ISO")
                    self.patches.append((offset, offset + length, payload))
        self.patches.sort()
        if len(self.patches) > 1024:
            raise ValueError("disc overlay exceeds 1024 patches")
        if any(left[1] > right[0] for left, right in zip(self.patches, self.patches[1:])):
            raise ValueError("disc overlay patches overlap")

    def apply(self, offset, data):
        if not self.patches:
            return data
        end = offset + len(data)
        result = None
        for patch_start, patch_end, payload in self.patches:
            if patch_start >= end:
                break
            if patch_end <= offset:
                continue
            if os.environ.get("PS2_DISC_OVERLAY_TRACE") == "1" and patch_start not in self.reported_offsets:
                self.reported_offsets.add(patch_start)
                print(f"disc overlay read at ISO offset {patch_start}", flush=True)
            if result is None:
                result = bytearray(data)
            start = max(offset, patch_start)
            stop = min(end, patch_end)
            result[start - offset:stop - offset] = payload[start - patch_start:stop - patch_start]
        return bytes(result) if result is not None else data


def build_dave_overlay(iso_path, archive_name, asset_name, replacement_path, manifest_path):
    """Replace all copies of one asset, keeping each archived byte slot fixed."""
    iso_path = Path(iso_path)
    manifest_path = Path(manifest_path)
    payload_path = manifest_path.with_suffix(".bin")
    if manifest_path.exists() or payload_path.exists():
        raise ValueError("overlay output already exists")
    replacement = Path(replacement_path).read_bytes()
    image = Image(iso_path)
    archive = image.find(archive_name)
    if archive is None:
        raise ValueError(f"archive not found: {archive_name}")
    archive_base = archive[2] * SECTOR
    matches = [row for row in entries(image, archive_name) if row["name"].lower() == asset_name.lower()]
    if not matches:
        raise ValueError(f"asset not found: {asset_name}")
    if any(row["size"] != len(replacement) for row in matches):
        raise ValueError("replacement must have the same uncompressed size as every matched asset")
    if len({(row["stored_size"], row["compressed"]) for row in matches}) != 1:
        raise ValueError("matching asset copies use different storage layouts")
    first = matches[0]
    if first["compressed"]:
        compressor = zlib.compressobj(level=9, wbits=-15)
        payload = compressor.compress(replacement) + compressor.flush()
        if len(payload) > first["stored_size"]:
            raise ValueError(f"replacement compressed to {len(payload)} bytes; slot has {first['stored_size']}")
        payload += bytes(first["stored_size"] - len(payload))
        if zlib.decompress(payload, -15) != replacement:
            raise ValueError("replacement did not survive raw-deflate round trip")
    else:
        payload = replacement
    patches = []
    for row in matches:
        absolute = archive_base + row["offset"]
        image.file.seek(absolute)
        original = image.file.read(row["stored_size"])
        if len(original) != row["stored_size"]:
            raise ValueError("asset is truncated in ISO")
        patches.append({"offset": absolute, "length": len(payload),
                        "original_sha256": sha256(original), "data_file": payload_path.name})
    manifest = {"format": "iso-byte-overlay-v1", "disc_size": iso_path.stat().st_size,
                "archive": archive_name, "asset": asset_name, "patches": patches}
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    payload_path.write_bytes(payload)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return len(matches), len(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso")
    parser.add_argument("archive")
    parser.add_argument("asset", help="exact DAVE asset name, all duplicate entries")
    parser.add_argument("replacement", help="local replacement with the same uncompressed byte size")
    parser.add_argument("manifest", help="output JSON; companion .bin file is created beside it")
    args = parser.parse_args()
    try:
        count, size = build_dave_overlay(args.iso, args.archive, args.asset,
                                         args.replacement, args.manifest)
        DiscOverlay(args.manifest, args.iso)
    except (OSError, ValueError, KeyError, TypeError, zlib.error) as exc:
        parser.exit(1, f"error: {exc}\n")
    print(f"overlay ready: {count} entries, {size} bytes each; source ISO unchanged")


if __name__ == "__main__":
    main()
