# Independent camera-proxy first-person body protection — experiment v1

## Scope

This is an **opt-in independent experiment** in `experiment/fpb-native-fps-mask-v1`.
It does not call L4D2Visual, identify StudioModel, inspect entity/bone memory,
change game rendering, modify FSR SDK shaders or upload a full-screen CPU mask.

The 64-bit GF Host already recovers the engine's row-major world-to-clip
`cViewProj` and camera pose. It estimates a local-player origin as the camera
position **minus a tunable eye height**, then builds three upright 3D
bounding volumes for chest/abdomen and two legs. The volumes are shifted in
horizontal camera yaw (not pitch). The projected torso rectangle can be
horizontally trimmed **after** 3D projection, preventing a near-camera torso
from covering large portions of the floor. The original center and vertical
range remain unchanged; the leg rectangles are independent. Projected 2D
rectangles are merged into the existing exact-current-frame overlay image
after the mask classifier.

This is **not** a measurement of the actual local-player entity position:
it uses a camera-derived proxy. FPB must be enabled separately by the user.
With FPB turned off, `fpb_projection_protect=1` could still protect some floor
pixels. Turn it off if no body is being rendered.

## Dynamic INI controls

All projection controls are read from
`bin/.l4d2bridge/fg-config.ini` and **hot-reload** approximately every
500 ms. Save the file while the game is running; no restart or shader
recompilation is required for these values:

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `fpb_projection_protect` | `0` | `0/1` | Opt-in proxy mask |
| `fpb_projection_forward` | `-13` | `[-48,48]` | Horizontal forward offset, Source units |
| `fpb_projection_right` | `-1` | `[-48,48]` | Horizontal right offset |
| `fpb_projection_up` | `-1` | `[-48,48]` | World-vertical offset |
| `fpb_projection_width` | `1.0` | `[0.5,2.5]` | Scales 3D horizontal torso and leg volumes |
| `fpb_projection_torso_screen_width` | `1.0` | `[0.1,1.0]` | **Crop only the projected torso's left/right sides**; preserve legs, vertical range, and torso center |
| `fpb_projection_eye_height` | `64` | `[24,85]` | Assumed eye height above entity origin |
| `fpb_projection_{torso,left_leg,right_leg}_width` | `1.0` | `[0,3]` | Independent screen rectangle horizontal scale; zero hides this box |
| `fpb_projection_{torso,left_leg,right_leg}_height` | `1.0` | `[0,3]` | Independent screen rectangle vertical scale; zero hides this box |
| `fpb_projection_{torso,left_leg,right_leg}_x` | `0` | `[-1920,1920]` | Screen X offset (+ moves right) at 1920px reference |
| `fpb_projection_{torso,left_leg,right_leg}_y` | `0` | `[-1080,1080]` | Screen Y offset (+ moves down) at 1080px reference |


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
fpb_projection_torso_screen_width=1.0
fpb_projection_eye_height=64
mask_debug_white=1
```

`dynamic_models=0` is startup-only; restart to change it.
All `fpb_projection_*` controls and `mask_debug_white` are hot-reloadable.
The source's `mask_debug_white=1` is a **combined mask**, not an FPB-only
source view. Toggle `fpb_projection_protect` in game to see the difference.

## GPU integration

At each accepted provider frame, the CPU projects up to three clipped 3D
volumes and produces 0..3 integer screen-space rectangles. These are not
uploaded as bitmaps. The Host's existing mask classifier first writes the
normal exact-current RGB+alpha overlay.

The optional projected-region compositor **reuses the already validated
Asteria HUD compute SPIR-V**, without recompiling FSR or the main
classifier. It samples the current real frame through a Vulkan image view
whose alpha component is mapped to `VK_COMPONENT_SWIZZLE_ONE`. It then
explicitly writes **RGB from the real frame and alpha=1** to the existing
RGBA8 overlay inside the projected region, independently of any
unspecified D3D9 backbuffer alpha.

The optional pipeline is created lazily when projection is enabled and uses
`VK_PIPELINE_CREATE_DISPATCH_BASE_BIT`. Up to three `vkCmdDispatchBase`
calls cover the 8x8-workgroup-rounded rectangles. This intentionally
protects up to seven additional pixels along each edge. Overlapping body
regions are synchronized with compute image barriers. Unsupported Vulkan
dispatch or incomplete descriptor allocations disable **only** the
optional projection, leaving existing FG untouched.

INI edits change only the CPU projection parameters. They do not
rebuild the pipeline or upload any pixel bitmap.

## Safety and diagnostics

- Near-plane intersections are computed before projection; coordinates are
  clipped to the viewport.
- If the proxy is entirely behind/off-screen, zero regions are produced.
  No old-screen rectangles are reused after looking up.
- If summed upper-bound area exceeds 70% of screen area, the frame's
  projection is rejected instead of protecting most of the screen.
- Only provider mode is supported; other FG modes are untouched.
- Unsupported camera matrices, invalid input or unavailable ROI compute support
  disable the projected overlay for the affected frame.
- `live_protection_updated` and `live_protection_rejected` log INI changes.
  `fpb_projection` samples report region count, conservative area estimate,
  coverage rejection and GPU overlay readiness.
- No user-testing of image quality, occlusion or on-device ROI dispatch has yet
  been completed. Screen-space rectangles can protect background between
  body parts, causing local 60 FPS stepping.

## Recommended evaluation

1. Begin with `fpb_projection_protect=0`, FG on, and observe pistol-body
   overlap. Then set it to `1` without restarting.
2. With `mask_debug_white=1`, look straight ahead, down and up. The FPB-only
   difference should appear while looking down and disappear when looking up.
3. Adjust `forward/right/up` one axis at a time to align body; use
   `width` only if the 3D proxy shape is globally too narrow/wide.
   If the broad lower white rectangle extends far to either side, reduce
   `fpb_projection_torso_screen_width` from `1.0` to `0.5`, then `0.4`,
   while leaving existing `forward/up/width` untouched.
4. Check whether pistol-hand/body overlap is sharp and whether the overlay
   incorrectly covers the floor, HUD or weapon.
5. If FPS or latency regress, compare with `fpb_projection_protect=0`.

## Side-wing trim notes

A near-plane torso projection can yield a very wide rectangle below the
narrower leg boxes, even with `fpb_projection_width=0.5`. The new
`fpb_projection_torso_screen_width` applies a **2D horizontal crop to the
torso only**, after near-plane clipping, before the maximum-coverage guard.
It is separate from the 3D `width` setting and is safe to hot reload.

- `1.0`: original full projected torso width (default; preserves baseline)
- `0.5`: retain the central 50% of the torso rectangle
- `0.4`: retain the central 40%
- `0.2`: aggressive trim; may clip actual clothes

If white side panels persist when this value changes, confirm
`dynamic_models=0` at startup and toggle `fpb_projection_protect` to
separate the projected-body mask from HUD/viewmodel/legacy protection.
Also note: the latest branch derives yaw from camera.forward, unlike older
builds that guessed it from camera.right. An old build calibrated to
`forward=+8` may require `forward=-8` after upgrading; compare the resulting
projected rectangles before changing other parameters.

## Fine-tune the three mask boxes independently (new)

The 3D body proxy produces up to three projected rectangles in order:
`torso` (chest/abdomen), `left_leg`, and `right_leg`. The new twelve
`fpb_projection_<box>_<field>` INI keys alter only the corresponding
**2D projected rectangle**, after camera projection and near-plane clipping.
This is intentionally much more aggressive than changing the world-space
body dimensions. It doesn't change camera yaw/pitch or the other boxes.

For example, starting from a good base calibration, trim the large torso
side wings but preserve the two leg rectangles:

```ini
fpb_projection_torso_width=0.35
fpb_projection_torso_height=1.0
fpb_projection_torso_x=0
fpb_projection_torso_y=0

