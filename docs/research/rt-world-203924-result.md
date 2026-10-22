# v28 B580 bounded GPU skinning result

The three uploaded volumes reconstruct a CRC-clean result from commit `df465301e4ffce5cf123ae7bc6223aa819bca534`. The 600-Present diagnostic and the independent 60.296-second preview both completed. The launcher later timed out only because the game/Host remained open during the normal-exit wait; the runtime recorded no exception, device loss, fence failure, or backend object leak.

The bounded path was exercised for 360 diagnostic frames. Every frame stayed at 13 or 16 successful GPU-skinned submissions, never above the configured limit of 16. The session completed 5,580 persistent GPU-skinned submissions: 35 mesh creations and 5,545 reuses. Final mesh entries, bytes, pending fences, and budget omissions were all zero. This accepts 16 submissions per frame as the current Arc B580 safety limit; it does not establish that a larger burst is safe.

All reviewed world and preview color images show complete animated models, clothing, hair, limbs, sky, and world geometry. The three preview images differ in camera and pose and contain finite color. The previous v25/v26 fragmentation and frozen-output signature is absent.

The preview produced 340 frames in 60.296 seconds, approximately 5.64 FPS. That is not a controlled comparison with v27 and does not prove a performance gain. Host timings still show model preparation, CPU skinning of excess draws, texture work, hashing, and pose-mesh construction dominating Present. The next product step therefore keeps the verified limit, retains CPU fallback for complete display, and adds a longer gameplay Beta session before any limit increase.
