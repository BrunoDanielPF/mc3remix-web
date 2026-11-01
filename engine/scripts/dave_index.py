"""List DAVE/Dave archive entries inside a PS2 ISO without unpacking the archive.

Usage: python3 scripts/dave_index.py games/mc3remix/disc.iso ASSETS.DAT --contains menu
The game data stays in the user's local ISO. No archive contents are written.
"""

import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

from disc import Image, SECTOR


NAME_ALPHABET = "\x00 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~\x7f"


def decode_name(data, offset, previous):
    codes = []
    cursor = offset
    name = ""
    while True:
        if not codes:
            if cursor + 3 > len(data):
                raise ValueError("compressed name extends beyond the name table")
            packed = int.from_bytes(data[cursor:cursor + 3], "little")
            codes = [(packed >> shift) & 0x3F for shift in (0, 6, 12, 18)]
            cursor += 3
        code = codes.pop(0)
        if not name and code >= 0x38:
            if not codes:
                continue
            prefix = (codes.pop(0) - 0x20) * 8 + code - 0x38
            if prefix < 0 or prefix > len(previous):
                raise ValueError("invalid compressed name prefix")
            name = previous[:prefix]
            continue
        if code == 0:
            return name
        if code >= len(NAME_ALPHABET):
            raise ValueError("invalid compressed name character")
        name += NAME_ALPHABET[code]


def entries(image, archive_path):
    found = image.find(archive_path)
    if found is None:
        raise ValueError(f"archive not found in ISO: {archive_path}")
    _, _, sector, archive_size = found
    archive_start = sector * SECTOR
    image.file.seek(archive_start)
    header = image.file.read(16)
    if len(header) != 16 or header[:4] not in (b"DAVE", b"Dave"):
        raise ValueError(f"not a DAVE/Dave archive: {archive_path}")
    count, table_size, name_size = struct.unpack_from("<III", header, 4)
    if count > 1_000_000 or count * 16 > table_size or 0x800 + table_size + name_size > archive_size:
        raise ValueError("invalid archive index bounds")
    image.file.seek(archive_start + 0x800)
    table = image.file.read(count * 16)
    image.file.seek(archive_start + 0x800 + table_size)
    names = image.file.read(name_size)
    if len(table) != count * 16 or len(names) != name_size:
        raise ValueError("truncated archive index")

    previous = ""
    for index in range(count):
        name_offset, offset, full_size, stored_size = struct.unpack_from("<IIII", table, index * 16)
        if name_offset >= name_size or offset + stored_size > archive_size:
            raise ValueError(f"entry {index} points outside the archive")
        if header[:4] == b"DAVE":
            end = names.find(b"\x00", name_offset)
            if end < 0:
                raise ValueError(f"entry {index} has no name terminator")
            name = names[name_offset:end].decode("ascii")
        else:
            name = decode_name(names, name_offset, previous)
        previous = name
        yield {"index": index, "name": name, "offset": offset,
               "size": full_size, "stored_size": stored_size,
               "compressed": full_size != stored_size}


def extract_entry(image, archive_path, row, output):
    found = image.find(archive_path)
    base = found[2] * SECTOR
    image.file.seek(base + row["offset"])
    data = image.file.read(row["stored_size"])
    if len(data) != row["stored_size"]:
        raise ValueError("truncated archive entry")
    if row["compressed"]:
        data = zlib.decompress(data, -15)
    if len(data) != row["size"]:
        raise ValueError("archive entry size mismatch")
    output = Path(output)
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", help="path to the user's local ISO")
    parser.add_argument("archive", help="archive path inside the ISO, e.g. ASSETS.DAT")
    parser.add_argument("--contains", default="", help="case-insensitive name filter")
    parser.add_argument("--limit", type=int, default=100, help="maximum rows to print")
    parser.add_argument("--json", action="store_true", help="emit one JSON object per row")
    parser.add_argument("--extract-index", type=int, help="extract exactly one listed entry")
    parser.add_argument("--output", help="destination file for --extract-index")
    args = parser.parse_args()
    if args.limit < 0:
        parser.error("--limit must be nonnegative")
    if (args.extract_index is None) != (args.output is None):
        parser.error("--extract-index and --output must be used together")
    try:
        image = Image(args.iso)
        total = matched = 0
        for row in entries(image, args.archive):
            total += 1
            if args.extract_index is not None:
                if row["index"] == args.extract_index:
                    extract_entry(image, args.archive, row, args.output)
                    print(f'extracted {row["name"]} to {args.output}')
                    return
                continue
            if args.contains.lower() not in row["name"].lower():
                continue
            matched += 1
            if matched > args.limit:
                continue
            if args.json:
                print(json.dumps(row, ensure_ascii=False))
            else:
                print(f'{row["index"]:5} {row["offset"]:10} {row["size"]:9} '
                      f'{"z" if row["compressed"] else "-"} {row["name"]}')
        if args.extract_index is not None:
            raise ValueError(f"entry index {args.extract_index} does not exist")
        print(f"{matched} matching entries of {total}; printed {min(matched, args.limit)}", file=sys.stderr)
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(1, f"error: {exc}\n")


if __name__ == "__main__":
    main()
