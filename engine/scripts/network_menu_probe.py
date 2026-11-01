"""Create an isolated probe for the MC3 Remix title-menu Network entry.

Only for diagnostics with the user's own PS2 Remix build. The ordinary
generated tree and web build remain untouched. Set PS2_NETWORK_MENU_PROBE=1
when running a binary built from the output tree. This changes only the
visibility gate; LAN emulation and the configuration utility are separate.
"""

import argparse
from pathlib import Path


TARGET = "unit_006D.cpp"
OTHER_CAREER_TARGET = "unit_006A.cpp"
FIRST = "if (t) { goto L_00354180; }"
SECOND = "if (t) { goto L_003541EC; }"
PROBE = 'std::getenv("PS2_NETWORK_MENU_PROBE") != nullptr'


def build_probe_tree(generated, output):
    generated = Path(generated).resolve()
    output = Path(output).resolve()
    source = generated / TARGET
    if not source.is_file() or not (generated / "units.hpp").is_file():
        raise ValueError("not a generated MC3 game tree")
    if output.exists():
        raise ValueError("probe output already exists")
    text = source.read_text()
    anchor = text.index("L_00354150:")
    end = text.index("L_003541EC:", anchor)
    section = text[anchor:end]
    if section.count(FIRST) != 1 or section.count(SECOND) != 2:
        raise ValueError("network menu code shape changed; inspect the generated unit")
    # The first SECOND skips the block before L_00354180. The second one
    # checks the device/configuration byte after that label. Both need bypass.
    section = section.replace(FIRST, f"if (t || {PROBE}) {{ goto L_00354180; }}", 1)
    section = section.replace(SECOND, f"if (t && !({PROBE})) {{ goto L_003541EC; }}", 2)
    trace = ('L_00354150:\n'
             '    if (std::getenv("PS2_NETWORK_MENU_TRACE")) {\n'
             '        static bool once = false;\n'
             '        if (!once) { std::fprintf(stderr, "[menu-probe] network entry gate at frame %llu\\n",\n'
             '                                 static_cast<unsigned long long>(ps2::frame_count())); once = true; }\n'
             '    }')
    section = section.replace('L_00354150:', trace, 1)
    text = text[:anchor] + section + text[end:]
    career_label = 'L_00354078:'
    if text.count(career_label) != 1:
        raise ValueError("title menu trace anchor changed")
    career_trace = (career_label + '\n'
                    '    if (std::getenv("PS2_NETWORK_MENU_TRACE")) {\n'
                    '        static bool once = false;\n'
                    '        if (!once) { std::fprintf(stderr, "[menu-probe] Career candidate at frame %llu\\n",\n'
                    '                                 static_cast<unsigned long long>(ps2::frame_count())); once = true; }\n'
                    '    }')
    text = text.replace(career_label, career_trace, 1)
    text = '#include <cstdlib>\n#include <cstdio>\n#include "ps2/system.hpp"\n' + text
    other_career = (generated / OTHER_CAREER_TARGET).read_text()
    other_label = 'L_00349050:'
    if other_career.count(other_label) != 1:
        raise ValueError("second Career reference trace anchor changed")
    other_trace = (other_label + '\n'
                   '    if (std::getenv("PS2_NETWORK_MENU_TRACE")) {\n'
                   '        static bool once = false;\n'
                   '        if (!once) { std::fprintf(stderr, "[menu-probe] alternate Career reference at frame %llu\\n",\n'
                   '                                 static_cast<unsigned long long>(ps2::frame_count())); once = true; }\n'
                   '    }')
    other_career = other_career.replace(other_label, other_trace, 1)
    other_career = '#include <cstdlib>\n#include <cstdio>\n#include "ps2/system.hpp"\n' + other_career
    output.mkdir(parents=True)
    for path in generated.iterdir():
        if path.is_file():
            target = output / path.name
            if path.name == TARGET:
                target.write_text(text)
            elif path.name == OTHER_CAREER_TARGET:
                target.write_text(other_career)
            else:
                target.symlink_to(path)
    return output / TARGET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", help="unmodified generated/mc3remix folder")
    parser.add_argument("output", help="new isolated folder for the probe build")
    args = parser.parse_args()
    try:
        result = build_probe_tree(args.generated, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"error: {exc}\n")
    print(f"isolated menu probe created: {result}")


if __name__ == "__main__":
    main()
