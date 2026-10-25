# v39 complete-model performance result

The `rt-performance-20261024-163117-f1f6665e.zip` archive is internally valid and contains build `4da120256c28e6e1ba5adf19be5cd0e11f3c2a67` (pilot revision 39). The user reported normal, complete rendering at roughly 5 FPS. Runtime evidence agrees: all three 30-frame RT phases completed, 60 raster Presents survived after RT shutdown, every API mesh/fence was retired, and there was no device loss, fence failure, model-budget omission, or resident texture leak.

The session submitted 59,130 model parts in 90 RT frames. Of those, 720 used persistent GPU skinning and 58,410 used explicit CPU-pose meshes. The final 16-mesh phase still reconstructed about 641 CPU-pose parts per frame, so the result establishes visual/lifecycle correctness but also identifies the remaining throughput bottleneck.

| Persistent GPU-skin limit | FPS | CPU-pose submissions | Model time | CPU skin time | Host device-thread CPU |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 3.56 | 19,710 | 6,370.96 ms | 2,846.00 ms | 4,593.75 ms |
| 8 | 4.44 | 19,470 | 5,993.65 ms | 2,819.53 ms | 4,531.25 ms |
| 16 | 4.53 | 19,230 | 5,873.54 ms | 2,767.95 ms | 4,359.38 ms |

The first phase includes a one-time 1.30-second Present/compile cost; later phases spend only about 3 ms total in Present. The meaningful steady-state cost is model preparation, especially CPU pose reconstruction, geometry hashing/building, and per-pose mesh creation. Raising the limit from 8 to 16 offloads only eight additional parts out of roughly 657 parts per frame, so its small FPS gain is expected.

GPU Engine PDH sampling stopped after Windows returned transient status `0xc0000bba`; process CPU sampling continued. This result therefore contains no trustworthy per-phase GPU utilization curve. v40 keeps the complete v39 fallback, scales audited GPU skinning through 16/64/128, and treats that PDH status as a recoverable sample gap.

The final game exit contains the recurring `necola.dll` access violation after both Bridge sides completed cleanup. It makes strict process-exit acceptance fail but does not invalidate the already completed RT lifecycle evidence. It remains reported separately rather than being reclassified as a clean exit.
