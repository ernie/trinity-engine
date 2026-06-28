# HDR Display Output (Vulkan renderer)

Real HDR output to an HDR display: the engine emits an scRGB FP16 HDR swapchain
instead of letting the OS map SDR content. Vulkan renderer only (see
Limitations). Enable with `r_hdrDisplay 1` then `\vid_restart`, on a display
with HDR turned on in the OS.

## Cvars

| Cvar | Default | Range | Meaning |
|------|---------|-------|---------|
| `r_hdrDisplay` | `0` | 0-1 | Master on/off. Latched; needs `vid_restart`. |
| `r_hdrPeak` | `1000` | 250-10000 | Display peak brightness in nits. The setting to match to your panel. |
| `r_hdrPaperWhite` | `0` | 0-1000 | Nits that "SDR white" maps to. `0` = auto (derived from peak). |
| `r_hdrHighlight` | `1.0` | 0.5-4.0 | Extra push for the brightest true highlights. `1` = natural. |

`r_hdrPeak`, `r_hdrPaperWhite`, and `r_hdrHighlight` apply live (no restart).

## How it works

Q3 renders in gamma (sRGB-encoded) space, clamped to [0,1], and applies
"overbright" (a brightness multiply) in the final gamma pass. There is no
linear-light or HDR scene buffer; overbright is the only signal that can exceed
SDR white. The HDR path reuses that final pass (`code/renderervk/shaders/gamma.frag`):

1. Apply overbright in gamma space, exactly as the SDR path does, then convert
   to linear with the sRGB EOTF. The level SDR would clip to white now sits at
   `r_hdrPaperWhite` nits, and overbright extends above it as real highlight
   headroom.
2. Roll that headroom off toward `r_hdrPeak` with a hue-preserving curve: all
   three channels scale by one factor taken from the brightest channel, so
   chromaticity is preserved (no per-channel hue shift) and highlights soften
   toward the panel peak instead of hard-clipping.
3. Output linear scRGB (Rec.709 primaries, 1.0 = 80 nits) to the FP16 swapchain
   in `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`.

The internal color buffer stays `R16G16B16A16_UNORM`, clamped to [0,1], because
Q3's destination-dependent blend modes (such as `GL_ONE_MINUS_DST_COLOR`, used by
glow/filter stages) require a clamped framebuffer. An unclamped float buffer lets
bright additive overlays (the quad-damage shell is the clearest case) push the
destination above 1.0, which drives those blends negative and renders the surface
black. A side effect of clamping is that additive highlights (bloom) saturate at
SDR white before the encode, so the brightest pixels reach one HDR level rather
than spanning toward the panel peak. Extending that range would require
compositing bright content in a separate layer so the scene buffer stays clamped.

Auto paper-white (`r_hdrPaperWhite 0`) uses the BT.2408 HLG reference white for
the chosen peak: `peak * 0.264964^(1.2 + 0.42*log10(peak/1000))`, giving ~203
nits at 1000, ~248 at 1300, ~344 at 2000. This holds the midtone/highlight
balance constant as panels get brighter.

On Windows the engine queries the OS HDR switch (DisplayConfig advanced color);
if HDR output is requested while the switch is off, it stays SDR and logs a
warning rather than emitting an oversaturated image.

## Emissive highlight layer

When `r_hdrDisplay 1` is active, the renderer adds a second MRT attachment
alongside the scene color buffer. The attachment is `R16G16B16A16_SFLOAT`
(FP16), so it can record values above 1.0 without clamping.

Only additive 3D scene draws write to this layer. The push constant
`emissiveFactor` is set to the draw's overbright value for additive blends in the
3D pass (`TYPE_GENERIC_BEGIN` program class and above); it is zero for opaque
draws, sky, decals, and all 2D draws. HUD, menus, and the crosshair never write
the emissive layer.

At the end of the frame the gamma pass reconstructs highlights via a per-channel
saturation gate:

```glsl
vec3 recon = mix(base, max(base, emissive), step(0.999, base));
```

`step(0.999, base)` fires only for channels that have saturated to the top of the
UNORM range. When it fires, `max(base, emissive)` replaces the saturated channel
with its FP16 value, recovering the true emitter brightness above 1.0. Unsaturated
channels pass through unchanged. The recovered value feeds the HDR tone-mapping
curve and can push toward `r_hdrPeak`.

**Why 2D occludes correctly:** 2D draws happen after the 3D pass and write
directly into the UNORM scene buffer. They overwrite any saturated 3D pixel with
their own value (below 1.0), so `step(0.999, base)` returns 0 and the emissive
layer is ignored at that pixel. No special compositing is needed.

**HDR bloom is out of scope.** The dominant gain is source brightness recovery:
emitters that were flat-clipped at SDR white now resolve to their true brightness,
which the display renders as a visibly brighter point. The halo around that source
is necessarily dimmer. Furthermore, the `max()` in the reconstruction can only
raise a saturated channel, never cancel or darken it, so a bloom kernel applied
to the emissive values cannot undo an emitter's contribution. Bloom is not
planned for this layer.

**Known upgrade path:** the saturation gate relies on 2D being drawn after 3D. If
a confirmed edge case shows a 2D draw failing to occlude a highlight, the correct
fix is to split emissive capture into a separate 2D layer or to formalize the
3D-to-2D pass boundary so the two paths cannot alias the emissive output. This is
tracked but not built until the edge case is confirmed in testing.

## Setup

- Turn on HDR in the OS display settings first.
- Set `r_hdrPeak` to your panel's rated peak for small highlights. This is
  higher than its full-screen brightness, and is not the same as a
  "DisplayHDR 400/600" badge (those are lower).
- Leave `r_hdrPaperWhite` on auto; raise it only if the image feels dim in a
  bright room.
- Tune `r_hdrHighlight` to taste while watching the scene.

Keep `r_hdrPeak` matched to the real display peak. Setting it higher makes the
rolloff cap above what the panel can show, so the OS compresses on top of our
curve (mild double tone-mapping).

## Design rationale

- Overbright is applied in gamma space (matching SDR), then linearized. This
  reproduces the Q3 look with extended highlights. Scaling in linear light
  would be more physically correct but would shift the midtones away from the
  SDR appearance, and Q3's overbright is itself a gamma-space construct.
- Tone-mapping is intentionally minimal (a soft rolloff, not a film curve such
  as ACES) to stay faithful to the source rather than restyle it.
- No gamut conversion: Q3 content is Rec.709 and scRGB is Rec.709.

## Limitations

- Vulkan renderer only. The OpenGL renderers cannot output HDR with the current
  SDL2 platform layer (the GL backbuffer is 8-bit; unblocking it needs SDL3 or
  a native float swapchain).
- `r_hdr` is internal buffer precision, unrelated to `r_hdrDisplay`.
- HDR10/PQ output is not implemented. For Rec.709 Q3 content, scRGB delivers the
  same picture at higher precision, so PQ would be a lateral change.
