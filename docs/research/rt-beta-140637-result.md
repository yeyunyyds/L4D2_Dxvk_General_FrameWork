# RT Beta v29 high-density result (2026-10-22)

The 30-frame gate completed and the five-minute preview started, but the run ended after 17.453 seconds and 64 preview frames. The runtime reported `api_mesh_fence_failed` with `D3DERR_DEVICELOST`; the Remix log independently reported `VK_ERROR_DEVICE_LOST`. This result is incomplete and does not pass the Beta gate.

The failure occurred under sustained model pressure rather than an unbounded GPU-skin burst. Frame 60 contained 629 model submissions, 183,896,344 live mesh bytes, 1,217 live entries, and one pending fence. The preview submitted 40,920 models in total. GPU skinning obeyed the v29 limit exactly: 1,024 submissions over 64 frames, with 20 created and 1,004 reused.

One 640×400 readback completed at frame 90 with no non-finite RGB values. It shows the scene and character silhouettes, but it cannot establish five-minute stability or complete image quality. Native TAA was never reached because the runtime failed before 30 seconds.

Host timing also remained substantial. The first 30-frame timing window recorded 20,031 model calls, 4,818.2872 ms in model preparation, and 3,596.7699 ms in CPU skinning. The next partial window recorded 21,007 model calls, 4,980.1511 ms in preparation, and 3,710.1048 ms in CPU skinning. GPU engine telemetry was collected, but overlapping per-engine counters cannot be summed as whole-GPU utilization.

v30 therefore keeps a GPU baseline instead of disabling GPU skinning. It permits 16 GPU-skinned submissions when the preceding complete frame has at most 256 model submissions and 4 when it exceeds 256. The warm-up frame and excess models use the existing CPU-pose path. A successful high-density five-minute run is required before increasing the high-density tier from 4 to 8, 12, and 16.
