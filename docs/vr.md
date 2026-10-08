# VR setup and migration

Trinity Engine runs flatscreen and VR through the same client. VR uses the
Vulkan renderer and an OpenXR runtime with `XR_KHR_vulkan_enable2`. The GPU
must support Vulkan multiview with at least two views. OpenGL remains available
for flatscreen play.

## Runtime and launch

Install and select the OpenXR runtime supplied by your headset or streaming
software, then connect the headset through that software. Supported: Windows
x64 with the SteamVR, PICOXR and Virtual Desktop VDXR runtimes, and Linux arm64
on the Steam Frame; Linux x86_64 and macOS OpenXR are untested. On Linux, install a compatible OpenXR runtime and
Vulkan driver. The loader packaged with the engine dispatches to that runtime
and does not supply headset support by itself.

Use `trinity.exe +set vr_enabled 1` on Windows, or the packaged Linux executable
with `+set vr_enabled 1`. Use `+set vr_enabled 0` for flatscreen. The mode you
choose in the menu or at the console is saved, on every build. A `vr_enabled` or
`vr_mirrorEnabled` value given on the launch line (`+set`, `+seta`, `+setu` or
`+sets`) lasts the whole session, through video restarts and game directory
switches, but is not saved: the configuration keeps the value it had. If you
change the setting during that session, your change is saved. `set vr_enabled 1`
in `baseq3/autoexec.cfg` (or the selected game directory's `autoexec.cfg`) can
set your preferred startup mode. The Steam Frame package launches in VR with the
desktop mirror off by default, so a `+set vr_enabled 0` on the launch line plays
flat for one session. Its `vrpreferences.json` carries the SteamVR per-app
resolution, refresh rate and motion smoothing settings.

In the System menu, change **Display Mode** and select **Apply**. At the console,
use `set vr_enabled 1; vid_restart` or `set vr_enabled 0; vid_restart`. Entering
VR uses Vulkan without changing the saved flatscreen renderer. Switching while
connected keeps the connection. Servers without VR support receive the standard
command set, so VR controller buttons beyond the first sixteen are not sent to
them. The client widens commands from the server's `vr_support` serverinfo key
while the server reads the `vr` userinfo key, so each side derives the width from
its own configstring. `vr_status` reports the VR state, the requested and active
modes, and the most recent failure; `xr_info` reports the runtime, its API
version, the enabled extensions and the controller models found. A request can
wait for a headset; applying flatscreen cancels it.

## Staying in VR

While `vr_enabled` is `1`, the client stays in VR through map changes, game
directory switches (for example joining a Team Arena server from baseq3), video
restarts and errors. Only you change `vr_enabled`: the Display Mode setting, the
console, a game directory's own configuration, or Flatscreen in the dialog shown
when VR cannot start. `vr_status` names the state:

| State | Meaning |
| --- | --- |
| `FLAT` | Flatscreen was chosen, or repeated errors stopped VR for now. |
| `VR` | Running in the headset. |
| `VR_TRANSITION` | The renderer is rebuilding into VR, or VR resumes at the next match or the main menu. |
| `WAITING_HEADSET` | VR is wanted but no headset or runtime is available. The desktop shows the tracking prompt and VR starts when the headset returns. |
| `VR_UNSUPPORTED` | The current server's or mod's game modules cannot run in VR. |

When the runtime ends the session itself, by Exit game in its dashboard, by
quitting SteamVR, or by announcing that it is going away, the game quits.

Otherwise the client leaves VR in four cases:

- The server's or mod's game modules are proven VR-incompatible: a pure server
  whose cgame or UI QVM has no VR support, a mod other than baseq3 or
  missionpack without VR modules, or a bundled fallback that is missing or
  fails to start.
  The game continues in flatscreen. The same message names the incompatible pak
  either way: over the match once play starts, or in the main menu's error
  message when it happens at the menu. VR returns when you disconnect, connect to a server, or change mods,
  including from the Mods menu.
- The headset or runtime is unavailable, the session fails, or a probe fails.
  The client waits in `WAITING_HEADSET` and returns to VR when it is back.
- The machine cannot run VR: no OpenXR loader, no Vulkan binding in the
  runtime, or a runtime without `XR_KHR_vulkan_enable2`. The game continues in
  flatscreen with no polling, and the next launch or `vid_restart` tries again.
  When VR was on at launch, a failure dialog names the reason, and choosing
  Flatscreen there sets `vr_enabled` to `0`; enabling VR later in the session
  reports the reason in the console only.
- Three errors or headset losses within 30 seconds. The game continues in
  flatscreen, and an on-screen notice in a match, or the main menu's error
  message, says VR stopped; `vid_restart` or the next launch tries VR again.

While VR is active, game audio continues even if the desktop mirror is unfocused
or minimized. The focus-mute settings still apply in flatscreen mode. Removing
the headset does not automatically pause the game; open the menu manually.

## Game modules

The engine runs the cgame and UI QVM that ordinary pk3
priority selects, in VR and flatscreen alike, because a server's cgame must
match its game module. In VR, when that QVM has no supported VR API marker,
non-pure baseq3 and missionpack use the bundled native modules. This fallback is
limited to those two games. When the bundled modules replace a QVM, an on-screen
notice at the start of each connection's play names that pak (or game
directory) for a few seconds, and `vr_status` lists it.

