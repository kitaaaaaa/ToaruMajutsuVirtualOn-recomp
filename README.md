# A Certain Magical Virtual-On: Windows recompilation

A native Windows port of the PS4 game *A Certain Magical Virtual-On* (とある魔術の電脳戦機), made by statically recompiling its code. **No game files are included: you need your own copy.**

## How it works

- **Recompiler:** the game's x86-64 code is translated to C and compiled into `librecomp.dll`. Anything it can't translate falls back to an interpreter.
- **Runtime (`vox.exe`):** loads the game and stands in for the PS4's system libraries: files, threads, memory, saves, controller, audio, movies. PSN features are disabled, so play is offline.
- **Renderer:** shadPS4's PS4 graphics stack, running on Vulkan.
- **Translations:** applied as an overlay while the game reads its archives. Your game files are never modified.

## Status

Menus, battles, story mode, movies and sound work. An Xbox controller is supported through XInput, with no rumble yet. This is early software: expect bugs, including an occasional freeze whose cause isn't fully understood.

## Building

You need Windows, Git, Python 3, Visual Studio with "Desktop development with C++", a Vulkan GPU, and about 10 GB free plus 12 GB for the game. Your game must be a fake-signed `.pkg` or an already extracted folder containing `eboot.bin`.

```powershell
port\setup.ps1                        # fetch tools and sources
port\build_all.ps1                    # build the engine
port\prepare_game.ps1 -Pkg game.pkg   # or: -Game <folder>
```

## Playing

```powershell
port\run.ps1 -Timeout 3600 -NoBuild              # Japanese
port\run.ps1 -Timeout 3600 -NoBuild -English     # budget English translation
port\run.ps1 -Timeout 3600 -NoBuild -UserTl      # installed fan translation, see below
port\run.ps1 -Timeout 3600 -NoBuild -UnlockAll   # everything unlocked; combines with the others
```

`-Timeout` is how many seconds until the game closes itself. The log is `port\work\run.log`, and the next launch overwrites it.

`-UnlockAll` opens every robot, colour, mission, title and gallery item, sets your points to 9,999,999, marks the main story chapters cleared and turns on the DLC stories and colours. **It changes your save** as soon as the game next saves, and turning it off won't undo that, so back up `port\work\user\savedata` first if you care. Some colours and gallery entries may have no files on the disc, so they could misbehave.

Face buttons match by position, so Xbox B is PS4 Circle, which confirms in this version.

## Using the Vita fan translation

MakutaMaster962 (also known as The World - 2.0) spent over three years on a real English translation of the PS Vita version. It isn't included here, but you can play its text on this port. You need your own copy of the Vita game, with its v1.04 update:

1. Get the `.xdelta` patches from [the GBAtemp thread](https://gbatemp.net/threads/toaru-majutsu-no-virtual-on-english-translation-xdelta-patches.679584/).
2. Copy these Vita files into one empty folder. The story DLCs are optional.

   | File | Patch |
   |---|---|
   | `disc00.cpk`, from `ux0\app\PCSG01089\cpk` | `Base Game\disc00.xdelta` |
   | `Patch01.cpk`, from `ux0\app\PCSG01089\patch_cpk` | `1.04 Update\Patch01.xdelta` |
   | `DLC_Story_A.cpk` to `E`, from `ux0\addcont\PCSG01089\DLCXSMPXXXXXX...\data` | `DLCs\...\DLC_Story_A.xdelta` to `E` |

3. Apply each patch to its copy with [Delta Patcher](https://github.com/marco-calautti/DeltaPatcher).
4. Install the result. This also accepts a `.rar`, `.7z` or `.zip` of that folder (archives need 7-Zip):

   ```powershell
   port\install_vita_patch.ps1 -Patch <folder>
   ```

5. Play with `-UserTl`.

Only text carries over. The patch's translated graphics are in the Vita's format, so art with Japanese baked in stays Japanese. The author asks for credit if you post gameplay.

## Credits and license

GPL-2.0-or-later, see [LICENSE](LICENSE). Built on [shadPS4](https://github.com/shadps4-emu/shadPS4), [bbcpu](https://github.com/bloodborne-native/bbcpu), [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp), [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), [Zydis](https://github.com/zyantific/zydis) and [SDL](https://github.com/libsdl-org/SDL). Not affiliated with SEGA, Kadokawa or Sony. Development notes are in [PASSDOWN.md](PASSDOWN.md).
