# RT world v25 result: stretched skinned models and Vulkan device loss

The `rt-world-20261021-130022-ce4a0064.zip` collection is incomplete. It used build `1eddbe65eab81104aee7b2184c43a9bc95bf243f`, entered the model phase, displayed stretched characters, and stopped after present 209.

The final completed frame submitted 609 API meshes, including 551 skinned submissions. Nineteen persistent skinned meshes were created and reused 532 times. No draw or memory budget omitted work; 23 live entries occupied 8,140,884 bytes and one fence remained pending. The next fence query returned `0x8876086c` (`D3DERR_DEVICELOST`). `remix-dxvk.log` records `VK_ERROR_DEVICE_LOST` while synchronizing and submitting the Vulkan fence at the same point.

The failure is caused by a layout mismatch in the pinned custom Remix runtime. The observer supplies the documented scalar totals: three float weights and three uint32 indices per vertex. The runtime copies all weights, but describes the weight buffer to the skinning shader with a four-byte vertex stride. A three-weight vertex requires a twelve-byte stride. Vertex zero is read correctly; subsequent vertices begin inside the previous vertex's tuple, which explains the visible stretching before invalid geometry reaches acceleration-structure work and the device is lost.

The Bridge scalar-count transport patch remains correct and is retained. v26 patches the separate runtime stride, validates pointer/count/index bounds before allocation, and uses a full CPU skin-output bounds oracle before submitting pilot geometry. The runtime source and patch hashes are recorded in its prerequisite manifest so an old runtime cannot silently satisfy the new package.

Acceptance requires a new pinned runtime build and a new one-F9 game package. The next game run must complete 600 diagnostic presents and the 60-second preview, show stable characters, record no `api_mesh_fence_failed` or `VK_ERROR_DEVICE_LOST`, and show nonzero persistent skinning reuse.
