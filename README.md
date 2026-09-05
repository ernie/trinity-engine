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
engines (Trinity VR, Trinity Quest) use it to decide whether to pack head orientation into usercmds
and write 32-bit buttons.

The actual head tracking encoding, game logic, and rendering are handled by the
[Trinity](https://github.com/ernie/trinity) game mod and the VR client engines. See
[VR_PROTOCOL.md](https://github.com/ernie/trinity/blob/main/docs/VR_PROTOCOL.md) for the
full protocol specification.

### Improved Stencil Shadows

Stencil shadow volumes (`cg_shadows 2`) are reworked across all three renderers — OpenGL,
Vulkan, and OpenGL2 (the renderer the WebAssembly demo player uses) — built on a z-fail
("Carmack's reverse") approach so shadows stay correct even when the camera is inside a
shadow volume:

- **Sealed silhouettes** — Model edges are welded by position so the silhouette forms
  closed loops, sealing the light leaks and cracks that plain edge-matching left on
  Quake III's models.
- **Capped volumes** — Volumes are closed at both ends, eliminating the "ghost shadow"
  silhouettes that floated in mid-air when players jumped.
- **Configurable distance** — `r_shadowDistance` (default `256`) sets the extrusion and cull
  distance, replacing the old hardcoded 512-unit extrusion and greatly reducing wall bleed.
- **Raw light direction** — Extrusion follows the actual light direction instead of clamping
  to straight down, producing correct shadows from all light angles.
- **Expanded entity culling** — Shadows don't disappear the moment their caster leaves the
  screen.
- **BSP clipping** — `r_shadowClip 1` clips shadow volumes against world geometry to curb
  floor/wall bleed; `r_shadowClipPenetration` (default `4`) and `r_shadowClipExtension`
  (default `16`) tune how far a clipped volume may pass through and extend past a surface.
  Some bleed can still occur where a shadow spills across stairs or through a gap.

The defaults are tuned for normal play; just set `cg_shadows 2`.

### HDR Display Output

On the Vulkan renderer, Trinity Engine can output true HDR (scRGB linear FP16) to an
HDR display, for brighter highlights and more lifelike color than standard dynamic range.
It requires the Vulkan renderer, `r_fbo 1`, and an HDR-capable display with HDR enabled in
the OS. This is separate from `r_hdr`, which only sets internal framebuffer precision.
Toggle it in-game under **Setup → Graphics → HDR Display** and tune it under
**Setup → Display → HDR Calibration**, or set the cvars directly:

- `r_hdrDisplay` (default `0`) — master toggle; `1` enables true HDR output. Latched
  (takes effect after `vid_restart`).
- `r_hdrPeak` (default `400`) — your display's usable peak brightness in nits, the main
  value to match to your panel. Best found with the calibration screen, which reflects the
  brightness the panel actually reaches (often well below its rated peak). Range `250`–`10000`;
  the in-game calibration slider covers `250`–`2000`.
- `r_hdrPaperWhite` (default `0`) — nits that normal (SDR) white maps to; `0` = auto from the
  peak.
- `r_hdrHighlight` (default `1.0`, range `0.5`–`4.0`) — extra emphasis on the brightest
  highlights; does not change overall brightness.
- `r_hdrSaturation` (default `0.0`, range `0`–`1`) — how far the brightest highlights bleed
  toward white (`1` keeps full color, `0` lets them go fully white).
- `r_hdrSaturationFull` (default `1.5`, range `0.5`–`3.0`) — emitter intensity at which that
  bleed reaches its maximum.
- `r_hdrSoftKnee` (default `1.0`, range `0`–`2`) — how gradually the highlight treatment eases
  in above paper-white.
- `r_hdrActive` (read-only) — `1` when HDR output is genuinely live (HDR swapchain up and the
  display's HDR switch on).

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
    - 0 = No download, ever
    - 1 = Offer download. Cgame-controlled handling. For Trinity, show a dialog. 1 = default to decline after `cg_tvdTimeout` seconds
    - 2 = Offer download. Cgame-controlled handling. For Trinity, show a dialog. 2 = default to accept after `cg_tvdTimeout` seconds
- Client-side viewpoint switching and seek during playback

#### Live streaming

The same TV capture can be broadcast while the match is being played.
With `sv_tvLive 1` the dedicated server offers the in-progress match as
a `TVL1` byte stream on a loopback TCP socket (same port number as the
UDP game port); a consumer such as the
[Trinity tracker](https://github.com/ernie/trinity-tracker) relays it to
browser viewers, where this engine's WASM build plays it live (`cl_tv`).
The stream survives map changes on one connection — each map session
ends with an end marker and the next starts with a fresh header.

- `sv_tvLive` — offer the live TVL1 stream tap (default `0`)
- `sv_tvLiveKeyframeMsec` — keyframe cadence for the live stream, in ms (default `1000`); the relay derives its viewer holdback from it
- `tv_resync` — (WASM build) command the embedding page runs on tab foreground, so a throttled tab's backlog snaps back to the playback cushion instead of replaying

### Web Demo Player (Emscripten/WebAssembly)

The engine compiles to WebAssembly via Emscripten, enabling browser-based playback of `.tvd` demos
with no install required:

- `make web` — web engine that loads game assets from a configurable JSON manifest

The loader (`loader.js`) is an ES module that handles fetching the engine, game assets,
and demo files into an Emscripten virtual filesystem. It parses TVD headers to determine the map
and `fs_game`, fetches the appropriate pk3s, and supports browser-side asset caching. It can be
used as a web client or demo player, and embedded into an existing page.

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcmake`/`emmake`).

### Bot Grapple Navigation

Botlib can fly a grapple along its routes. It reads Trinity navigation data (`maps/<map>.aat`) beside the map's own `.aas`, falling back to stock routing on maps that have none, while the game module decides when a bot shoots, bites, and lets go. `bot_grapple` defaults to `1`; `2` logs every route shot, bite, and tow ending. Navigation reads never mark a pak referenced, so clients are never asked to download server-only data.

### Server Lifecycle Callbacks

Game DLL callbacks for server startup and shutdown events, enabling integration with external tooling for statistics tracking and server management.

### Dedicated-Server Operations

- `sv_conTap` — with `1`, stream the server console over a loopback TCP socket on a kernel-assigned port, advertised read-only in serverinfo as `sv_conPort` (`0` = no tap offered). The Trinity tracker consumes this for `trinity console`.
- `com_writeConfig` — with `0`, never write archived cvars back to `q3config.cfg` (`q3config_server.cfg` on dedicated servers). Disable on multi-instance dedicated hosts that share a homepath, where the write-back cross-contaminates instances.
- `sv_fps` defaults to `40` and client `rate` to `50000` — the values the Trinity mod is tuned against.

## Game Data

Trinity Engine binaries do not ship with Quake III Arena game content. To play, drop the engine alongside an existing Quake III Arena installation, or assemble the required pk3s yourself:

- **Original game data** — `pak0.pk3` from your retail Quake III Arena CD (and the Team Arena `pak0.pk3` if you want to play missionpack maps). Place in `baseq3/` and `missionpack/` respectively.
- **Quake III Arena 1.32 point release** — `pak1.pk3` through `pak8.pk3` for `baseq3/`, plus `pak1.pk3` through `pak3.pk3` for `missionpack/`. If you don't already have these, the ioquake3 project hosts the patch data here: <https://ioquake3.org/extras/patch-data/>.
- **Trinity mod pk3s** — `pak8t.pk3` (baseq3) and `pak3t.pk3` (missionpack) ship with each release. Run the bundled `update-trinity` script to refresh them between Trinity Engine releases.

Earlier Trinity Engine releases bundled the point release pk3s directly. They have been removed to keep download sizes small for the common case where players already have a Quake III installation.

## macOS

Downloaded builds will be quarantined by Gatekeeper. Before launching for the first time, remove the quarantine attribute:

```bash
xattr -cr Trinity.app
```

## The Trinity Ecosystem

**[Trinity](https://github.com/ernie/trinity)** — A unified Quake III Arena / Team Arena game mod featuring unlagged weapons, VR head and torso tracking, an orbital follow camera for spectating and demo playback, Quake Live-style damage indicators, and visual enhancements. This mod provides server-side support for VR clients (Trinity VR or Trinity Quest) and attempts to replicate what features it can for flatscreen players.

**[Trinity Engine](https://github.com/ernie/trinity-engine)** — This project. The flatscreen engine, forked from Quake3e. Needed to run dedicated servers with Trinity, or to play back or auto-download TrinityVision demos. Loads the Trinity mod's game modules (cgame, game, ui) as QVM files at runtime.

**[Trinity VR](https://github.com/ernie/trinity-vr)** — PCVR client for Windows (OpenXR/SteamVR). Based on ioquake3 + ioq3quest VR with Trinity features compiled in. Supports full 6DoF single-player and multiplayer with crossplay between PC and Quest, a virtual screen for 2D content, haptic feedback, a weapon wheel, and configurable comfort options.

**[Trinity Quest](https://github.com/ernie/trinity-quest)** — Meta Quest standalone VR client. Based on ioq3quest with Trinity features compiled in. Runs natively on Meta Quest headsets with full VR support and crossplay with Trinity VR and flatscreen players.

**[Trinity Tracker](https://github.com/ernie/trinity-tracker)** — A real-time statistics tracking and server administration platform. Monitors multiple Quake 3 servers, tracks player performance and match history, provides leaderboards, and serves a web interface with live updates via WebSocket. Includes CLI tools for server management and game asset extraction.

> **Note:** The VR clients (Trinity VR and Trinity Quest) compile Trinity mod code directly into their binaries because VR-specific function implementations would be replaced by flatscreen QVMs. The flatscreen engine loads QVMs at runtime instead.

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

