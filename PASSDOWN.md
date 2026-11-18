# Passdown notes

Working notes for whoever picks this up. They are not a user guide: the project isn't ready for players.

## What this is

A static recompilation of the PS4 game *A Certain Magical Virtual-On* (とある魔術の電脳戦機, `JP0177-CUSA08425_00-VOX2017INDEX0000`, v01.01, SDK 4.50) to native Windows x64. The game's x86-64 code is lifted to C, compiled with clang into `librecomp.dll`, and run by `vox.exe` with reimplemented PS4 system libraries and shadPS4's Vulkan renderer.

**No game data goes in this repo, ever.** You need your own dump of the game. `.gitignore` is a whitelist for that reason. Don't add anything from `extracted`, `port\work`, `analysis\export`, Ghidra projects or the generated C, all of which derive from the game.

## Current state (2026-11-16)

Test machine: Windows 10, RTX 2080.

| Area | State |
|---|---|
| Boot to title screen and menus | Works |
| Attract-mode battle rendering | Works, about 60 flips per second |
| Battles and most UI | Work (user's report), including with the controller |
| Xbox controller (XInput) | Works. No rumble (`scePadSetVibration` is a no-op) |
| Sound (SDL3) | Works by ear (the user's report); no movie sound |
| Movies | All play through Media Foundation (`hle_movie.c`), picture and sound; the menu panel videos and story CGs are confirmed working. If decoding fails, a looping movie gives one black frame and a one-shot movie just ends |
| English | `-English` plays our full translation of all 10,880 Japanese lines; `-Engrish` replaces about 8,860 lines with a deliberately terrible joke translation; `-UserTl` plays a user-supplied Vita fan patch. See Translation below |
| PSN | Unimplemented `sceNp*` calls answer `ORBIS_NP_ERROR_SIGNED_OUT`; `sceRudp*` calls answer `0x80770001` (not initialized, from memory and unverified); `sceNetCtlUnregisterCallback` answers 0, so the Network and Co-op menus show their error and return instead of stopping (user-tested) |
| Saves, trophies | Saves work; trophies are logged only |
| Unlocks | `-UnlockAll` (`--unlock-all`) opens everything and turns on the DLC stories and colours. Tested as far as the game booting with it and overriding a loaded save; nobody has reported on the unlocked screens yet |
| Model export | All 15 robots to glTF, with motions and weapons, and a set with weapons mounted by the game's own rules. PPRIMA has no armed model. See Model export below |
| Debug menu | `--debug` opens the developers' menu and performance overlay. Tested: Title, Battle, quitting back with L1+R1+Options. See Debug menu, Neo and frame rate below |
| Neo (PS4 Pro) | `--neo`. Battles render and hold their frame rate with it |
| Frame rate | `--fps 120` measured 116-120 flips a second in battle, at real-time game speed. Story mode and long sessions at 120 untested |
| Loose files | The game reads `root+` files from `/app0` when no CPK mounts. Title and a battle tested from an extracted copy. Translation overlays don't apply then |

## Open problems, most urgent first

1. **Missing letters in English text (user report, early).** Not seen in any capture, and other English lines drew complete. It may be gone or specific to some screens. Capture one with `capture_window.ps1` first. Ruled out: the fonts. `unicode_font_jp_8bpp.ufn` has no ASCII, only fullwidth, but `unicode_font_ascii_8bpp.ufn` has every glyph (all rendered and checked), and `FUN_01156010` registers both, each with a `0x100` argument. Unconfirmed ideas: a glyph cache limit, the renderer missing updates to a glyph atlas, or code measuring ASCII text as Japanese.
2. **Intermittent freeze (fixed 2026-11-12, see the last points).** Seen in attract battles (about 50–65 s in) and mid-match. All threads go idle with 0 CPU. Evidence from every stall:
   - The GPU thread is in the WAIT_REG_MEM label wait in `ProcessGraphics` (`liverpool.cpp`), waiting for `buffer_labels[1] == 0`.
   - Labels are `0 1 0 0...`, `prev_index` is 1, flips pending 0, gc queue 0, 3 gfx submits queued, 0 commands.
   - A label only clears in `VideoOutDriver::Flip` when a later flip replaces that buffer, and CPU flips must pass through the sleeping GPU thread.
   - **Workaround:** after 200 ms in that state the wait releases the label and logs `CP VO label wait released`. It fired once in a user session, during a flip-rate dip, and got past the freeze. The game then crashed on shadPS4's "Out of order flip IRQ" assertion.
   - `VideoOutDriver::Flip` skips clearing a buffer's label when it's flipped twice in a row, and the assertion became a logged warning (`VO label N was cleared before its flip`). The theory behind that guard was wrong: the game walks its buffers as a strict ring.
   - **Cause, measured (2026-11-07).** `sceGnmSubmitDone` resets the copy buffer and locks submissions until the `GpuIdle` interrupt. The GPU thread sends that some time after its queue empties, after a flush, without rechecking. A frame and a `submitDone` landing in that gap get the lock cleared with the frame still queued (seen as idle with 2 submits queued), so the next submit copies over it, and that frame then waits on the on-screen buffer's label.
   - **Deliberately left alone.** The user only sees it before the SEGA logo, not in play, and lost the intro sounds during the work on it, so the logging and the fix were rolled back at their request. Whether that work caused the sound loss wasn't established: the level at the audio ports measured the same with it in. The fix, ignoring an idle signal while submits are queued in `ResetSubmissionLock`, froze 0 of 10 runs against 3 of 3 with a 20 ms delay injected before `submitDone`, in an unfinished test.
   - **Fixed (2026-11-12), at the user's request.** It came back in play: two of the user's story-mode runs froze at a battle load, each right after one `CP VO label wait released`, with 3 submits queued. Every thread was waiting: the game in `sceKernelWaitEqueue`, the GPU thread idle in `Liverpool::Process`. `ResetSubmissionLock` (`gnmdriver.cpp`) now ignores a `GpuIdle` signal while submits are still queued, and the signal after they run clears the lock. Since then: 5 user runs, about 2 hours with story battles, 0 freezes, 0 label-wait releases. The intro sounds play (audio ports 1 and 2 both report sound, as before). Logs are in `work\freeze` (local only).
   - Ruled out: a label written directly by the game (the wait re-checks on a timer).
3. **Crash on pressing start in attract mode (user report, once).** No log survived, and it may have been the freeze. Note the log gets overwritten by every run.
4. **Starting a match "crashes" (early user report).** An unimplemented `sceRudpEnd` stopped the game during online shutdown after a signed-out PSN error. `sceNp*` and `sceRudp*` calls now return errors. Battles have worked since, but the user's menu path to that stop wasn't confirmed.
5. **Movie notes.** Only `open00`, `staffroll` and `to_be_continued_00/01` have a sound track; the `adv` story CGs are video only by design. The story mode hang was fixed by real movie frames: the game's state machine (`FUN_010fda80`, state 2) waits for a flag only `sceAvPlayerGetVideoDataEx` sets. Media Foundation reports 1080 rows for frames whose buffer holds 1088, so `hle_movie.c` finds the chroma plane from the buffer's size.
6. **158 known imports are unimplemented.** Any other than `sceNp*`, `sceRudp*` and `sceNetCtlUnregisterCallback` stops the game by default and prints `STOP: unimplemented ...`. `VOX_LENIENT=1` logs and returns 0 instead.
7. **Remaining Phase 6 work:** movie sound and one-shot movies, rumble, and dialogs without UI.

Smaller items:
- `libc` and `libSceFios2` still run in the interpreter.
- Unmaps now update the renderer's mapping table, but the renderer's own caches aren't told about them, so stale entries are possible.
- `liverpool.cpp` still holds diagnostics (`vox_recent`, the CP wait prints). They're useful for now.
- A user report of "7 failed background operations" during Phase 5 was never explained.

## Debug menu, Neo and frame rate

All of this is in `port\runtime\loader.c`. Addresses are image offsets.

**Debug menu (`--debug`).** The process manager (pointer at `+0x6D11F0`) launches the auto boot entry at its `+0xD0` ("BOOT") instead of drawing its "VOX DEBUG ROOT MENU" (rev 20527, built Dec 16 2017). The switch clears that entry once the game sets it, then sets the debug mode flag (`+0x6EB57C`) and the overlay flag (`+0x6EB580`) once the manager reaches its menu state.
- Entries: Battle, NetworkRoom, NetworkRank, MissionSelect, StorySelect, Title, Boot, Exit. D-pad moves, Circle launches. With debug mode on, holding L1+R1+Options quits to the menu.
- Overlay: NEO on or off, the game's CPU and GPU times, DLT, the output mode with **its configured rate, not a measured one** (59.94 whatever the real rate), and whether a CPK is mounted.
- The game's own codes: debug mode is Up, Down, Up, Down, Left, Right, Left, Right then logical button `0x2000`; the overlay, with debug mode on, Up, Down, Up, Down then `0x2000` (`FUN_01001090`). No retail control produces `0x2000` or `0x1000`: every button, trigger and stick direction and the pad's remap table were checked. So they can't be entered on a normal game.
- Quitting to the menu tears the scene down, which empties the battle renderer's list (`+0xD2F528`, first entry `+0x58`) but leaves its loader's state (`+0x1A8`) at done (`0x100`), so the next battle crashed on a null read at `+0x26618C`. The debug thread resets the state and the game reloads the list. The retail game never tears down.

**Neo (`--neo`).** `sceKernelIsNeoMode` answers 1. The framework keeps it at `+0x294`. What it changes, seen in the code: `FUN_012142c0` draws every object group instead of one when some count reaches 21, and the GPU clock the engine assumes goes from 800 to 911 MHz. No change to the output resolution was found.

**Frame rate (`--fps N`).** shadPS4's vblank timer was the 60 cap; it now runs at N (`VoxGpuConfig.vblank_hz`). The game shipped with a fixed step: its framework object (pointer at `+0x6BA108`) has a flag at `+0x2E0` that makes the frame delta (`+0x2D0`, the overlay's DLT) always 1. Cleared, the delta is the measured frame time over 1/60 s (`FUN_01000be0`), and above 5 it resets to 1. The game runs at real-time speed that way; the battle clock confirmed it.
- Some code rounds the delta down: an integer counter plus the delta, or a loop run `(int)delta` times, which never move below 1. That froze the Story Select background. Each frame, right after the delta is stored (a probe at `+0xC7B`), the whole frames it completes go into `+0x2D4`, unused padding, and seven reads moved there (`whole_frame_reads`). Their functions run in the interpreter, as their bytes changed; no cost was measured.
- They were found with a test build: every truncating conversion goes through `rc_f2i32` and `rc_f2i64` in bbcpu's `rc.h`, and a hook there logged each one whose value was a whole number plus or minus the delta. The hook and that build are local only (`work\unused\f2i_watch`, not in `port\patches`). Most logged sites were coincidences (positions, colour rounding), sorted by reading the code.
- Known left: `FUN_0100b3b0` runs `max(1, (int)(speed * delta))` steps a frame, so whatever it drives probably runs twice as fast at 120. A one-byte change can't fix it without breaking slow motion. Not checked: the stage monorail trains, story mode, other stages.

**Performance.** At 120 the GPU command thread was the limit, and most of its work was shadPS4's texture cache hashing whole images, about 1400 a second and 8 GB/s in battle, for a check made only when a GPU-written image is evicted. It now hashes 16 spread 4 KiB slices (`texture_cache.cpp`, `// vox:`): battles went from 75-100 to 116-120 flips a second. A CPU write that misses every slice would go unseen, and the GPU's data would be kept.

**Loose files.** With no `cpk` folder the CPK mount fails cleanly and the game reads `root+/<path>` as `/app0/<path>`. Make such a folder with `cpk.py extract` of both discs, the extracted `root+` contents at its top, and the rest of `uroot` (all but `cpk`) beside them; hard links save the space. `run.ps1 -Loose` plays from `extracted\loose`. The overlay then reads "Not Use CpkFile".

**Leftovers on the disc** (from reading files and code, not all confirmed unused): `se_menu_old`, the old Oratan menu sound bank, which the code never names; MX Composer stage-editor headers and samples in `root+/bg/param`; PC test textures (`common_textures/mesh_pattern` as png and bmp); sample shaders for PS3, Vita and Windows; 10 movies that are one identical 8 KB placeholder (`menu06`-`13`, `tutorial00`, `ed00`); the Windows E-mote driver DLLs; stage numbers 017 and 018 and voice folder 12 are missing.

## Model export

`analysis\scripts\export_models.py` exports every robot with `hmd2glb.py`, from files you extract from your own copy; the output is game data and stays out of the repo:

```powershell
python analysis\scripts\cpk.py extract extracted\uroot\cpk\disc00.cpk extracted\cpk_out "root+/chara/VR*" "root+/motion/*" "root+/param/*"
python analysis\scripts\cpk.py extract extracted\uroot\cpk\disc01.cpk extracted\cpk_out "root+/chara/VR*" "root+/motion/*" "root+/param/*"
python analysis\scripts\export_models.py extracted\cpk_out\root+ extracted\models
```

- Out come each robot's bodies per colour, its weapons, its low-detail models and `MANIFEST.txt`, as rigged, animated glTF binaries. Colours: C00 and C01 are types A and B, C02 and C03 the VOOT colours, C04 the special colour, `H` Fei-Yen's hyper mode.
- The `armed` folder has each body with its default weapons mounted by the game's own setup (`MOUNTS` in `export_models.py`), so they follow every motion. PPRIMA has none: its floating units are placed by the game.
- A weapon with no texture bank of its own uses the one `mdl_param` or `decken_model` pairs it with, or the nearest weapon's with the same material layout; the manifest says which.
- `hmd2glb.py` also converts one model by hand, with its textures, motion banks and `+ids` weapon mounts; its docstring has the usage.
- The formats, and what's verified and what isn't, are in `analysis\scripts\MODEL_FORMAT_NOTES.md`.

## Translation

The user's direction: `-Engrish` is **a joke on purpose**, in the spirit of Zero Wing, bootleg comics and Kung Pow. Menus are as literal as possible so people can still get around. The story is meant to be impossible to follow. Baked-in Japanese art stays as it is. A real translation already exists, by fans, for the Vita version: don't copy it, support installing it instead.

The user found the Vita patch's story badly machine translated, so `-English` is now our own full translation from the Japanese (2026-11-12): all story and menu text, voice gallery included. Their rule for it: never change the context or content of a joke or reference from its literal translation. Honorifics and verbal tics stay. The guide, glossary and checker used are in `work\tl` (`GUIDE.md`, `tl_check.py`).

| Option | Plays | Source |
|---|---|---|
| none | Japanese | The game |
| `port\run.ps1 -English` (`vox.exe --english`) | The full translation | `translation_english\*.en` |
| `port\run.ps1 -Engrish` (`vox.exe --engrish`) | The joke translation | `translation\*.en`, in this repo |
| `port\run.ps1 -UserTl` (`vox.exe --user-tl`) | The user's own Vita fan patch, text only | Their copy, via `port\install_vita_patch.ps1 -Patch <rar or folder>` |

Everything runs on the user's own copy of the game: extract with `analysis\scripts\cpk.py`, dump with `mtp.py dump`, and keep Japanese dumps out of the repo. **Never commit anything from a Vita patch.** It only lives in `extracted\vita_patch` and `port\work\translation_user`, both git-ignored.

- **Rebuild the full translation:** from `analysis\scripts`, `tl_pack.py <repo>\translation_english <extracted>\uroot\cpk <repo>\port\work\translation_english`. `prepare_game.ps1` does it when the folder exists. Ids whose Japanese is empty, ASCII or only a full-width space are left out on purpose.
- **Box limits, measured in game:** the story select description box draws at most 127 characters, line breaks included (Japanese never passes 84). The story dialogue box holds about 1285 units of the ASCII font per row; the widths are the byte table at `0xA0` in `unicode_font_ascii_8bpp.ufn` (characters 0x20-0x7E), drawn at 0.83 px per unit. Story text is kept within 1250 units per row, 2 rows. `work\tl\tl_check.py` warns on both limits. Battle and tutorial boxes aren't measured yet.
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
port\out\vox.exe --english
```

`run.ps1` takes `-DebugMenu`, `-Neo`, `-Fps N` and `-Loose` for the options above. For development, `port\run.ps1` also rebuilds `vox.exe` unless given `-NoBuild`, and the game closes itself after `-Timeout` seconds, 30 by default. Run without the `ELF_DIR NAMES.TSV` arguments, `vox.exe` finds its folders from its own path and never times out. Either way the log is `port\work\run.log`.

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
| `VOX_PAD_SCRIPT=<file>` | Presses at set times: lines of `<seconds> <PS4 buttons, hex> [<hold ms>]`, the bits above 0xFFFFFF pushing a stick. **The same rule applies: only when the user asks** |
| `VOX_WATCH=1` | Logs the debug flags, the active pad's own bits, the pad remap table, DLT each second, the battle renderer's list and movie frame requests |
| `VOX_PROBE=0x...,0x...` | Runs those functions in the interpreter and logs each call's first argument and the dword at `+0x14` in it, and calls per second |

`port\capture_window.ps1 -Delays "20,40" -Prefix shot` screenshots only the game's window. It attaches to a running game and doesn't launch one.

Exit codes: 40 fault, 41 watchdog (`-Timeout` reached, which is normal), 42 stall profiled, 43 the game raised a debug exception, 44 unimplemented import, 45 abort.

## Machine pitfalls

- A machine-wide `INCLUDE` variable points at Watcom and breaks clang. The build scripts clear it.
- Two Ghidra 12.1.4 installs share a settings folder unless the release is renamed. This copy has `application.release.name=VOX`. Set `JAVA_HOME` to `tools\jdk21\jdk-21.0.13+9` first.
- `VirtualQuery` on multi-GB regions costs milliseconds per call. That was the cause of an early 1 FPS problem, so keep it off hot paths.
- A system-wide thread snapshot takes about 2 s with a browser open. The profiler now lists threads once.
- `VOX_PROBE` offsets need the `0x`: bbcpu's `BB_RECOMP_OFF` reads them that way, and without it the function stays recompiled and the probe never fires.

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
