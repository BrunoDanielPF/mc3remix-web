#!/bin/bash
# Builds the web version of a recompiled game into build/web-<game>.
# usage: scripts/build_web.sh <game folder under games/> [--multiplayer] [emsdk dir]
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
game=${1:-gt4}
multiplayer=0
if [ "${2:-}" = --multiplayer ]; then multiplayer=1; shift; fi
emsdk=${2:-${EMSDK:-$here/tools/emsdk}}
generated="$here/generated/$game"
out="$here/build/web-$game"
if [ "$multiplayer" = 1 ]; then
    [ "$game" = mc3remix ] || { echo '--multiplayer is only supported for mc3remix' >&2; exit 2; }
    generated="$here/generated/$game-multiplayer"
    out="$here/build/web-$game-multiplayer"
    if [ ! -d "$generated" ]; then
        python3 "$here/scripts/multiplayer_menu_patch.py" "$here/generated/$game" "$generated"
    elif [ "$here/scripts/multiplayer_menu_patch.py" -nt "$generated/unit_0074.cpp" ] ||
         [ "$here/scripts/network_menu_probe.py" -nt "$generated/unit_006D.cpp" ] ||
         [ "$here/generated/$game/unit_006A.cpp" -nt "$generated/unit_006A.cpp" ] ||
         [ "$here/generated/$game/unit_006D.cpp" -nt "$generated/unit_006D.cpp" ] ||
         [ "$here/generated/$game/unit_0074.cpp" -nt "$generated/unit_0074.cpp" ] ||
         [ "$here/generated/$game/unit_0076.cpp" -nt "$generated/unit_0076.cpp" ]; then
        python3 "$here/scripts/multiplayer_menu_patch.py" "$here/generated/$game" "$generated" --refresh
    fi
fi
boot=$(sed -n 's/^BOOT2 *= *//p' "$here/games/$game/disc/SYSTEM.CNF" | tr -d '\r')
elf=$(ls "$here/games/$game/patched/"* 2>/dev/null | head -1)
[ -n "$elf" ] || elf="$here/games/$game/core.elf"
if [ ! -f "$elf" ]; then
    name=${boot#*\\}
    elf="$here/games/$game/disc/${name%;*}"
fi
mkdir -p "$out"
if [ "$multiplayer" = 1 ]; then
    python3 "$here/scripts/prepare_multiplayer_disc.py" "$here/games/$game/disc.iso" "$out"
fi
printf '%s\n' "$boot" > "$out/boot.txt"
source "$emsdk/emsdk_env.sh" > /dev/null 2>&1
emcmake cmake -S "$here" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2_GENERATED="$generated" -DPS2_WEB_ELF="$elf" -DPS2_WEB_BOOT_FILE="$out/boot.txt" \
    -DPS2_WEB_PATCHES="$([ -f "$here/games/$game/patches.txt" ] && echo "$here/games/$game/patches.txt")"
ninja -C "$out" ps2web
gzip -kf9 "$out/ps2web.wasm" "$out/ps2web.data" "$out/ps2web.js"
if [ "$multiplayer" = 1 ]; then
    printf 'built %s/ps2web.html; serve it with scripts/serve.py %q %q --disc-overlay %q\n' \
        "$out" "build/web-$game-multiplayer" "games/$game/disc.iso" "build/web-$game-multiplayer/multiplayer-strings-overlay.json"
else
    printf 'built %s/ps2web.html; serve it with scripts/serve.py %q %q\n' "$out" "build/web-$game" "games/$game/disc.iso"
fi
