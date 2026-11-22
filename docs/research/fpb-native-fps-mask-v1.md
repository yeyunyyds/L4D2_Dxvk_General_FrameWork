# FPB native-frame-rate protection — experiment v1

This branch tries the smallest GF-only compatibility change for L4D2Visual's first-person body (FPB): **keep pixels classified as dynamic in the current real frame** during generated intermediate frames. The goal is a sharp body at the source game frame rate, **not** a 2× interpolated FPB animation.

## What changes

- `bin/.l4d2bridge/fg-config.ini` adds `fpb_protect=1` in this **experimental branch**.
- The existing GPU dynamic-model protection path uses a colour residual threshold of **0.035** instead of **0.08** when both `dynamic_models=1` and `fpb_protect=1`.
- Protected pixels continue through the existing exact-current-frame overlay (the shader's `dynamicModel` alpha mask). No new body hook, draw callback, IPC, motion-vector prediction, framebuffer readback, shader interface, or SPIR-V recompilation.
- Set `fpb_protect=0` to restore the previous 0.08 threshold with no other behaviour change. `dynamic_models=0` also disables the new sensitivity.
- If the INI lacks `fpb_protect`, the bootstrap defaults it to **off**.

## Why the experiment may help

FPB renders the local player through the game-native model draw and optionally moves a temporary skeleton copy to track the camera. Its pixels can move differently from the camera-reprojected scene depth. GF already compares final current colour to the previous real frame reprojected into current space; reducing this threshold may catch additional FPB pixels and preserve them rather than moving them through frame generation.

## Limits

**Not a precise FPB segmentation.** It also catches other changing pixels, especially camera-motion residuals, particles, lighting effects and portions of the lower screen. Some static or very low-contrast body pixels still will not enter the protection mask. More overlay means less effective interpolation and potentially some original-frame-rate stepping; broad false positives may look like flicker. This does not fix arbitrary body occlusion, exact silhouettes or source-frame body animation. It also does not alter the legacy (non-provider) FG path.

## Manual validation

1. Use a matched GF experimental build with L4D2Visual FPB enabled. Do not swap only one Bridge binary; keep Client/Host32/Host64 and the Layer matched.
2. Baseline: `dynamic_models=1`, `fpb_protect=0`; compare FG on/off while looking down at the body and slowly turning, walking and crouching.
3. Trial: `dynamic_models=1`, `fpb_protect=1`; restart fully to apply the config and repeat the same actions. Compare body ghosting, body clarity, nearby world background and HUD.
4. Control: `dynamic_models=0`; confirm the new toggle cannot force protection when dynamic protection is disabled.
5. Check `fg-bootstrap.txt` for `fpb_protect`, and `fg-events.jsonl` session for `fpb_protection_requested`. Inspect `dynamic_protected_pixels` and Asteria's accepted generated-frame records rather than treating an enabled flag as proof of image quality.

This is source-only until a new build and real-game comparison are completed. Do **not** claim FPB compatibility as validated.
