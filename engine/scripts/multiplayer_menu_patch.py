"""Build an isolated MC3 generated tree with its web multiplayer menu hooks.

The translated game and its disc are supplied locally. This script refuses an
unknown generated code shape and never edits the input tree or disc image.
"""

import argparse
import tempfile
from pathlib import Path

from network_menu_probe import build_probe_tree


def build_multiplayer_tree(generated, output):
    output = Path(output).resolve()
    build_probe_tree(generated, output)
    title = output / "unit_006D.cpp"
    text = title.read_text()
    probe = 'std::getenv("PS2_NETWORK_MENU_PROBE") != nullptr'
    if text.count(probe) != 3:
        raise ValueError("MC3 title menu patch shape changed")
    title.write_text(text.replace(probe, "true"))

    network = output / "unit_0074.cpp"
    text = network.read_text()
    browse_label = "L_00372530:"
    if text.count(browse_label) != 1:
        raise ValueError("MC3 Networking entry patch shape changed")
    browse_hook = (browse_label + "\n"
                   "    { static std::uint64_t last = 0; static bool opened = false;\n"
                   "      static bool lan_opened = false;\n"
                   "      const auto frame = ps2::frame_count();\n"
                   "      if (frame > last + 30) { opened = false; lan_opened = false; }\n"
                   "      last = frame;\n"
                   "      const auto state = ld32((c.r[4].w[0] + 288u));\n"
                   "      if (state == 0 && !opened && ps2::g_options.on_multiplayer_screen) {\n"
                   "          opened = true; ps2::g_options.on_multiplayer_screen(0);\n"
                   "      } else if (state == 1 && !lan_opened && ps2::g_options.on_multiplayer_screen) {\n"
                   "          lan_opened = true; ps2::g_options.on_multiplayer_screen(1);\n"
                   "      } } ")
    text = text.replace(browse_label, browse_hook, 1)
    text = '#include "ps2/system.hpp"\n' + text
    network.unlink()
    network.write_text(text)

    lobby = output / "unit_0076.cpp"
    text = lobby.read_text()
    lobby_label = "L_003795D8:"
    if text.count(lobby_label) != 1:
        raise ValueError("MC3 Lobby patch shape changed")
    lobby_hook = (lobby_label + "\n"
                  "    { static bool opened = false;\n"
                  "      if (c.r[5].w[0] == 0) opened = false;\n"
                  "      if (c.r[5].w[0] != 0 && !opened && ps2::g_options.on_multiplayer_screen) {\n"
                  "          opened = true; ps2::g_options.on_multiplayer_screen(4);\n"
                  "      } } ")
    text = text.replace(lobby_label, lobby_hook, 1)
    lobby.unlink()
    lobby.write_text('#include "ps2/system.hpp"\n' + text)
    return output


def refresh_multiplayer_tree(generated, output):
    """Replace the derived units when the menu patch changes."""
    output_path = Path(output)
    if output_path.is_symlink():
        raise ValueError("multiplayer output must not be a directory symlink")
    output = output_path.resolve()
    generated = Path(generated).resolve()
    if output == generated or not output.is_dir():
        raise ValueError("expected an existing, isolated multiplayer tree")
    with tempfile.TemporaryDirectory(prefix="multiplayer-patch-", dir=output.parent) as temporary:
        staged = build_multiplayer_tree(generated, Path(temporary) / "tree")
        for source in staged.iterdir():
            target = output / source.name
            source.replace(target)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", help="original generated/mc3remix tree")
    parser.add_argument("output", help="new isolated generated tree")
    parser.add_argument("--refresh", action="store_true", help="refresh an existing isolated tree")
    args = parser.parse_args()
    try:
        result = (refresh_multiplayer_tree if args.refresh else build_multiplayer_tree)(
            args.generated, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"error: {exc}\n")
    print(f"multiplayer generated tree: {result}")


if __name__ == "__main__":
    main()
