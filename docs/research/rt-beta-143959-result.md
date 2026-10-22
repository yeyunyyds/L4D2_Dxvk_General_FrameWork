# RT Beta v30 performance result (2026-10-22)

The v30 run did not pass. It rendered 436 preview frames in 144.609 seconds, about 3.0 FPS overall, then failed at `api_mesh_fence_failed` with `D3DERR_DEVICELOST`. The Remix log independently reports `VK_ERROR_DEVICE_LOST`.

The run demonstrates two different bottleneck intervals. During the first 30 seconds without native TAA, the Host process 3D engine averaged about 18.7%. The later native-TAA interval averaged about 78.4% on the same per-process 3D engine and accumulated large `Present` waits. The user's roughly 20% observation therefore describes the initial CPU-supply-limited interval but not the whole run.

CPU pose processing remained structurally expensive. The preview submitted 201,996 model instances. GPU skinning submitted 2,752 instances, created 57 skinned meshes, and reused 2,695. By frame 420 the overall path had already created 171,352 meshes while reusing only 21,067. The GPU-skinned bind-pose path therefore has much better reuse than CPU-pose geometry keyed by changing animated vertices.

The first 60-frame timing interval took 9,214.8 ms and spent 7,253.1 ms in model handling, including 4,640.7 ms in preparation, 3,438.0 ms in the nested skin scope, 996.9 ms in geometry construction, and 370.4 ms in mesh creation. Later native-TAA intervals combined Host model work with substantial GPU/Present waits.

v31 pauses the five-minute Beta. A dedicated 240-frame test runs four 60-frame GPU-skin limits: 4, 64, 256, and 1024. It keeps TAA switching and image readbacks disabled and removes the duplicate full CPU output skin operation from the performance path while retaining finite/range validation of source positions, weights, indices, and bone matrices.
