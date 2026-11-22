# Independent camera-proxy first-person body protection — experiment v1

## Scope

This is an **opt-in independent experiment** in `experiment/fpb-native-fps-mask-v1`.
It does not call L4D2Visual, identify StudioModel, inspect entity/bone memory,
change game rendering, modify FSR SDK shaders or upload a full-screen CPU mask.

The 64-bit GF Host already recovers the engine's row-major world-to-clip
`cViewProj` and camera pose. It estimates a local-player origin as the camera
position **minus a tunable eye height**, then builds three upright 3D
bounding volumes for chest/abdomen and two legs. The volumes are shifted in
horizontal camera yaw (not pitch). Projected 2D rectangles are merged into
the existing exact-current-frame overlay image after the mask classifier.

This is **not** a measurement of the actual local-player entity position:
it uses a camera-derived proxy. FPB must be enabled separately by the user.
With FPB turned off, `fpb_projection_protect=1` could still protect some floor
pixels. Turn it off if no body is being rendered.

## Dynamic INI controls

All six new controls are read from
`bin/.l4d2bridge/fg-config.ini` and **hot-reload** approximately every
500 ms. Save the file while the game is running; no restart or shader
recompilation is required for these values:

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `fpb_projection_protect` | `0` | `0/1` | Opt-in proxy mask |
| `fpb_projection_forward` | `-13` | `[-48,48]` | Horizontal forward offset, Source units |
| `fpb_projection_right` | `-1` | `[-48,48]` | Horizontal right offset |
| `fpb_projection_up` | `-1` | `[-48,48]` | World-vertical offset |
| `fpb_projection_width` | `1.0` | `[0.5,2.5]` | Scales horizontal chest and leg widths |
| `fpb_projection_eye_height` | `64` | `[24,85]` | Assumed eye height above entity origin |

The original four dynamic-protection controls retain their semantics.
In particular `fpb_protect` (legacy residual preset) remains startup-only,
and **does not enable** the new proxy.

To isolate this experiment from broad residual false positives, start with:

```ini
dynamic_models=0
fpb_protect=0
fpb_projection_protect=1
fpb_projection_forward=-13
fpb_projection_right=-1
fpb_projection_up=-1
fpb_projection_width=1.0
fpb_projection_eye_height=64
mask_debug_white=1
```

`dynamic_models=0` is startup-only; restart to change it.
The six `fpb_projection_*` controls and `mask_debug_white` are hot-reloadable.
The source's `mask_debug_white=1` is a **combined mask**, not an FPB-only
source view. Toggle `fpb_projection_protect` in game to see the difference.

## GPU integration

At each accepted provider frame, the CPU projects up to three clipped 3D
volumes and produces 0..3 integer screen-space rectangles. These are not
uploaded as bitmaps. The Host's Vulkan command buffer first dispatches its
existing mask classification shader, then blits the corresponding rectangular
regions from the **current final color** to the existing RGBA8 provider
overlay. The Layer's existing exact-current composite handles the result.
The blit performs BGRA-to-RGBA conversion where necessary.

The source's **alpha channel is preserved** by this blit. Actual mask
coverage therefore depends on the game's backbuffer alpha in these
regions; this is a known unverified hardware prerequisite. Inspect
`mask_debug_white=1` before judging visual quality. If no FPB regions
become white, the alpha path needs a purpose-built mask-writing shader.
The blit requires Vulkan format `BLIT_SRC`/`BLIT_DST` support and
fails closed if those features are unavailable.

No extra Vulkan pipeline is rebuilt when offsets change, only a small
bounded number of blit regions are updated each accepted frame.

## Safety and diagnostics

- Near-plane intersections are computed before projection; coordinates are
  clipped to the viewport.
- If the proxy is entirely behind/off-screen, zero regions are produced.
  No old-screen rectangles are reused after looking up.
- If summed upper-bound area exceeds 70% of screen area, the frame's
  projection is rejected instead of protecting most of the screen.
- Only provider mode is supported; other FG modes are untouched.
- Unsupported camera matrices, invalid input or unsupported blit formats
  disable the projected overlay for the affected frame.
- `live_protection_updated` and `live_protection_rejected` log INI changes.
  `fpb_projection` samples report region count, conservative area estimate,
  coverage rejection and device blit support.
- No user-testing of image quality, occlusion or actual source alpha has yet
  been completed. Screen-space rectangles can protect background between
  body parts, causing local 60 FPS stepping.

## Recommended evaluation

1. Begin with `fpb_projection_protect=0`, FG on, and observe pistol-body
   overlap. Then set it to `1` without restarting.
2. With `mask_debug_white=1`, look straight ahead, down and up. The FPB-only
   difference should appear while looking down and disappear when looking up.
3. Adjust `forward/right/up` one axis at a time to align body; use
   `width` only if the shape is too narrow/wide.
4. Check whether pistol-hand/body overlap is sharp and whether the overlay
   incorrectly covers the floor, HUD or weapon.
5. If FPS or latency regress, compare with `fpb_projection_protect=0`.
