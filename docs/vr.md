# VR setup and migration

Trinity Engine runs flatscreen and VR through the same client. VR uses the
Vulkan renderer and an OpenXR runtime with `XR_KHR_vulkan_enable2`. The GPU
must support Vulkan multiview with at least two views. OpenGL remains available
for flatscreen play.

## Runtime and launch

Install and select the OpenXR runtime supplied by your headset or streaming
software, then connect the headset through that software. Supported: Windows
x64 with PICOXR and Virtual Desktop VDXR runtimes. Linux and macOS OpenXR are
available but untested. On Linux, install a compatible OpenXR runtime and
Vulkan driver. The loader packaged with the engine dispatches to that runtime
and does not supply headset support by itself.

Use `trinity.exe +set vr_enabled 1` on Windows, or the packaged Linux executable
with `+set vr_enabled 1`. Use `+set vr_enabled 0` for flatscreen. The preference
is archived; `set vr_enabled 1` in `baseq3/autoexec.cfg` (or the selected game
directory's `autoexec.cfg`) can set your preferred startup mode. Command-line
`+set` values override the configuration at startup.

In the System menu, change **Display Mode** and select **Apply**. At the console,
use `set vr_enabled 1; vid_restart` or `set vr_enabled 0; vid_restart`. Entering
VR uses Vulkan without changing the saved flatscreen renderer. Switching while
connected keeps the connection. Servers without VR support receive the standard
command set, so VR controller buttons beyond the first sixteen are not sent to
them. The client widens commands from the server's `vr_support` serverinfo key
while the server reads the `vr` userinfo key, so each side derives the width from
its own configstring. `vr_status` reports requested and active modes and the most
recent failure. A request can wait for a headset; applying flatscreen cancels it.

While VR is active, game audio continues even if the desktop mirror is unfocused
or minimized. The focus-mute settings still apply in flatscreen mode. Removing
the headset does not automatically pause the game; open the menu manually.

## Game modules

Keep the matching Trinity mod paks and any packaged `trinity-native` directory
with the release. The engine loads cgame and UI at runtime through their VR
interface. Compatible QVMs can run in VR. In non-pure baseq3 or missionpack,
bundled native modules can replace incompatible modules. This fallback is not
a general compatibility layer for other mods.

Pure servers require compatible cgame and UI QVMs; native fallback is prohibited.
An ineligible VR request leaves the client in flatscreen and reports the reason.
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
| `vr_sensitivity` | Smooth thumbstick turning speed; `100` is normal, independent of mouse `sensitivity` |
| `vr_snapturn` | Positive values select the snap-turn angle in degrees, with `1` meaning 45; nonpositive values enable smooth turning |

### Rendering path

`r_fbo` picks how the scene reaches the headset.

| Value | Behavior |
| --- | --- |
| `1` | Scene renders to an offscreen framebuffer, then a post pass writes the eye images. Bloom, `r_hdr`, HDR mirror output, `r_greyscale`, `r_dither`, `r_presentBits`, screenshots, and video capture are available. |
| `0` | Scene renders straight into the headset swapchain image. Overbright is off (`identityLight` 1.0, so lightmapped areas read flatter and multi-stage blends look different); bloom, `r_hdr`, HDR mirror output, `r_greyscale`, `r_dither`, and `r_presentBits` are unavailable. Screenshots and video capture are refused, pointing at `r_fbo 1`. `r_gamma` is baked into textures at load, so it takes a `vid_restart`. `r_ext_multisample` still works, resolving into the swapchain, and the desktop mirror shows the headset image unprocessed. |

Switching between the two changes the image, not just performance. If the runtime's swapchain format has no sRGB encoding, the renderer falls back to the framebuffer path for that session and logs a warning.

Supersampling, refresh rate, HUD, comfort, and foveation settings remain in the
Trinity VR menus. Available refresh rates and eye-tracked foveation depend on
the runtime and device; a requested value alone does not prove it was applied.

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