fpb_projection_left_leg_width=1.0
fpb_projection_left_leg_height=1.0
fpb_projection_left_leg_x=0
fpb_projection_left_leg_y=0

fpb_projection_right_leg_width=1.0
fpb_projection_right_leg_height=1.0
fpb_projection_right_leg_x=0
fpb_projection_right_leg_y=0
```

- `width=1.0` retains the original width; `0.35` keeps the central 35%;
  `2.0` doubles the horizontal extent; `0` disables that box.
- `height` does likewise along screen Y. It does not resize width.
- `x=+40` moves the box 40 px right at 1920×1080 reference resolution;
  `y=-30` moves it 30 px up. At higher/lower resolutions, offsets scale
  proportionally in the corresponding axis.
- All widths/heights scale about their own rectangle center, then offsets
  translate the result; coordinates are clipped to the actual viewport.
- The previous `fpb_projection_torso_screen_width` option is kept for
  backward compatibility. Its value multiplies the new
  `fpb_projection_torso_width` effect; keep the legacy value at `1.0`
  while tuning the new option to avoid double shrinking.
- The white debug view includes HUD/viewmodel and any remaining dynamic
  protection, not only the three FPB boxes. Toggle the projection protection
  off to distinguish unrelated white regions.
- A single box may represent only an **approximate** torso or leg volume;
  mask regions can overlap. To identify which box creates the broad side
  wings, set the corresponding box's width to zero and compare.
- New tuning fields are hot-reloaded alongside the previous controls and
  default to identity transforms, so previous saved calibrations remain valid.

**Direction compatibility:** Older builds using camera.right to derive yaw
can require the opposite `fpb_projection_forward` sign from this branch,
which now uses cam.forward. Keep this in mind when carrying a legacy
`forward=8` configuration into a newer build.
