"""Instrument MC3's MPEG callback selection in an isolated generated unit.

The original game selects one of 16 callbacks at 0x0061FA00. A loading
crash at 0x0053B0D8 used callback 2, which is data after that table. This
probe records the index when it is selected and the callback before use.
It does not change game behavior or the input generated tree.
"""

import argparse
from pathlib import Path


ASSIGN = "    set32(c.r[12], ld32((c.r[3].w[0])));\n"
CALL = "    set32(c.r[3], ld32((c.r[2].w[0])));\n    c.budget -= 14;\n"


def instrument(source: str) -> str:
    if source.count(ASSIGN) != 1 or source.count(CALL) != 1:
        raise ValueError("MC3 callback anchors changed; inspect unit_00E6.cpp")
    assignment_probe = (
        ASSIGN
        + "    if (mc3_transition_trace() && (c.r[7].w[0] >= 16u || c.r[12].w[0] < 0x001A0000u))\n"
        + "        std::fprintf(stderr, \"[mc3-transition] frame %llu callback index %u table %08X target %08X flags %08X/%08X/%08X sp %08X\\n\",\n"
        + "                     static_cast<unsigned long long>(ps2::frame_count()), c.r[7].w[0], c.r[3].w[0], c.r[12].w[0],\n"
        + "                     ld32(c.r[29].w[0] + 200u), c.r[19].w[0], c.r[25].w[0], c.r[29].w[0]);\n"
    )
    call_probe = (
        CALL.replace(
            "    c.budget -= 14;\n",
            "    if (mc3_transition_trace() && c.r[3].w[0] < 0x001A0000u)\n"
            "        std::fprintf(stderr, \"[mc3-transition] frame %llu invalid callback %08X slot %08X object %08X item %u\\n\",\n"
            "                     static_cast<unsigned long long>(ps2::frame_count()), c.r[3].w[0], c.r[2].w[0],\n"
            "                     c.r[20].w[0], c.r[21].w[0] - 1u);\n"
            "    c.budget -= 14;\n",
        )
    )
    source = source.replace(ASSIGN, assignment_probe, 1).replace(CALL, call_probe, 1)
    return (
        '#include <cstdio>\n#include <cstdlib>\n#include "ps2/system.hpp"\n'
        'static bool mc3_transition_trace() { static const bool enabled = std::getenv("PS2_MC3_TRANSITION_TRACE") != nullptr; return enabled; }\n'
        + source
    )


def build_probe_tree(generated: Path, output: Path) -> Path:
    generated = generated.resolve()
    output = output.resolve()
    source = generated / "unit_00E6.cpp"
    if not source.is_file() or not (generated / "units.hpp").is_file():
        raise ValueError("not a generated MC3 game tree")
    if output.exists():
        raise ValueError("probe output already exists")
    patched = instrument(source.read_text())
    output.mkdir(parents=True)
    for path in generated.iterdir():
        if not path.is_file():
            continue
        target = output / path.name
        if path == source:
            target.write_text(patched)
        else:
            target.symlink_to(path)
    return output / source.name


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", type=Path, help="generated MC3 directory")
    parser.add_argument("output", type=Path, help="new isolated generated directory")
    args = parser.parse_args()
    try:
        result = build_probe_tree(args.generated, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"error: {exc}\n")
    print(result)


if __name__ == "__main__":
    main()
