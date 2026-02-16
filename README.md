# Trinity Engine

Trinity Engine is a fork of [Quake3e](https://github.com/ec-/Quake3e) with features to support a VR-focused Quake III Arena ecosystem. It serves as the engine component alongside the [Trinity](https://github.com/ernie/trinity) game mod and [Trinity Tools](https://github.com/ernie/trinity-tools) server administration and statistics platform.

## Features beyond Quake3e

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

### Web Demo Player (Emscripten/WebAssembly)

The engine compiles to WebAssembly via Emscripten, enabling browser-based playback of `.tvd` demos
with no install required. Two build variants are available:

- `make web` — full web client that loads game assets from a configurable JSON manifest
- `make demoplayer` — locked-down demo player with minimal assets and preconfigured keybinds for spectating

The demo loader (`demo-loader.js`) is an ES module that handles fetching the engine, game assets,
and demo files into an Emscripten virtual filesystem. It parses TVD headers to determine the map
and `fs_game`, fetches the appropriate pk3s, and supports browser-side asset caching. It can be
integrated standalone or embedded into an existing page.

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcmake`/`emmake`).

### VR Client Support

The engine detects VR clients via userinfo and extends the network protocol to support 32-bit button states (up from 16), using this space to receive head orientation information so that it's reflected in-game. Servers advertise VR capability to clients via the `vr_support` serverinfo cvar.

### Server Lifecycle Callbacks

Game DLL callbacks for server startup and shutdown events, enabling integration with external tooling for statistics tracking and server management.

## The Trinity Ecosystem

**[Trinity](https://github.com/ernie/trinity)** — A unified Quake III Arena / Team Arena game mod featuring unlagged weapons, VR head and torso tracking, an orbital follow camera for spectating and demo playback, Quake Live-style damage indicators, and visual enhancements. This mod provides server-side support for VR clients (Q3VR or Quake 3 Quest) and attempts to replicate what features it can for flatscreen players.

**[Trinity Tools](https://github.com/ernie/trinity-tools)** — A real-time statistics tracking and server administration platform. Monitors multiple Quake 3 servers, tracks player performance and match history, provides leaderboards, and serves a web interface with live updates via WebSocket. Includes CLI tools for administration and game asset extraction.

**[Trinity Engine](https://github.com/ernie/trinity-engine)** — This project. Needed to run dedicated servers with Trinity, or to play back TrinityVision demos.

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