Pure servers prohibit native fallback. A pure server whose cgame or UI QVM has
no VR support keeps the client in flatscreen for that connection
(`VR_UNSUPPORTED`) and reports the reason.
Do not disable server purity to work around a mismatched release: install the
matching mod assets. Retail Quake III / Team Arena data is still required as
described in the [main README](../README.md#game-data).

## Mirror and controls

The VR mirror has its own saved window settings. Apply latched mirror changes
with `vid_restart`.

| Setting | Meaning |
| --- | --- |
| `vr_mirrorEnabled` | `0` disables mirror content, `1` enables it |
| `vr_mirrorFullscreen` | `0` windowed, `1` fullscreen |
| `vr_mirrorWidth`, `vr_mirrorHeight` | Windowed mirror size; defaults 1280 by 720 |
| `vr_desktopContentType` | `0` left eye, `1` right eye, `2` both |
| `vr_desktopContentFit` | `0` contain, `1` fill/crop |
| `vr_desktopMenuStyle` | `0` desktop menu view, `1` VR view |
| `vr_screenCurvature` | Virtual screen curvature; `0` is flat, `0.5` is the default curve, `1` is the tightest |
| `vr_controllerModels` | `1` draws the menu pointer in the headset while the virtual screen is up: a ray from the pointing hand (blue from the off hand while the keyboard is up), a pool of light where it meets the screen, and the runtime's own controller models on runtimes that provide them. `0` goes back to the menus' own cursor with no ray and no controllers. The menus' HUD & Display page has it as "Virtual screen controllers" |
| `vr_sensitivity` | Smooth thumbstick turning speed; `100` is normal, independent of mouse `sensitivity` |
| `vr_snapturn` | Positive values select the snap-turn angle in degrees, with `1` meaning 45; nonpositive values enable smooth turning |

### Virtual keyboard

The console, the chat line and menu text fields bring up an on-screen keyboard
on the virtual screen. It is a 65% PC layout: the main block, a column of
Home, End, Page Up and Page Down, and arrow keys. Shift is one-shot, so it
applies to the next character and clears; Caps Lock latches and affects
letters only, and Shift under it gives lowercase. Either hand types; the
pointing hand's pool of light hovers a key, the trigger presses it, and a held
trigger repeats. Crossing onto a key gives a light haptic tick. Dismiss it with Escape, the
controller's Menu button, or a click outside the keyboard; Enter closes it in
a menu field and keeps it open in the console. The font and key art come from
the Trinity paks; an older pak gives the same keyboard in a plain style.

### Rendering path

`r_fbo` picks how the scene reaches the headset.

| Value | Behavior |
| --- | --- |
| `1` | Scene renders to an offscreen framebuffer, then a post pass writes the eye images. Bloom, `r_hdr`, HDR mirror output, `r_greyscale`, `r_dither`, `r_presentBits`, screenshots, and video capture are available. |
| `0` | Scene renders straight into the headset swapchain image. Overbright is off (`identityLight` 1.0, so lightmapped areas read flatter and multi-stage blends look different); bloom, `r_hdr`, HDR mirror output, `r_greyscale`, `r_dither`, and `r_presentBits` are unavailable. Screenshots and video capture are refused, pointing at `r_fbo 1`. `r_gamma` is baked into textures at load, so it takes a `vid_restart`. `r_ext_multisample` still works, resolving into the swapchain, and the desktop mirror shows the headset image unprocessed. |

Switching between the two changes the image, not just performance. If the runtime's swapchain format has no sRGB encoding, the renderer falls back to the framebuffer path for that session and logs a warning.

Supersampling, refresh rate, HUD, comfort, and foveation settings remain in the
Trinity mod's VR menus. Available refresh rates and eye-tracked foveation depend on
the runtime and device; a requested value alone does not prove it was applied.

Eye-tracked foveation centers its sharp region where the runtime's own foveation
puts it (`XR_META_foveation_eye_tracked`) when the runtime offers that. For now only
standalone headset runtimes do, such as SteamVR on the Steam Frame; SteamVR on
Windows does not, so PCVR follows the eye gaze pose (`XR_EXT_eye_gaze_interaction`)
instead. The engine carries that pose through the head's turn since the eye was
sampled, which keeps the sharp region on a point the eyes hold in the world; with
the eyes riding along with the head, as when reading the HUD mid-turn, it lands a
little behind the gaze. `xr_info` reports which source is in use.

## Moving from Trinity VR

Keep the existing installation and configuration as a backup. Install the unified
engine with its matching renderer, loader, native modules, and Trinity paks;
do not mix renderer DLLs between the two engines. Start with a separate
`+set fs_homepath <directory>` if you want to compare configurations safely.

Copy selected bindings and comfort preferences rather than replacing the whole
new configuration. Set `vr_enabled` explicitly and tune `vr_sensitivity`
separately from mouse sensitivity. The mirror settings are `vr_mirrorEnabled`,
`vr_mirrorFullscreen`, `vr_mirrorWidth`, and `vr_mirrorHeight`; a Trinity VR
configuration kept them as `vr_desktopMode`, `r_fullscreen`, and
`r_customdesktopwidth` / `r_customdesktopheight`, and those names are not read
here, so carry the values over by hand.
