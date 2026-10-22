# RT world v26 result: persistent GPU skinning still loses the device

The `rt-world-20261021-185953-2d2b49a7.zip` collection is incomplete. It loaded the pinned v26 runtime and build `da01257c3318c40a06836433e699ff03e8f9bf90`. The user observed fragmented characters and a frozen image while the game process remained alive.

The 12-byte weight stride patch was present, so this run disproves the v25 diagnosis as a complete fix. Frames through 243 completed with no application budget omission. At frame 241 the combined model stage increased from 21 to 671 API draws per frame. By frame 243 it had submitted 3,102 skinned draws, created 131 persistent skinned meshes and reused them 2,971 times. The following fence query returned `0x8876086c` (`D3DERR_DEVICELOST`). `remix-dxvk.log` records repeated `VK_ERROR_DEVICE_LOST` fence and idle failures followed by failed command submission at the same time.

The evidence does not identify the remaining internal Remix skinning defect. It does isolate the unsafe boundary for this milestone: v24's CPU-skinned pose meshes displayed complete characters, while both persistent-GPU-skinning builds v25 and v26 produced corrupt character geometry before device loss. The next package therefore restores CPU pose meshes by default and retains the GPU implementation only for isolated fixtures. All other world, infected, texture and material coverage remains enabled.

v27 acceptance requires all persistent-skin counters to remain zero, completion of the 600-present diagnostic and 60-second preview, no `VK_ERROR_DEVICE_LOST`, and user confirmation that characters remain complete and animated.
