"""Rename MC3's title-menu Networking string in a local STRTBL extract.

The English table must retain its original byte length for disc_overlay.py.
The one UTF-16 code unit gained by Multiplayer comes from an unused trailing
space in the original logout message. Both original strings are verified.
"""

import argparse
import struct
from pathlib import Path

from strtbl_replace import mc3_hash, u32


def rename_networking(source):
    data = bytearray(source)
    languages = u32(data, 0)
    if not 1 <= languages <= 32:
        raise ValueError("invalid language count")
    table = u32(data, 4)  # English
    labels_at = 4 + 4 * languages
    if u32(data, labels_at) != 512 or not labels_at < table < len(data):
        raise ValueError("unexpected MC3 STRTBL layout")
    count = u32(data, labels_at + 4)
    if not 0 < count <= 100000 or u32(data, table) != count:
        raise ValueError("invalid English table")

    cursor = table + 4
    replacements = []
    for _ in range(count):
        record_hash = u32(data, cursor)
        cursor += 6  # hash and font hint
        font_length = u32(data, cursor)
        cursor += 4 + font_length
        units_at = cursor
        text_units = u32(data, cursor)
        cursor += 4
        text_at = cursor
        end = cursor + text_units * 2
        if end + 10 > len(data):
            raise ValueError("truncated English record")
        value = bytes(data[text_at:end]).decode("utf-16le")
        if not value.endswith("\0"):
            raise ValueError("unterminated English text")
        if record_hash == mc3_hash("CM_Networking"):
            if value != "Networking\0":
                raise ValueError(f"unexpected Networking string: {value!r}")
            replacements.append((units_at, text_at, end, "Multiplayer\0"))
        elif value == "Logging out of account... \0":
            replacements.append((units_at, text_at, end, "Logging out of account...\0"))
        cursor = end + 10

    if len(replacements) != 2:
        raise ValueError("expected exactly one Networking and one logout donor")
    original_end = u32(data, 8) if languages > 1 else len(data)
    if cursor != original_end:
        raise ValueError("unexpected English table boundary")
    for units_at, start, end, new_value in sorted(replacements, reverse=True):
        encoded = new_value.encode("utf-16le")
        struct.pack_into("<I", data, units_at, len(encoded) // 2)
        data[start:end] = encoded
    if len(data) != len(source):
        raise ValueError("string table length changed")
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="original local mcstrings01.strtbl extract")
    parser.add_argument("output", help="new extract with Multiplayer title")
    args = parser.parse_args()
    source, output = Path(args.source), Path(args.output)
    if output.exists():
        parser.error("output already exists")
    try:
        changed = rename_networking(source.read_bytes())
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(changed)
    except (OSError, ValueError, UnicodeError, struct.error) as exc:
        parser.exit(1, f"error: {exc}\n")
    print(f"Multiplayer string extract: {output}")


if __name__ == "__main__":
    main()
