# v41 optimized CPU-pose result

The `rt-performance-20261024-223513-2a45ce7d.zip` archive is internally valid and contains build `b9456878fc43da987194e6579f942347f0cde714` (pilot revision 41). All three fixed GPU-64 RT windows completed, all resources retired, 60 raster Presents survived, and the zero-omission completeness contract passed. The only overall-acceptance failure is the recurring exit-time `necola.dll` access violation after the RT lifecycle had already completed.

| Window | FPS | GPU 3D average / p50 | CPU-pose submissions | Model time | CPU skin time | Present time |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU-64 warm-up | 5.27 | 21.37% / 27.58% | 14,506 | 3,515.3 ms | 981.6 ms | 1,318.6 ms first compile |
| GPU-64 steady 1 | 7.67 | 37.45% / 36.99% | 14,160 | 3,085.1 ms | 961.4 ms | 3.9 ms |
| GPU-64 steady 2 | 7.72 | 39.61% / 42.19% | 14,160 | 3,053.0 ms | 950.6 ms | 3.9 ms |

Compared with v40's GPU-64 window at 4.54 FPS, v41 reaches 7.67–7.72 FPS in steady state, roughly 70% higher. CPU skin time fell from 2,552 ms to about 951–961 ms per 30 frames. Some of the absolute difference also reflects a lighter captured scene: v41 submitted about 536 model parts per frame versus roughly 663 in v40. Normalized per CPU-pose submission, skin time still fell from about 0.142 ms to 0.067 ms, a reduction of roughly 53%.

The v41 entry increase removed all five prior `mesh_entries` omissions while retaining the 512 MiB geometry cap. Dynamic-pose hash work also fell substantially. The remaining steady window spends about 101.8 ms per frame in the model path: 63.9 ms in preparation, including 31.7 ms in CPU skinning, plus 21.2 ms in geometry building and 7.1 ms in mesh creation. About 472 model parts per frame still use CPU poses.

At GPU-64 the Arc 3D engine median is only about 37–42%, materially below v40's saturated GPU-128 phase. The next bounded experiment therefore retains every v41 CPU optimization and measures GPU limits 64, 80 and 96. It deliberately stays below 128 while locating the new balance after the CPU fast-path change.
