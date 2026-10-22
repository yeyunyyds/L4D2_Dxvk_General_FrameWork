# RT world v27 result: stable complete CPU-skinned fallback

The three uploaded split volumes reconstruct a valid outer ZIP containing the complete `rt-world-20261021-200055-18a6e16e` result. It used build `30701437d08f270e751512a27cf3bb5fd74f8d46`, completed all 600 diagnostic presents, and completed the independent 60.125-second preview with 442 frames and three successful finite color readbacks.

The safety rollback worked exactly as requested. Diagnostic and preview counters report zero persistent GPU-skinned submissions, creations and reuses. The diagnostic submitted 145,761 CPU-skinned model draws, created 139,828 pose meshes, omitted none, then destroyed all 139,828 meshes with zero live bytes and zero pending fences. No observer API failure, `VK_ERROR_DEVICE_LOST`, exception, frozen readback, or non-finite preview color was recorded.

The color readbacks at frames 240, 300, 420, 540 and all three preview checkpoints show complete animated characters, including hair, clothing and limbs. The camera and poses change between preview readbacks, which rules out the prior frozen-output symptom. This accepts v27 as the stable full-character baseline. The remaining cleanup warning records 40 backend objects at process-log collection but no exception; runtime-owned mesh resources had already reached zero.

Performance is now clearly Host-supply limited. The 60-second preview averaged 7.35 output frames per second. The CPU model path consumed most wall time: preparation and CPU skinning dominate, while Present averaged about 0.11 ms and the Host GPU 3D engine averaged 15.4% over the collection (about 23% late in the run). This is expected from the safety rollback and is not a GPU rendering limit.

The next optimization must retain this baseline. A bounded persistent-skin canary can test whether the v26 failure was caused by the jump from 21 to roughly 654 skinned submissions per frame rather than a universally invalid skin payload. It must cap submissions per frame, keep all excess models on the proven CPU path, expose the cap in evidence, and preserve automatic resource retirement.
