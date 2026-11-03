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

## November 2 loading soft lock report

The player confirmed that the image stopped changing while the frame counter kept advancing. The supplied `mc3-diagnostic-2026-11-02T19-03-41-470Z.txt` contains three page starts. Its final session ends at frame 2449 with status **Running** and about 60 emulated and presented frames per second. There is no `halt`, JavaScript exception, or `[mc3-transition]` event in the file. The first session did slow to roughly 3 fps from frame 99 through frame 356, so it may represent a separate loading/performance problem; that session also has no fatal event.

Five IPU bad-code events occur in the final session at frames 1604, 1778, 1968 (twice), and 2200. They precede the player's report of a static image, but the current log does not show whether the game was waiting on disc I/O, IPU completion, or another condition. In particular, continuing frame presentation does not prove the game simulation or loading thread made progress.

The diagnostic build now requests an emulation-thread snapshot every five seconds. Each snapshot records the EE PC/return address, cumulative GS primitives, IOP synchronization time, and IPU command/stall/busy state. The browser heartbeat records disc fetch count, fetched bytes, readiness misses, disc wait time, status text, and page visibility. It also records the page path and query when a diagnostic session begins. These are read-only measurements and run only in diagnostic mode.

There is one opt-in experiment: `PS2_IPU_ERROR_IRQ=1` sends the IPU completion interrupt after a bad MPEG code sets ECD. The existing branch sets ECD and clears BUSY without an interrupt, whereas PCSX2 signals IPU command completion through an interrupt ([IPU worker](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/IPU/IPU_MultiISA.cpp)). The flag is **off by default** until a like-for-like loading test shows it resolves the soft lock without introducing a regression. Compare baseline and flagged runs from the same state or navigation path, recording both diagnostics and whether the picture advances.

A native A/B run from boot to frame 2600 reproduced all five bad codes with the same command values and decoder positions. With the flag on, five completion IRQs were requested. After removing those five extra log lines, the two full native logs were byte-for-byte identical, including the frame limit, cycle count, IPU command totals, DMA totals, and GIF/VIF totals. Thus this flag did not change the startup outcome through frame 2600 and is not a demonstrated fix for the reported soft lock. Browser presentation still needs comparison with the new heartbeat and snapshots.

### Reproduced opening-video soft lock

A later Edge diagnostic run reproduced a static, zoomed MC3 logo while the counter continued at about 60 FPS. In `mc3-diagnostic-2026-11-02T19-16-44-287Z.txt`, frames 2306 through 2906 had the same EE PC (`0x0053D3E8`), 50,176 GS primitives, 84 disc fetches, and 22,020,096 fetched bytes. The IPU remained busy on BDEC `0x24020000`, at bit 33 of its final 16-byte input buffer. The guest code at that PC polls IPU_CTRL.BUSY, so the emulated frame clock can advance without game progress.

The matching native state at frame 2200 reproduces the wait. IPU input DMA channel 4 is in **normal mode**, stopped with QWC 0 and MADR `0x00A68050`; output channel 3 is active with 48 qwords pending. DMAC status `0x4020031A` has channel 4 completion set but not enabled in the mask, so no guest DMA interrupt is expected to refill it. A forced extra IPU decode attempt from that same state still stalls. The input available to BDEC is insufficient to decode the next macroblock.

Two isolated experiments were rejected as fixes. Forcing ECD and clearing BUSY after 120 starved frames moved the guest into a new FDEC `0x40000008` wait without adding any GS primitives. Reading 8 or 512 additional qwords past the programmed DMA boundary completed BDEC, but FDEC again exhausted the data while scanning; the GS primitive count stayed at 50,176. Neither experiment is in the browser build. Treating the frame counter or an unchanged logo as a GPU performance measurement would misdiagnose this wait.

The next implementation target is IPU bitstream/DMA boundary accuracy: determine why the game begins this BDEC with too little compressed data remaining, compare the VDEC/BDEC sequence and bit position against a trusted PS2 implementation, and test from the saved frame-2200 state and a fresh browser boot. The separate Arcade loading failure that called address `2` may share an upstream IPU error, but that relationship has **not** been established. Do not replace the callback, auto-complete the IPU command, or read past the guest's programmed DMA range as a release fix.

A one-shot retry of the frozen BDEC from the frame-2200 state consumed bits 33 through 121, then requested more input while decoding the second coded block (coefficient index 3). This rules out a missing retry after the last DMA transfer: the command genuinely needs more compressed data than the guest submitted. Setting VDEC's result to zero after an invalid VLC was also tested from boot through frame 2600. It changed the intervening decode path and produced additional invalid-code events, but ended at the **same** IPU command, bit position, DMA address, and GS primitive count. That behavior change is not in the release.

Another native A/B run enabled the normally suppressed completion interrupt when BDEC detected a video start code. It requested 4,978 additional IPU interrupts through frame 2600, yet the command totals, cycle count, 50,176 GS primitives, and final BDEC wait matched baseline. This is not a startup soft-lock fix. The site still runs the unchanged decoder and DMA behavior.

Diagnostic snapshots now include pending IPU control/result/input bytes and both IPU DMA channels (CHCR, MADR, QWC, TADR, end/paused state), DMAC status and enabled pending bits. These fields distinguish a CPU wait, data starvation, and an actual halted recompilation in the next player log.

The local-only native reproduction state is `build/native-transition-probe/ipu-stuck-2200.state` in the WSL build checkout at `/home/brdanpe/midnightclubweb-ps2-recomp`. It is 45 MB and intentionally excluded from Git along with the ISO and generated code. Load it with `ps2run games/mc3remix/disc/SLUS_213.55 --disc <local-iso> --load-state build/native-transition-probe/ipu-stuck-2200.state --frames 2300 --ntsc` to compare a candidate fix against the stable 50,176-primitive wait.

The local ISO's SHA-1 is `ac1ce1a1c07cf3252fecb0c8f9a5ed710722153d`, matching the catalogued USA disc image in [HFS DB](https://db.hfsplay.fr/games/95808). Its SHA-256 also matches the VPS copy (see `VPS_DEPLOYMENT.md`). This rules out a truncated local download or transfer mismatch for this reproduction; it does not identify which emulator behavior diverges from the console.
