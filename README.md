# This fork - Nintendo Wii (AI-assisted)

> [!NOTE]
> This repository is a **fork of [ButterscotchRunner/Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)** focused on a **native Nintendo Wii** port. The Wii work was built **with AI assistance** (Cursor / Codex agents) for my own amusement. No warranties this'll work properly or get updated!


|            |                                                                                                          |
| ---------- | -------------------------------------------------------------------------------------------------------- |
| Upstream   | [https://github.com/ButterscotchRunner/Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) |
| Fork focus | `PLATFORM=wii` - native GX renderer, AESND audio, HBC `boot.dol` packaging                               |
| Open todos | [`TODO-wii.md`](TODO-wii.md)                                                                             |




### Build & run (Wii)

Requires [devkitPro](https://devkitpro.org/) (`wii-dev`) and MSYS2 `cmake`.

```powershell
.\src\wii\scripts\setup-wii-deps.sh   # once, from MSYS2
.\src\wii\scripts\build-wii.ps1
.\src\wii\scripts\run-wii-dolphin.ps1
```

MCP-assisted debugging with [Felk's Python-scripting Dolphin](https://github.com/Felk/dolphin) + [mcp-dolphin](https://github.com/dmang-dev/mcp-dolphin):

```powershell
.\src\wii\scripts\run-wii-dolphin-mcp.ps1
```

Or from MSYS2 bash:

```bash
export DEVKITPRO=/c/msys64/opt/devkitpro
./src/wii/scripts/build-wii.sh
```

macOS / Linux (no host toolchain — official Docker image):

```bash
./src/wii/scripts/build-wii-docker.sh   # uses devkitpro/devkitppc (linux/arm64 on Apple Silicon)
./src/wii/scripts/run-wii-dolphin.sh    # mainline Dolphin; brew install --cask dolphin
```

Felk Python-scripting Dolphin + `mcp-dolphin` remain a **Windows** debug path (`src/wii/scripts/run-wii-dolphin-mcp.ps1`); Felk does not ship macOS scripting builds.

Outputs `build-wii/butterscotch.dol` and stages `build-wii/apps/butterscotch/{boot.dol,meta.xml,icon.png}`.

### Load the game (after you have a `.dol`)

Butterscotch looks for game files under **`sd:/apps/butterscotch/`** (HBC layout). Put the runner and Undertale assets in the same folder:

```
apps/butterscotch/
  boot.dol          # from the build (or rename butterscotch.dol → boot.dol)
  meta.xml          # from packaging/wii/ (HBC listing)
  icon.png          # from packaging/wii/ (HBC icon, 128×48)
  data.win          # Undertale data.win (WTL1-prepared if you use the texture pipeline)
  *.ogg             # Undertale music/SFX next to data.win (required for audio)
  CONFIG.JSN        # optional
  saves/            # created at runtime
```

**Homebrew Channel (real SD / USB loader):** copy that whole `apps/butterscotch` folder onto the SD card’s `apps/` directory, then launch **Butterscotch** from HBC.

**Dolphin:** enable Wii SD card + folder sync, point the sync folder at a directory that contains `apps/butterscotch/` as above (Windows often uses `%AppData%\Dolphin Emulator\Load\WiiSDSync`), then open/run `boot.dol` (File → Open, or `Dolphin.exe -e path\to\boot.dol`).

On first boot, use the on-screen shell (**Controls**) to pick a preset (vertical/horizontal Wiimote, GameCube, Classic). Default vertical Wiimote map: D-pad → arrows, A→Z, B→X, +→Enter, −→Shift, 1→C, 2→Escape. In-game, **HOME** opens the system menu.

---



# Upstream README

The content below is from upstream Butterscotch, kept here for reference. Wii-specific instructions live **only** in the fork section above.

# 🥧 Butterscotch 🥧

> [!IMPORTANT]
> Butterscotch is still VERY early in development and it is NOT that good yet.

When you create a game in GameMaker: Studio and export it, GameMaker: Studio exports the game code as bytecode instead of native compiled code, and that bytecode is compatible with any other GameMaker: Studio runner (also known as YoYo runner), as long as they have matching GameMaker: Studio versions. This is similar to how Java applications work.

This is how projects such as [Droidtale](https://mrpowergamerbr.com/projects/droidtale) (which was also made by yours truly) can exist. We exploit that GameMaker: Studio games compile to bytecode, which means they can be ran on *any* platform that has an official runner for it!

Ever since I created Droidtale 10+ years ago, I had that lingering thought in my mind... If GameMaker games use bytecode, what prevents us from creating our *own* runner? And if we can write our *own* runner, what prevents us from porting GameMaker: Studio games to other platforms?

And that's where Butterscotch comes in! Butterscotch is an open source re-implementation of GameMaker: Studio's runner.

**Butterscotch Web (WASM):** [https://butterscotch.mrpowergamerbr.com/web/](https://butterscotch.mrpowergamerbr.com/web/)

**Butterscotch PlayStation 2 ISO Generator:** [https://butterscotch.mrpowergamerbr.com/](https://butterscotch.mrpowergamerbr.com/)

## Game Compatibility

Butterscotch's goal is to be able to have Undertale v1.08 (GameMaker: Studio 1.4.1804, WAD Version 16) fully playable. But we do want to support more GameMaker: Studio games in the future too!

While our target is Undertale v1.08, that doesn't mean that other games CAN'T run in Butterscotch! Because Butterscotch is a runner and not a Undertale port/remake, you CAN run other GameMaker: Studio games with it and, as long as the game is compiled with GameMaker: Studio 1.4.1804 and they only use GML variables and functions that Butterscotch supports, it should work fine.

Butterscotch supports the following WAD versions:

- WAD Version 8 (GameMaker: Studio 1.0.198+)
- WAD Version 9 (GameMaker: Studio 1.0.527+)
- WAD Version 10 (GameMaker: Studio 1.1.609+)
- WAD Version 11 (GameMaker: Studio 1.1.754+)
- WAD Version 12 (GameMaker: Studio 1.1.867+)
- WAD Version 13 (GameMaker: Studio 1.1.917+)
- WAD Version 14 (GameMaker: Studio 1.4.1464+)
- WAD Version 15 (GameMaker: Studio 1.4.1675+)
- WAD Version 16 (GameMaker: Studio 1.4.1767+)
- WAD Version 17 (GameMaker: Studio 2.2+)

Other modding tools, such as UndertaleModTool, calls it "bytecode version" instead of "WAD version". We decided to go with WAD version instead because there are GameMaker: Studio versions (WAD version 6 and 7) that DO NOT use bytecode altogether, so calling it "bytecode version" is not quite correct, and because that's what the YoYo Runner calls it under the hood.

Versions before GameMaker: Studio 1.0.198 (that is, pre-WAD version 8) uses raw GML code interpreted on load, so these versions would require a GML compiler to be supported in Butterscotch.

However, that doesn't mean that a game that uses a compatible version WILL run! The bytecode support is still a WIP, and Butterscotch may have quirks that the original GameMaker: Studio runner may not have.

Of course, there are exceptions that break game compatibility altogether:

- Games compiled with YYC, because they use native code instead of bytecode.
- Games compiled with the new [GMRT](https://github.com/YoYoGames/GMRT-Beta/tree/main), because they use native code instead of bytecode.



## Supported Platforms

- Windows
- Web
- PlayStation 2
- PlayStation 3
- PlayStation Vita
- ...and maybe more in the future!

Additionally, any platform with reasonably complete C and POSIX conformance should work, the following have been tested.

- Linux with glibc as old as about ~1996
- FreeBSD as old as 2.2.8
- Haiku

The following backends are available for desktop platforms (Windows and POSIX systems).

- GLFW 2
- GLFW 3
- SDL 1.2
- SDL 2
- SDL 3

The following compilers have been tested to successfully build butterscotch, older versions may work but are untested.

- GCC 2.7 and up in C++ mode, and 3.0 and up in C99 mode
- Clang 1.1 and up
- TinyCC 0.9.27 and up
- MSVC 4.0 and up



## Community Ports

- [Xbox 360 (Butterscotch-360)](https://github.com/ceilingtilefan/Butterscotch-360) by @ceilingtilefan
- [3DS and Wii U (Cinnamon)](https://github.com/Project-Sunshine-Native/cinnamon) by @casrielasriel, @grayforz24682, @d16.dorian, @ralcactus



## Building Butterscotch

```bash
mkdir build && cd build
cmake -DPLATFORM=desktop -DDESKTOP_BACKEND=glfw3 -DCMAKE_BUILD_TYPE=Debug ..
make
```

If you are using CLion, set the platform in `Settings` > `Build, Execution, Deployment` > `CMake` and add `-DDESKTOP_BACKEND=glfw3`

Then run Butterscotch with `./butterscotch /path/to/data.win`!

## CLI parameters

The desktop target has a lot of nifty CLI parameters that you can use to trace and debug games running on it.

```
--help                                 - Show this message
--screenshot <filename>                - Specify the filename for screenshots
--screenshot-at-frame <frame>          - Take a screenshot at the specified frame
--screenshot-surfaces <filename>       - Take a screenshot of all surfaces at the specified frame
--screenshot-surfaces-at-frame <frame> - Specify the filename for surface screenshots
--headless                             - Launch without a window
--print-rooms                          - Print all rooms in the game and exit
--print-objects                        - Print all objects in the game and exit
--print-shaders                        - Print all shaders in the game and exit
--print-declared-functions             - Print all declared functions in the game and exit
--print-unknown-functions              - Print all unknown functions used by the game and exit
--trace-variable-reads                 - Trace variable reads
--trace-variable-writes                - Trace variable writes
--trace-function-calls                 - Trace function calls
--trace-alarms                         - Trace alarms
--trace-instance-lifecycles            - Trace instance creations and deletions
--trace-events                         - Trace events
--trace-collisions                     - Trace collisions between instances
--trace-event-inherited                - Trace event inherited calls
--trace-tiles                          - Trace drawn tiles
--trace-opcodes                        - Trace opcodes
--trace-stack                          - Trace stack
--trace-frames                         - Log frametimes
--always-log-unknown-functions         - Always log unknown function calls instead of once per script
--always-log-stubbed-functions         - Always log stubbed function calls instead of once per script
--exit-at-frame <frame>                - Exit at the specified frame
--trace-bytecode-after-frame <frame>   - Delay stack and opcode tracing until the specified frame
--dump-frame <frame>                   - Dump the runner state at the specified frame
--dump-frame-json <frame>              - Dump the runner state in json at the specified frame
--dump-frame-json-file <file>          - Specify an output file for runner state dumps
--speed <speed>                        - Set a normal speed multiplier
--fast-forward-speed <speed>           - Set a fast-forward speed multiplier
--seed <seed>                          - Seed for the random number generator
--debug                                - Enable debug mode
--disassemble <script>                 - Disassemble the specified script and print to console (\* disassembles all)
--record-inputs <file>                 - Record all keyboard inputs to a file
--playback-inputs <file>               - Playback input from file
--renderer <renderer>                  - Set the rendering API
--lazy-rooms                           - Lazily load rooms, increases load times but reduces memory usage
--eager-room <rooms>                   - When --lazy-rooms is set, keep these rooms always in memory
--os-type <os>                         - Set the reported OS type
--window-size <dimentions>             - Set a custom window size
--widescreen-hack <aspect ratio>       - Set a custom aspect ratio
--profile-gml-scripts                  - Log which GML scripts are the heaviest in terms of time and executed instructions
--save-folder <directory>              - Set the directory will save files will be stored
--game-args <args>                     - Arguments to pass to the game
--profile-opcodes                      - Rank which GML opcodes were executed the most
--lazy-textures                        - Load textures into VRAM on first use, improving startup times
--load-type <type>                     - Specify how data.win is loaded, per-chunk or all at once
--disable-log-colours                  - Disable colours for warning, error, and debug logs
--disable-log-colors                   - Same as --disable-log-colours, but different spelling
```



## Debug Features

When running Butterscotch with `--debug`, the following hotkeys are enabled:

- `Page Up`: Moves forward one room
- `Page Down`: Moves backwards one room
- `P`: Pauses the game
- `O`: While paused, advances the game loop by one frame
- `F12`: Dumps the current runner state to the console
- `F11`: Dumps the current runner state to the console (JSON format), or dumps it to a file if `--dump-frame-json-file` is set.
- `F10`: Sets the `global.interact` flag to `0`. Useful in Undertale when you are moving through rooms and one of them starts a cutscene that doesn't let you move.



## Performance

Performance is pretty good on any modern computer, but when running on low end targets (like the PS2) it is *very* slow when there's a lot of instances on screen, or when a instance does a for loop.

## Then why not have a transpiler?

The issue with a transpiler is that, if you try transpiling the game in the "naive" way, that is, emitting VM calls like it was the original bytecode, you won't get any
*improvement* from it, you would need to create a *good* transpiler that actually transpiles it into *good* code, and that's way harder.

Having a transpiler also have other disadvantages:

1. You lose the ability of debugging the runner at a "high level" by tracing opcodes.
2. Compilation is SLOW, transpiling Undertale in a naive way to C and building it takes 90 seconds on a modern computer, and building it to other targets is so slow that I wasn't even able to test it.



## Screenshots



### Undertale (GLFW) [WAD Version 16]



### Undertale (PlayStation 2) [WAD Version 16]

Here's a video :3 [https://youtu.be/PuzBxe0VGtY](https://youtu.be/PuzBxe0VGtY)

Here's also another video, this time showing off the Asriel Dreemurr final battle [https://youtu.be/vkQMqXr0MQE](https://youtu.be/vkQMqXr0MQE)

### DELTARUNE (SURVEY_PROGRAM) (PlayStation 2) [WAD Version 16]

Here's a video :3 [https://youtu.be/TLJtV2WnrmQ](https://youtu.be/TLJtV2WnrmQ)

### DELTARUNE Chapter 2 (GLFW) [WAD Version 17]



### DELTARUNE Chapter 2 (PlayStation 2) [WAD Version 17]

Here's a video :3 [https://youtu.be/uuN72Hv50d4](https://youtu.be/uuN72Hv50d4)

### DELTARUNE Chapter 3 (GLFW) [WAD Version 17]



### DELTARUNE Chapter 3 (PlayStation 2) [WAD Version 17]

Here's a video :3 [https://youtu.be/c9r79sQABYg](https://youtu.be/c9r79sQABYg)

### DELTARUNE Chapter Selector (GLFW) [WAD Version 17]



### Undertale 10th Anniversary (GLFW) [WAD Version 17]



### NXTALE (Undertale for Xbox One) (GLFW) [WAD Version 17]



### AM2R (GLFW) [WAD Version 14]



### GameMaker: Studio Platformer Demo (GLFW) [WAD Version 10]

