# Passdown notes

Working notes for whoever picks this up. They are not a user guide: the project isn't ready for players, and a build from a fresh clone hasn't been tried.

## What this is

A static recompilation of the PS4 game *A Certain Magical Virtual-On* (とある魔術の電脳戦機, `JP0177-CUSA08425_00-VOX2017INDEX0000`, v01.01, SDK 4.50) to native Windows x64. The game's x86-64 code is lifted to C, compiled with clang into `librecomp.dll`, and run by `vox.exe` with reimplemented PS4 system libraries and shadPS4's Vulkan renderer.

**No game data goes in this repo, ever.** You need your own dump of the game. `.gitignore` is a whitelist for that reason. Don't add anything from `extracted`, `port\work`, `analysis\export`, Ghidra projects or the generated C, all of which derive from the game.

## Current state (2026-11-06)

Test machine: Windows 10, RTX 2080.

| Area | State |
|---|---|
| Boot to title screen and menus | Works |
| Attract-mode battle rendering | Works, about 60 flips per second |
| Battles and most UI | Work (user's report), including with the controller |
| Xbox controller (XInput) | Works. No rumble (`scePadSetVibration` is a no-op) |
| Sound (SDL3) | Works by ear (the user's report); no movie sound |
| Movies | Looping backgrounds play through Media Foundation, video only (`hle_movie.c`); the menu panel videos are confirmed working. One-shot movies are skipped. If decoding fails: one black frame |
| English | Started: 218 of 10,874 Japanese lines. Displays, but with letters missing (see below) |
| PSN | Unimplemented `sceNp*` calls answer `ORBIS_NP_ERROR_SIGNED_OUT`; `sceRudp*` calls answer `0x80770001` (not initialized, from memory and unverified) |
| Saves, trophies | Saves work; trophies are logged only |

## Open problems, most urgent first

1. **Missing letters in English text (user report).** Never seen by a developer: capture a screen with `capture_window.ps1` first. Ruled out: the fonts. `unicode_font_jp_8bpp.ufn` has no ASCII, only fullwidth, but `unicode_font_ascii_8bpp.ufn` has every glyph (all rendered and checked), and `FUN_01156010` registers both, each with a `0x100` argument. Unconfirmed ideas: a glyph cache limit, the renderer missing updates to a glyph atlas, or code measuring ASCII text as Japanese.
2. **Intermittent freeze.** Seen in attract battles (about 50–65 s in) and mid-match. All threads go idle with 0 CPU. Evidence from every stall:
   - The GPU thread is in the WAIT_REG_MEM label wait in `ProcessGraphics` (`liverpool.cpp`), waiting for `buffer_labels[1] == 0`.
   - Labels are `0 1 0 0...`, `prev_index` is 1, flips pending 0, gc queue 0, 3 gfx submits queued, 0 commands.
   - A label only clears in `VideoOutDriver::Flip` when a later flip replaces that buffer, and CPU flips must pass through the sleeping GPU thread.
   - **Workaround, not a fix, and unproven:** after 200 ms in that state the wait releases the label and logs `CP VO label wait released`. It hasn't fired since it was added: three 110 s no-input tests and one user session of over 8 minutes had no freeze.
   - Root cause unknown: a lost flip, or the game picking the on-screen buffer. With `VOX_TRACE=1`, compare the last `TRACE flip` and `TRACE present` lines with the "CP VO label wait" line.
   - Ruled out: a label written directly by the game (the wait re-checks on a timer).
3. **Crash on pressing start in attract mode (user report, once).** No log survived, and it may have been the freeze. Note the log gets overwritten by every run.
4. **Starting a match "crashes" (early user report).** An unimplemented `sceRudpEnd` stopped the game during online shutdown after a signed-out PSN error. `sceNp*` and `sceRudp*` calls now return errors. Battles have worked since, but the user's menu path to that stop wasn't confirmed.
5. **Story mode hang (user report).** The story UI appeared with empty text boxes over black, flips still at 60. The game's background movie state machine (`FUN_010fda80`, state 2) waits for a "frame ready" flag at +10 that only `sceAvPlayerGetVideoDataEx` sets. Real movie frames should fix this, but **story mode hasn't been retested**.
6. **158 known imports are unimplemented.** Any other than `sceNp*` and `sceRudp*` stops the game by default and prints `STOP: unimplemented ...`. `VOX_LENIENT=1` logs and returns 0 instead.
7. **Remaining Phase 6 work:** movie sound and one-shot movies, rumble, and dialogs without UI.

Smaller items:
- `libc` and `libSceFios2` still run in the interpreter.
- Unmaps now update the renderer's mapping table, but the renderer's own caches aren't told about them, so stale entries are possible.
- `port\gpu\fetch_externals.ps1` doesn't list abseil-cpp, miniz or SPIRV-Headers yet.
- `liverpool.cpp` still holds diagnostics (`vox_recent`, the CP wait prints). They're useful for now.
- A user report of "7 failed background operations" during Phase 5 was never explained.

## Translation

Everything runs on the user's own copy of the game: extract with `analysis\scripts\cpk.py`, dump with `mtp.py dump`, and keep Japanese dumps out of the repo. `translation\*.en` holds English only, as `id<TAB>text` for the matching `.mtp`.

- **Build it:** from `analysis\scripts`, run `tl_pack.py <repo>\translation <extracted>\uroot\cpk <repo>\port\work\translation`. The runtime overlays `translation\<disc>.overlay` from its working directory onto the archive as it's read, and never changes the CPKs. The game needs a restart to pick up new overlays.
- **The limit:** a translated file must fit the original's byte size, and `tl_pack.py` refuses any that don't. Larger files would mean patching the archive's TOC and ITOC.
- **Where the text is:** `root+/mtpa/*.mtp`, 496 files, Shift-JIS, about 200,000 Japanese characters. Story scenes hold about 82,000 of those and tutorials 43,000. `mtp.py check` rebuilds all 494 parseable files byte for byte; `_MTPA_story_kamijo_01.mtp` and `message00.mtp` don't parse, reason unknown.
- **Art:** the main menu buttons are already English in the textures. Other words are baked into `.htx` textures (DDS behind a small header; view with `htx2png.py`), such as the `決定` and `戻る` prompts and `リソース`. Those need redrawing, and no tool writes `.htx` yet.
- **Dialogue care:** lines contain `<color>`, `<ruby>` and `<shake>` tags, and line ids seem to encode the speaker in the high bits. Keep the ids and tags. Use the spellings in `MTPA_vr_name_01.mtp` for mech names.
- **Quality:** the English so far is machine translation by Claude, unreviewed. "Ririn Furashina", "Berserk" for 暴走 and the stage names are guesses.
- **Order:** fix the missing letters before translating much more.

## Layout

| Path | What it is |
|---|---|
| `port\runtime` | `vox.exe`: loader, HLE of PS4 libraries, profiler |
| `port\gpu\shadps4` | shadPS4 v0.21 renderer, GNM and VideoOut, vendored. Local edits are marked `// vox:` |
| `port\gpu\shim` | Windows replacements for shadPS4's core, including the guest mapping table in `core\memory_win.cpp` |
| `port\patches` | Our changes to bbcpu and bloodborne-recomp, applied by `port\fetch_forks.ps1` at pinned commits |
| `analysis\scripts` | Ghidra scripts and Python tools for the code map |

Not in the repo, on the development machine:
- `extracted\`: the game's files.
- `analysis\vox*.gpr`, `analysis\export\`: Ghidra projects and the exported code map. Use `voxfix`.
- `port\work\`: game-derived build output, generated C, `librecomp.dll`, logs and saves.
- `port\deps`: Zydis and Zycore headers and libs (source in `tools\src\zydis`).
- `tools\`: PkgTool, Ghidra 12.1.4 + GhidraOrbis, JDK 21, LLVM, CMake, and pristine reference clones in `tools\src` (shadPS4 v0.21, LibOrbisPkg, bbcpu, bloodborne-recomp, bloodborne-runtime, zydis).

## Build and run

```powershell
port\fetch_forks.ps1                # clone bbcpu and bloodborne-recomp, apply port\patches
port\build_windows.ps1              # libbbcpu and tools (bbrecomp)
port\gpu\fetch_externals.ps1        # renderer dependencies
port\gpu\build.ps1 -Target voxgpu   # renderer
port\run.ps1 -Timeout 3600          # builds vox.exe and runs; log in port\work\run.log
```

To regenerate the C, in `port\work`, with `BB_JIT=0` and `BB_RECOMP_TABLES=recomp/known_tables.tsv`:

```powershell
..\out\bbrecomp.exe eboot.elf recomp/functions.tsv recomp/gen/recomp.c "@recomp/all.txt"
```

Then run `port\compile_generated.ps1` and link `librecomp.dll` with clang: `-shared -fuse-ld=lld -g0 -Wl,/EXPORT:bb_recomp_init "-Wl,/MAP:librecomp.map" @link.rsp` plus libbbcpu, Zydis, Zycore and the clang builtins. The map file resolves `game+offset` frames to `f_<guest address>`.

Pipeline before that, in order: extract the pkg with PkgTool, analyze in Ghidra with the Orbis SELF loader (disable switch analysis and "Non-Returning Functions - Discovered"), then export the code map with `analysis\scripts`. `.eh_frame` supplies the function ranges and `SwitchRecover.java` the jump tables. Expected result: 13,375+ functions, 962 tables, 0 errors.

## Debugging

| Variable | Effect |
|---|---|
| `VOX_TRACE=1` | Logs flip submissions and button changes |
| `VOX_PROFILE=<seconds>` | Thread CPU times and stack samples to `profile.tsv`; read with `analysis\scripts\profile_report.py <tsv> <llvm-symbolizer> <vox.exe> [all]` |
| `VOX_PROFILE_DELAY=<seconds>` or `stall` | When profiling starts; `stall` waits for flips to stop, then exits with code 42 |
| `VOX_LENIENT=1` | Unimplemented imports log and return 0 |
| `VOX_NO_GPU=1` | Headless |
| `VOX_PAD_AUTO=1` or `2` | Automatic button taps. **Don't use this unless the user asks for it.** It got in the way of their controller and wandered into the wrong mode |

`port\capture_window.ps1 -Delays "20,40" -Prefix shot` screenshots only the game's window. It attaches to a running game and doesn't launch one.

Exit codes: 40 fault, 41 watchdog (`-Timeout` reached, which is normal), 42 stall profiled, 43 the game raised a debug exception, 44 unimplemented import, 45 abort.

## Machine pitfalls

- A machine-wide `INCLUDE` variable points at Watcom and breaks clang. The build scripts clear it.
- Two Ghidra 12.1.4 installs share a settings folder unless the release is renamed. This copy has `application.release.name=VOX`. Set `JAVA_HOME` to `tools\jdk21\jdk-21.0.13+9` first.
- `VirtualQuery` on multi-GB regions costs milliseconds per call. That was the cause of an early 1 FPS problem, so keep it off hot paths.

## Working with the user

- Do only what's asked. Ask first about anything else, and no unrequested extras.
- Keep game data out of every push, and confirm before pushing.
- Warn the user before launching the game: a timed test closes itself and looks exactly like a crash. Don't launch or build while they're playing.
- When they report a crash, read `run.log` before anything else runs, because the next launch overwrites it.

## Credits and license

GPL-2.0-or-later (see `LICENSE`). Built on:
- [shadPS4](https://github.com/shadps4-emu/shadPS4) v0.21: renderer, shader recompiler, GNM, VideoOut, event queues
- [bbcpu](https://github.com/bloodborne-native/bbcpu) and [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) from bloodborne-native: CPU runtime and recompiler
- [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), [Ghidra](https://github.com/NationalSecurityAgency/ghidra), GhidraOrbis, [Zydis](https://github.com/zyantific/zydis), [SDL3](https://github.com/libsdl-org/SDL)

The game belongs to its rights holders and isn't distributed here.
