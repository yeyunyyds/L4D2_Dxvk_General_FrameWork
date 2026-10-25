# v40 GPU-skin scaling result

The `rt-performance-20261024-214155-03d3bcba.zip` archive is internally valid and contains build `f803d48024fb3a99c496169b745b9e9042090022` (pilot revision 40). The user reported normal rendering with no meaningful frame-rate improvement. Runtime evidence agrees: all 90 RT frames and the final 60 raster Presents completed, RT resources retired to zero, both Bridge sides cleaned up, and there was no device loss or fence failure.

| GPU-skin limit | FPS | GPU 3D average / p50 | CPU-pose submissions | Model time | CPU skin time | Present time |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 3.25 | 16.09% / 21.36% | 20,615 | 7,144.0 ms | 3,156.0 ms | includes 1.22 s first-compile stall |
| 64 | 4.54 | 29.99% / 30.09% | 17,970 | 5,824.5 ms | 2,552.4 ms | 3.9 ms |
| 128 | 3.78 | 55.47% / 84.96% | 16,138 | 5,169.6 ms | 2,051.2 ms | 1,948.5 ms |

The GPU transfer worked: increasing the limit reduced CPU-pose and CPU-skin work, while the Arc 3D engine rose from roughly 16% to a sustained median near 85% in the 128 phase. The 128 phase then lost more time in Present than it saved on CPU work. GPU-64 is therefore the measured balance for the next optimization pass; increasing the GPU-skin limit again is not supported by this result.

Strict collection review reports five `mesh_entries` omissions. They occurred at the 2,048 live-mesh entry ceiling during the first phase. The original Draw remained as the fallback and the user saw a normal image, so this is not a device or geometry failure, but it prevents the run from satisfying the zero-omission completeness contract. Peak live geometry was about 309 MiB, below the independent 512 MiB byte cap, which supports raising the entry count to 3,072 while retaining the byte limit.

The remaining target is the explicit CPU-pose path. Across each 30-frame window it still spent 2.05–3.16 seconds in skinning. Only 1,145 of 54,723 CPU-pose submissions reused a mesh, about 2.1%, so hashing dynamic vertex and index payloads twice per pose has little value. v41 fixes GPU skinning at 64, validates each referenced bone matrix once per draw, skips exact zero-weight blend terms, bypasses low-hit SHA256 reuse for dynamic poses using exact frame/draw identities, and raises the entry ceiling under the unchanged byte cap.
