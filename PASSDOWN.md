# Passdown notes

Working notes for whoever picks this up. They are not a user guide: the project isn't ready for players.

## What this is

A static recompilation of the PS4 game *A Certain Magical Virtual-On* (とある魔術の電脳戦機, `JP0177-CUSA08425_00-VOX2017INDEX0000`, v01.01, SDK 4.50) to native Windows x64. The game's x86-64 code is lifted to C, compiled with clang into `librecomp.dll`, and run by `vox.exe` with reimplemented PS4 system libraries and shadPS4's Vulkan renderer.

**No game data goes in this repo, ever.** You need your own dump of the game. `.gitignore` is a whitelist for that reason. Don't add anything from `extracted`, `port\work`, `analysis\export`, Ghidra projects or the generated C, all of which derive from the game.

## Current state (2026-11-07)

Test machine: Windows 10, RTX 2080.

| Area | State |
|---|---|
| Boot to title screen and menus | Works |
| Attract-mode battle rendering | Works, about 60 flips per second |
| Battles and most UI | Work (user's report), including with the controller |
| Xbox controller (XInput) | Works. No rumble (`scePadSetVibration` is a no-op) |
| Sound (SDL3) | Works by ear (the user's report); no movie sound |
| Movies | All play through Media Foundation (`hle_movie.c`), picture and sound; the menu panel videos and story CGs are confirmed working. If decoding fails, a looping movie gives one black frame and a one-shot movie just ends |
| English | `-English` replaces about 8,860 lines with a deliberately terrible joke translation; `-UserTl` plays a user-supplied Vita fan patch. See Translation below |
| PSN | Unimplemented `sceNp*` calls answer `ORBIS_NP_ERROR_SIGNED_OUT`; `sceRudp*` calls answer `0x80770001` (not initialized, from memory and unverified) |
| Saves, trophies | Saves work; trophies are logged only |
| Unlocks | `-UnlockAll` (`--unlock-all`) opens everything and turns on the DLC stories and colours. Tested as far as the game booting with it and overriding a loaded save; nobody has reported on the unlocked screens yet |
| Model export | All 15 robots to glTF, with motions, weapons and a set with weapons attached. See `analysis\scripts\MODEL_FORMAT_NOTES.md` |

## Open problems, most urgent first

1. **Missing letters in English text (user report, early).** Not seen in any capture, and other English lines drew complete. It may be gone or specific to some screens. Capture one with `capture_window.ps1` first. Ruled out: the fonts. `unicode_font_jp_8bpp.ufn` has no ASCII, only fullwidth, but `unicode_font_ascii_8bpp.ufn` has every glyph (all rendered and checked), and `FUN_01156010` registers both, each with a `0x100` argument. Unconfirmed ideas: a glyph cache limit, the renderer missing updates to a glyph atlas, or code measuring ASCII text as Japanese.
2. **Intermittent freeze.** Seen in attract battles (about 50–65 s in) and mid-match. All threads go idle with 0 CPU. Evidence from every stall:
   - The GPU thread is in the WAIT_REG_MEM label wait in `ProcessGraphics` (`liverpool.cpp`), waiting for `buffer_labels[1] == 0`.
   - Labels are `0 1 0 0...`, `prev_index` is 1, flips pending 0, gc queue 0, 3 gfx submits queued, 0 commands.
   - A label only clears in `VideoOutDriver::Flip` when a later flip replaces that buffer, and CPU flips must pass through the sleeping GPU thread.
   - **Workaround:** after 200 ms in that state the wait releases the label and logs `CP VO label wait released`. It fired once in a user session, during a flip-rate dip, and got past the freeze. The game then crashed on shadPS4's "Out of order flip IRQ" assertion.
   - `VideoOutDriver::Flip` skips clearing a buffer's label when it's flipped twice in a row, and the assertion became a logged warning (`VO label N was cleared before its flip`). That guard stays, but the theory behind it was wrong: the game's frame code (`FUN_01384e40`) walks its buffers as a strict ring, so it doesn't choose the buffer on screen.
   - Open: either a frame's wait and flip target different buffers, or a flip gets lost, for instance when a frame submits no contexts. Every stall report and every release of the wait now prints the last 256 flip events as `FLIPLOG` lines in the log, which should settle it. No freeze has happened with this build yet.
   - Ruled out: a label written directly by the game (the wait re-checks on a timer).
3. **Crash on pressing start in attract mode (user report, once).** No log survived, and it may have been the freeze. Note the log gets overwritten by every run.
4. **Starting a match "crashes" (early user report).** An unimplemented `sceRudpEnd` stopped the game during online shutdown after a signed-out PSN error. `sceNp*` and `sceRudp*` calls now return errors. Battles have worked since, but the user's menu path to that stop wasn't confirmed.
5. **Movie notes.** Only `open00`, `staffroll` and `to_be_continued_00/01` have a sound track; the `adv` story CGs are video only by design. The story mode hang was fixed by real movie frames: the game's state machine (`FUN_010fda80`, state 2) waits for a flag only `sceAvPlayerGetVideoDataEx` sets. Media Foundation reports 1080 rows for frames whose buffer holds 1088, so `hle_movie.c` finds the chroma plane from the buffer's size.
6. **158 known imports are unimplemented.** Any other than `sceNp*` and `sceRudp*` stops the game by default and prints `STOP: unimplemented ...`. `VOX_LENIENT=1` logs and returns 0 instead.
7. **Remaining Phase 6 work:** movie sound and one-shot movies, rumble, and dialogs without UI.

Smaller items:
- `libc` and `libSceFios2` still run in the interpreter.
- Unmaps now update the renderer's mapping table, but the renderer's own caches aren't told about them, so stale entries are possible.
- `liverpool.cpp` still holds diagnostics (`vox_recent`, the CP wait prints). They're useful for now.
- A user report of "7 failed background operations" during Phase 5 was never explained.

## Translation

The user's direction: the distributed English is **a joke on purpose**, in the spirit of Zero Wing, bootleg comics and Kung Pow. Menus are as literal as possible so people can still get around. The story is meant to be impossible to follow. Baked-in Japanese art stays as it is. A real translation already exists, by fans, for the Vita version: don't copy it, support installing it instead.

| Option | Plays | Source |
|---|---|---|
| none | Japanese | The game |
| `port\run.ps1 -English` (`vox.exe --english`) | The joke translation | `translation\*.en`, in this repo |
| `port\run.ps1 -UserTl` (`vox.exe --user-tl`) | The user's own Vita fan patch, text only | Their copy, via `port\install_vita_patch.ps1 -Patch <rar or folder>` |

Everything runs on the user's own copy of the game: extract with `analysis\scripts\cpk.py`, dump with `mtp.py dump`, and keep Japanese dumps out of the repo. **Never commit anything from a Vita patch.** It only lives in `extracted\vita_patch` and `port\work\translation_user`, both git-ignored.

- **Regenerate the joke:** from `analysis\scripts`, run `wacky.py <extracted>\mtp_text <repo>\translation`, then `tl_pack.py <repo>\translation <extracted>\uroot\cpk <repo>\port\work\translation`. The game needs a restart to pick up new overlays.
- **What's generated:** `wacky.py` writes everything except its `HAND` list of literal menu files, which are edited by hand. It uses only each line's tags, punctuation, length and katakana, so its output holds no game text beyond romanized katakana words. Output is deterministic. The voice gallery's text lists (`voice_*`) are deliberately left Japanese. Audio is never touched.
- **Overlays:** the runtime lays `<folder>\<disc>.overlay` over an archive as it's read, and never changes the CPKs. A file too big for its slot goes past the archive's end, its TOC row is patched in the overlay, and the runtime reports the larger size. The ITOC and GTOC aren't patched: the game seems to look files up by name, but that isn't proven.
- **Where the text is:** `root+/mtpa/*.mtp`, 496 files, Shift-JIS, about 200,000 Japanese characters. `mtp.py check` rebuilds all 494 parseable files byte for byte; `_MTPA_story_kamijo_01.mtp` and `message00.mtp` don't parse, reason unknown.
- **Vita patch format:** same `.mtp` format as PS4. The `.xdelta` patches' per-window checksums all match the patched `.cpk` files tested here, so those are exactly what the patches produce. That patch overwrites strings in place and **doesn't update their length fields**: they can be too long, too short or zero, and only the NUL ends a string. Never trust those lengths when reading a fan-patched file, or lines come out empty or chopped (`mtp.read` does trust them, so it's for the game's own files). `tl_pack.py --vita` reads to the NUL and rebuilds each file on the PS4 file's tables, with real lengths. Its textures and fonts (`*_1bpp.ufn`) aren't in the PS4's format and aren't used. `<Effect N/A>` and `<MB is` are literal author text left as they are; whether the PS4 parser minds them is unknown.
- **Art:** main menu buttons are already English in the textures. Other words are baked into `.htx` textures (DDS behind a small header; view with `htx2png.py`).
- **Content rule:** generated text stays silly, never sexual. Two DLC titles (`story_title_01` ids 19 and 20) are deliberately left in Japanese because of sexualized jokes around teenage characters.

## Layout

| Path | What it is |
|---|---|
| `port\runtime` | `vox.exe`: loader, HLE of PS4 libraries, profiler |
| `port\gpu\shadps4` | shadPS4 v0.21 renderer, GNM and VideoOut, vendored. Local edits are marked `// vox:` |
| `port\gpu\shim` | Windows replacements for shadPS4's core, including the guest mapping table in `core\memory_win.cpp` |
| `port\patches` | Our changes to bbcpu and bloodborne-recomp, applied by `port\fetch_forks.ps1` at pinned commits |
| `analysis\scripts` | Ghidra scripts and Python tools for the code map; also `hmd2glb.py` and `export_models.py` for the model export |

Not in the repo, on the development machine:
- `extracted\`: the game's files, and `extracted\models`, the exported models. Never publish these.
- `analysis\vox*.gpr`, `analysis\export\`: Ghidra projects and the exported code map. Use `voxfix`.
- `port\work\`: game-derived build output, generated C, `librecomp.dll`, logs and saves.
- `port\deps`: Zydis and Zycore headers and libs (source in `tools\src\zydis`).
- `tools\`: PkgTool, Ghidra 12.1.4 + GhidraOrbis, JDK 21, LLVM, CMake, and pristine reference clones in `tools\src` (shadPS4 v0.21, LibOrbisPkg, bbcpu, bloodborne-recomp, bloodborne-runtime, zydis).

## Build and run

Needs Windows, Git, Python 3, Visual Studio's "Desktop development with C++" (clang uses its libraries and the Windows SDK) and about 10 GB free, plus room for your game (12 GB extracted). 7-Zip only if installing a patch from an archive.

```powershell
port\setup.ps1                        # tools and sources at pinned versions, in tools\ and port\
port\build_all.ps1                    # the engine: port\out\vox.exe. No game needed
port\prepare_game.ps1 -Pkg game.pkg   # or -Game <folder with eboot.bin>: your copy to librecomp.dll
port\run.ps1 -Timeout 3600 -NoBuild -English
```

`run.ps1` also rebuilds `vox.exe` first unless given `-NoBuild`. Its log is `port\work\run.log`.

Tested from a clean folder on the development machine, using every published file. Setup and the engine build took about 15 minutes on first run and the game about 2 minutes. `-Pkg` and PkgTool's download have not been run through this script.

**Jump tables are optional.** `scan.exe` finds all 13,771 functions from `.eh_frame`, byte-identical to before, and `prepare_game.ps1` needs no Ghidra. Without tables, bbrecomp decodes 849 jump tables and leaves 269 indirect jumps to the interpreter. With `-Tables <known_tables.tsv>` it decodes 962 and leaves 168, and the generated C was byte-identical to the working copy's. Without tables, only a boot and the opening have been tested, not story or battles.

`known_tables.tsv` comes from Ghidra, by hand: analyze with the Orbis SELF loader (disable switch analysis and "Non-Returning Functions - Discovered"), run `SwitchRecover.java`, then `tables_tsv.py` on the exported `jump_tables.csv`. It isn't in this repo.

The link's map file (`port\work\recomp\librecomp.map`) resolves `game+offset` frames to `f_<guest address>`.

## Debugging

| Variable | Effect |
|---|---|
| `VOX_TRACE=1` | Logs flip submissions and button changes |
| `VOX_PROFILE=<seconds>` | Thread CPU times and stack samples to `profile.tsv`; read with `analysis\scripts\profile_report.py <tsv> <llvm-symbolizer> <vox.exe> [all]` |
| `VOX_PROFILE_DELAY=<seconds>` or `stall` | When profiling starts; `stall` waits for flips to stop, then exits with code 42 |
| `VOX_TEST_STALL=<seconds>` | Writes a stall report at that time, to test it. Real ones are automatic when flips stop for two heartbeats; read them with `analysis\scripts\stall_report.py` |
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
