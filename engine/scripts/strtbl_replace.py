"""Replace one same-length label value in a local MC3 Remix .strtbl extract.

The game file is supplied by the user and remains local. This tool targets
the MC3 table-v2/hash-v1 form; the DAVE overlay tool handles compression.
"""

import argparse
from pathlib import Path
import struct


def u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("truncated string table")
    return struct.unpack_from("<I", data, offset)[0]


def mc3_hash(label):
    value = 0
    for byte in label.encode("cp1252"):
        signed = byte if byte < 128 else byte - 256
        value = (value + signed) & 0xffffffff
        value = (value + (value << 10)) & 0xffffffff
        value ^= value >> 6
    value = (value + (value << 3)) & 0xffffffff
    value ^= value >> 11
    return (value + (value << 15)) & 0xffffffff


def replace_label(data, label, language, expected, replacement):
    data = bytearray(data)
    languages = u32(data, 0)
    if not 0 < languages <= 32 or not 0 <= language < languages:
        raise ValueError("invalid language index")
    pointers = [u32(data, 4 + 4 * i) for i in range(languages)]
    labels_at = 4 + 4 * languages
    if u32(data, labels_at) != 512:
        raise ValueError("this tool requires the MC3 table-v2 layout")
    count = u32(data, labels_at + 4)
    if count > 100000:
        raise ValueError("invalid label count")
    cursor = labels_at + 8
    labels = set()
    for _ in range(count):
        length = u32(data, cursor)
        cursor += 4
        if cursor + length + 1 > len(data) or data[cursor + length] != 0:
            raise ValueError("invalid label record")
        labels.add(data[cursor:cursor + length].decode("cp1252"))
        cursor += length + 1
    if label not in labels:
        raise ValueError(f"label not found: {label}")
    cursor = pointers[language]
    if cursor < labels_at or cursor >= len(data) or u32(data, cursor) != count:
        raise ValueError("invalid language table")
    cursor += 4
    target_hash = mc3_hash(label)
    for _ in range(count):
        record_hash = u32(data, cursor)
        cursor += 6  # hash and font-size hint
        font_length = u32(data, cursor)
        cursor += 4 + font_length
        text_units = u32(data, cursor)
        cursor += 4
        text_start = cursor
        text_stop = cursor + text_units * 2
        if text_stop + 10 > len(data):
            raise ValueError("invalid text record")
        cursor = text_stop + 10  # text, two scales and two byte hints
        if record_hash != target_hash:
            continue
        current = bytes(data[text_start:text_stop]).decode("utf-16le")
        if not current.endswith("\0") or current[:-1] != expected:
            raise ValueError(f"unexpected value for {label}: {current!r}")
        encoded = (replacement + "\0").encode("utf-16le")
        if len(encoded) != text_stop - text_start:
            raise ValueError("replacement must have the same UTF-16 byte size")
        data[text_start:text_stop] = encoded
        return bytes(data), text_start
    raise ValueError(f"hash record not found for {label}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("label")
    parser.add_argument("expected", help="current value, checked before changing")
    parser.add_argument("replacement")
    parser.add_argument("--language", type=int, default=0, help="language slot (0 = English)")
    args = parser.parse_args()
    try:
        output = Path(args.output)
        if output.exists():
            raise ValueError("output already exists")
        changed, offset = replace_label(Path(args.source).read_bytes(), args.label,
                                        args.language, args.expected, args.replacement)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(changed)
    except (OSError, ValueError, UnicodeError, struct.error) as exc:
        parser.exit(1, f"error: {exc}\n")
    print(f"replaced {args.label} at byte {offset}; source unchanged")


if __name__ == "__main__":
    main()
