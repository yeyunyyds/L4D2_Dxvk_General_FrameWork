# FPB native-frame-rate protection — experiment v1

This branch tries the smallest GF-only compatibility change for L4D2Visual's first-person body (FPB): **keep pixels classified as dynamic in the current real frame** during generated intermediate frames. The goal is a sharp body at the source game frame rate, **not** a 2× interpolated FPB animation.

## What changes

- `bin/.l4d2bridge/fg-config.ini` adds `fpb_protect=1` in this **experimental branch**.
- The existing GPU dynamic-model protection path uses a colour residual threshold of **0.035** instead of **0.08** when both `dynamic_models=1` and `fpb_protect=1`. A numeric `dynamic_residual_threshold` setting can override either preset.
- Protected pixels continue through the existing exact-current-frame overlay (the shader's `dynamicModel` alpha mask). No new body hook, draw callback, IPC, motion-vector prediction, framebuffer readback, shader interface, or SPIR-V recompilation.
- Set `fpb_protect=0` to restore the previous 0.08 preset with no other behaviour change. `dynamic_models=0` disables this dynamic protection irrespective of the numeric threshold.
- If the INI lacks `fpb_protect`, the bootstrap defaults it to **off**.

## Editable residual threshold

`dynamic_residual_threshold=auto` is backward compatible: `0.035` when `dynamic_models=1` and `fpb_protect=1`, otherwise `0.08`. An explicit decimal between `0.001` and `0.5` replaces this preset. For example:

```ini
[frame_generation]
dynamic_models=1
fpb_protect=1
dynamic_residual_threshold=0.05
first_person_fallback_start=0.28
dynamic_mask_dilation=1
mask_debug_white=1
```

A lower threshold protects more changed pixels, possibly reducing real FG coverage. Higher values protect fewer pixels, possibly increasing body blur/ghosting. Invalid entries fall back to the preset instead of disabling GF; the session log reports `dynamic_residual_threshold` and `dynamic_residual_override_rejected`. A full restart is required. This setting does not alter HUD/viewmodel thresholds or change the underlying scene-depth gate.

`first_person_fallback_start` is configurable within `0.0..1.0`, default `0.28`. It determines the vertical position below which the lower-screen relaxed colour-change protection is used. `1.0` essentially disables that fallback (other colour/depth criteria still protect pixels). The pinned FG SPIR-V has its unique 0.28 literal patched at shader-module creation; the default passes the original binary unchanged.

`dynamic_mask_dilation` supports **0 or 1** (default `1`). At `1`, the dynamic candidate test additionally checks four adjacent pixels and final composite further expands the combined HUD/viewmodel/dynamic overlay to a 3x3 neighbourhood. At `0`, the pinned FG shader's cardinal offset array is replaced by zero offsets and both final-composite neighbourhood loops operate only at (0,0). This affects all overlay types in the final stage; it is not an FPB-only dilation. The normal, white-debug and no-dilation compositor variants are generated from the same checked-in SPIR-V with exact opcode/offset assertions; the existing bytecode is unchanged on default settings. Invalid values fall back to defaults, with reject flags in the Host session log.

The effective depth residual tolerance (currently at least `0.0005`) remains internal and is **not** configurable in this experiment. The current INI only accepts dilation `0/1`, not arbitrary radii.

## Why the experiment may help

FPB renders the local player through the game-native model draw and optionally moves a temporary skeleton copy to track the camera. Its pixels can move differently from the camera-reprojected scene depth. GF already compares final current colour to the previous real frame reprojected into current space; reducing this threshold may catch additional FPB pixels and preserve them rather than moving them through frame generation.

## Limits

**Not a precise FPB segmentation.** It also catches other changing pixels, especially camera-motion residuals, particles, lighting effects and portions of the lower screen. Some static or very low-contrast body pixels still will not enter the protection mask. More overlay means less effective interpolation and potentially some original-frame-rate stepping; broad false positives may look like flicker. This does not fix arbitrary body occlusion, exact silhouettes or source-frame body animation. It also does not alter the legacy (non-provider) FG path.

## White protection overlay diagnostics

Set `mask_debug_white=1` under `[frame_generation]` and fully restart the game.
On **generated intermediate frames only**, the post-FSR overlay shader paints
every pixel that would have been replaced by the original real-frame colour
**pure white (#FFFFFF)**. Real frames and non-protected generated pixels remain
unchanged. Disable it again (`mask_debug_white=0`) after inspecting the coverage.

The white output represents the combined HUD, viewmodel, and dynamic-model
protection mask **after the optional 3x3 dilation (when dynamic_mask_dilation=1)**,
not just FPB or the pre-dilation
`dynamic_protected_pixels` counter. It shows where interpolation was overwritten,
but **does not prove that nonwhite pixels were interpolated correctly**.
The mask may flicker between source frames because FPB is only heuristically
classified. This display mode does not calculate or log a protection percentage.

It uses a second version of the *existing* overlay SPIR-V, with the exact same
coverage logic; only the final stored RGB differs. The original shader remains
the default. There are no new hooks, memory readbacks or CPU masks.

## Manual validation

1. Use a matched GF experimental build with L4D2Visual FPB enabled. Do not swap only one Bridge binary; keep Client/Host32/Host64 and the Layer matched.
2. Baseline: `dynamic_models=1`, `fpb_protect=0`; compare FG on/off while looking down at the body and slowly turning, walking and crouching.
3. Trial: `dynamic_models=1`, `fpb_protect=1`; restart fully to apply the config and repeat the same actions. Compare body ghosting, body clarity, nearby world background and HUD.
4. Control: `dynamic_models=0`; confirm the new toggle cannot force protection when dynamic protection is disabled.
5. Enable `mask_debug_white=1` and look at the generated output; compare the size/location of white regions with `fpb_protect=0` and `fpb_protect=1`, including HUD and FPB body.
6. Check `fg-bootstrap.txt` for `fpb_protect` and `mask_debug_white`. In current Provider mode, `dynamic_protected_pixels` is **not** a reliable percentage because GPU quality reduction/readback is skipped. White coverage is visual only.

This is source-only until a new build and real-game comparison are completed. Do **not** claim FPB compatibility as validated.
