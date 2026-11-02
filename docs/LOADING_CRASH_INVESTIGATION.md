# Arcade loading crash investigation

## Observed failure

The player reported crashes when entering or leaving an Arcade race without a LAN room. The supplied browser log stops at frame 19,226 with `missing code at 0x00000002 (ra 0x0053B0D8)`. The three immediately preceding IPU commands (`0x24190000`, `0x34000000`, `0x38000000`) report bad codes. This establishes ordering, not yet causality.

At `0x0053B0D8`, the translated game calls a function pointer from an object with a 320-byte item stride. The crash register values place the callback slot at `0x016E8090` (`r20=0x016E7980`, callback offset 1488, item offset 320). Its value is `2`, which cannot be a translated EE function address.

The original ELF contains a 16-entry callback table at `0x0061FA00`; entries 0–15 are valid addresses in `0x0053B248..0x0053BE20`. The 32-bit words after that table are small data values, several equal to `2`. The setup code at `0x0053AF0C` reads the table at `0x0061FA00 + 4 * index` and copies the result to the callback slot. The index is composed from three fields (`mode << 2 | flag << 1 | flag`). A mode above 3 would read past the table. An independent memory overwrite of the slot is also possible until the assignment is traced in the failing run.

The public build should remain unchanged while this is diagnosed. Masking the index or substituting a callback would hide the upstream state error and may corrupt race loading.

## Diagnostic build

`engine/scripts/mc3_transition_probe.py` creates an isolated generated-code tree and logs invalid table selections and invalid callbacks before they are called. It changes no emulated behavior. Build that tree with `PS2_GENERATED` pointing to it and set `PS2_MC3_TRANSITION_TRACE=1` for the run. The IPU bad-code message now includes bit offset, input length, next bits, control word, phase, and result. Capture the first `[mc3-transition]` and `[ipu] bad code` lines with their frame numbers.

The hosted landing page offers **Jogar com diagnóstico de travamentos**. This enables the callback probe and keeps a bounded log in the current browser tab's session storage. After a halt, the game's bottom bar offers **Baixar diagnóstico do travamento**; the player can also download it before a halt. The diagnostic file stays on the player's computer until they choose to share it. Normal **Jogar agora** runs without the trace. A browser process crash can still prevent the download; the session storage copy may survive a page reload in the same tab.

For a deterministic test, save a state just before an Arcade race starts or ends, then run the same state with and without the probe. If the invalid table index is logged before the callback becomes `2`, trace the source mode and the immediately preceding VDEC result. If the index is valid but the callback later changes to `2`, trace writes to that object slot. Compare the same transition with PCSX2's IPU behavior before changing decoder semantics.

## Current status

The failure location and out-of-range callback table read are identified. The specific upstream cause and a safe fix are not yet established. No crash fix has been deployed.
