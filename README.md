# Trinity Engine

Trinity Engine is a fork of [Quake3e](https://github.com/ec-/Quake3e) with features to support a VR-focused Quake III Arena ecosystem. It serves as the engine component alongside the [Trinity](https://github.com/ernie/trinity) game mod and [Trinity Tracker](https://github.com/ernie/trinity-tracker) server administration and statistics platform.

## Features beyond Quake3e

### VR Client Support

The engine supports VR clients connecting to flatscreen servers. This involves changes
on both the server and client sides of the engine:

**Server engine** — Detects VR clients via the `vr` userinfo key and reads 32-bit usercmd
buttons (instead of 16) for those clients, allowing head orientation data packed into the
upper bits to pass through to the game mod. Sets `vr_support=1` in serverinfo so VR clients
know they can send extended data.

**Client engine** — Reads `vr_support` from serverinfo on connect. Flatscreen clients use
this to display VR player status (e.g., scoreboard icons via configstrings). The VR client
engines (Q3VR, Quake 3 Quest) use it to decide whether to pack head orientation into usercmds
and write 32-bit buttons.

The actual head tracking encoding, game logic, and rendering are handled by the
[Trinity](https://github.com/ernie/trinity) game mod and the VR client engines. See
[VR_PROTOCOL.md](https://github.com/ernie/trinity/blob/main/docs/VR_PROTOCOL.md) for the
full protocol specification.

### Improved Stencil Shadows

Stencil shadow volumes (`cg_shadows 2`) have been substantially reworked in both the OpenGL
and Vulkan renderers:

- **Shadow volume capping** — Back caps are generated for shadow volumes, fixing the "ghost
  shadow" artifacts that appeared as floating model silhouettes when players jumped
- **Configurable distance** — `r_shadowDistance` (default 128) replaces the hardcoded 512-unit
  extrusion, greatly reducing wall bleed
- **Raw light direction** — Shadow extrusion follows the actual light direction instead of
  clamping to downward travel, producing correct shadows from all light angles
- **Draw order** — Shadow compositing happens before blended surfaces, so sprites and
  transparent effects draw on top of shadows as expected
- **Entity stencil masking** — Entity pixels are marked in the stencil buffer so shadows
  don't darken the casting model itself
- **Expanded entity culling** — Prevents shadows disappearing the moment the entity casting
  them goes offscreen.
- **BSP clipping** — Avoids most cases of wall/floor bleed, when a shadow is fully "caught"
  by a surface, via per-vertex clipping to the BSP. You'll still see some bleeding and
  artifacts if the shadow spills over a staircase or through a gap.

To use: `cg_shadows 2`, `r_shadowDistance 512`, `r_shadowClip 1`. The latter two values
are the defaults.

### TV (TrinityVision, of course :wink:) Demo System

Server-side demo recording captures complete match state — all entities, player states,
and server commands — into a compact, zstd-compressed, delta-encoded `.tvd` format.
Client-side playback supports multiple viewpoints, smooth interpolation between snapshots,
and seeking via replay from the beginning.

- `tvrecord` / `tvstop` — manual recording control
- `sv_tvAuto` — automatic recording on map load
- `sv_tvAutoMinPlayers` — minimum concurrent non-spectator human players to keep auto-recording (0 = always keep)
- `sv_tvAutoMinPlayersSecs` — seconds the threshold must be continuously met (0 = instantaneous)
- `sv_tvDownload` — notify clients to download the completed demo via HTTP at map change (requires `sv_dlURL`)
- `cl_tvDownload` — opt in to automatic TV demo downloads from the server
- Client-side viewpoint switching and seek during playback
    - 0 = No download, ever
    - 1 = Offer download. Cgame-controlled handling. For Trinity, show a dialog. 1 = default to decline after `cg_tvdTimeout` seconds
    - 2 = Offer download. Cgame-controlled handling. For Trinity, show a dialog. 2 = default to accept after `cg_tvdTimeout` seconds

### Web Demo Player (Emscripten/WebAssembly)

The engine compiles to WebAssembly via Emscripten, enabling browser-based playback of `.tvd` demos
with no install required:

- `make web` — web engine that loads game assets from a configurable JSON manifest

The loader (`loader.js`) is an ES module that handles fetching the engine, game assets,
and demo files into an Emscripten virtual filesystem. It parses TVD headers to determine the map
and `fs_game`, fetches the appropriate pk3s, and supports browser-side asset caching. It can be
used as a web client or demo player, and embedded into an existing page.

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcmake`/`emmake`).

### Server Lifecycle Callbacks

Game DLL callbacks for server startup and shutdown events, enabling integration with external tooling for statistics tracking and server management.

## macOS

Downloaded builds will be quarantined by Gatekeeper. Before launching for the first time, remove the quarantine attribute:

```bash
xattr -cr Trinity.app
```

## The Trinity Ecosystem

**[Trinity](https://github.com/ernie/trinity)** — A unified Quake III Arena / Team Arena game mod featuring unlagged weapons, VR head and torso tracking, an orbital follow camera for spectating and demo playback, Quake Live-style damage indicators, and visual enhancements. This mod provides server-side support for VR clients (Q3VR or Quake 3 Quest) and attempts to replicate what features it can for flatscreen players.

**[Trinity Engine](https://github.com/ernie/trinity-engine)** — This project. The flatscreen engine, forked from Quake3e. Needed to run dedicated servers with Trinity, or to play back or auto-download TrinityVision demos. Loads the Trinity mod's game modules (cgame, game, ui) as QVM files at runtime.

**[Q3VR](https://github.com/ernie/q3vr)** — PCVR client for Windows (OpenXR/SteamVR). Based on ioquake3 + ioq3quest VR with Trinity features compiled in. Supports full 6DoF single-player and multiplayer with crossplay between PC and Quest, a virtual screen for 2D content, haptic feedback, a weapon wheel, and configurable comfort options.

**[Quake 3 Quest](https://github.com/nicomigu/ioq3quest)** — Meta Quest standalone VR client. Based on ioq3quest with Trinity features compiled in. Runs natively on Meta Quest headsets with full VR support and crossplay with Q3VR and flatscreen players.

**[Trinity Tracker](https://github.com/ernie/trinity-tracker)** — A real-time statistics tracking and server administration platform. Monitors multiple Quake 3 servers, tracks player performance and match history, provides leaderboards, and serves a web interface with live updates via WebSocket. Includes CLI tools for server management and game asset extraction.

> **Note:** The VR clients (Q3VR and Quake 3 Quest) compile Trinity mod code directly into their binaries because VR-specific function implementations would be replaced by flatscreen QVMs. The flatscreen engine loads QVMs at runtime instead.

---

# Quake3e

_The following is the original Quake3e README, preserved for reference._

[![build](../../workflows/build/badge.svg)](../../actions?query=workflow%3Abuild) \* <a href="https://discord.com/invite/X3Exs4C"><img src="https://img.shields.io/discord/314456230649135105?color=7289da&logo=discord&logoColor=white" alt="Discord server" /></a>

This is a modern Quake III Arena engine aimed to be fast, secure and compatible with all existing Q3A mods.
It is based on last non-SDL source dump of [ioquake3](https://github.com/ioquake/ioq3) with latest upstream fixes applied.

Go to [Releases](../../releases) section to download latest binaries for your platform or follow [Build Instructions](#build-instructions)

_This repository does not contain any game content so in order to play you must copy the resulting binaries into your existing Quake III Arena installation_

**Key features**:

- optimized OpenGL renderer
- optimized Vulkan renderer
- raw mouse input support, enabled automatically instead of DirectInput(**\in_mouse 1**) if available
- unlagged mouse events processing, can be reverted by setting **\in_lagged 1**
- **\in_minimize** - hotkey for minimize/restore main window (win32-only, direct replacement for Q3Minimizer)
- **\video-pipe** - to use external ffmpeg binary as an encoder for better quality and smaller output files
- significally reworked QVM (Quake Virtual Machine)
- improved server-side DoS protection, much reduced memory usage
- raised filesystem limits (up to 20,000 maps can be handled in a single directory)
- reworked Zone memory allocator, no more out-of-memory errors
- non-intrusive support for SDL2 backend (video, audio, input), selectable at compile time
- tons of bug fixes and other improvements

## Vulkan renderer

Based on [Quake-III-Arena-Kenny-Edition](https://github.com/kennyalive/Quake-III-Arena-Kenny-Edition) with many additions:

- high-quality per-pixel dynamic lighting
- very fast flares (**\r_flares 1**)
- anisotropic filtering (**\r_ext_texture_filter_anisotropic**)
- greatly reduced API overhead (call/dispatch ratio)
- flexible vertex buffer memory management to allow loading huge maps
- multiple command buffers to reduce processing bottlenecks
- [reversed depth buffer](https://developer.nvidia.com/content/depth-precision-visualized) to eliminate z-fighting on big maps
- merged lightmaps (atlases)
- multitexturing optimizations
- static world surfaces cached in VBO (**\r_vbo 1**)
- useful debug markers for tools like [RenderDoc](https://renderdoc.org/)
- fixed framebuffer corruption on some Intel iGPUs
- offscreen rendering, enabled with **\r_fbo 1**, all following requires it enabled:
- `screenMap` texture rendering - to create realistic environment reflections
- multisample anti-aliasing (**\r_ext_multisample**)
- supersample anti-aliasing (**\r_ext_supersample**)
- per-window gamma-correction which is important for screen-capture tools like OBS
- you can minimize game window any time during **\video**|**\video-pipe** recording
- high dynamic range render targets (**\r_hdr 1**) to avoid color banding
- bloom post-processing effect
- arbitrary resolution rendering
- greyscale mode

In general, not counting offscreen rendering features you might expect from 10% to 200%+ FPS increase comparing to KE's original version

Highly recommended to use on modern systems

## OpenGL renderer

Based on classic OpenGL renderers from [idq3](https://github.com/id-Software/Quake-III-Arena)/[ioquake3](https://github.com/ioquake/ioq3)/[cnq3](https://bitbucket.org/CPMADevs/cnq3)/[openarena](https://github.com/OpenArena/engine), features:

- OpenGL 1.1 compatible, uses features from newer versions whenever available
- high-quality per-pixel dynamic lighting, can be triggered by **\r_dlightMode** cvar
- merged lightmaps (atlases)
- static world surfaces cached in VBO (**\r_vbo 1**)
- all set of offscreen rendering features mentioned in Vulkan renderer, plus:
- bloom reflection post-processing effect

Performance is usually greater or equal to other opengl1 renderers

## OpenGL2 renderer

Original ioquake3 renderer, performance is very poor on non-nvidia systems, unmaintained

## [Build Instructions](BUILD.md)

## Contacts

Discord channel: <https://discordapp.com/invite/X3Exs4C>

## Links

- <https://bitbucket.org/CPMADevs/cnq3>
- <https://github.com/ioquake/ioq3>
- <https://github.com/kennyalive/Quake-III-Arena-Kenny-Edition>
- <https://github.com/OpenArena/engine>

