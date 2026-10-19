LDB RT frozen-geometry GPU replay 1

1. Extract this ZIP next to your previous rt-world-pilot folder so its verified
   cache/runtime can be reused. Close L4D2 to free GPU memory.
2. Drag the PREVIOUS v15 result ZIP onto run-replay.cmd. You may instead run
   run-replay.cmd and drag the result ZIP or extracted result folder into its
   window, then press Enter. A 7z must first be extracted with your usual tool.
3. Wait for all 19 cases to finish. Upload ALL ZIPs listed after UPLOAD:.
   There is no game installation, map load, F8, F9, or trigger file.

Input: the same v15 result that contains full model payloads at frames 240/420.
This reuses your own local model capture; it does not ship your model assets.
Keep the old result. Results are new folders; input is never overwritten.
The program may download the existing pinned runtime if no verified sibling
cache exists. Python is included. Final PNGs are in each case/capture folder.
Results are split automatically below the attachment size limit.

Two scenes x six routes = 12 cases:
- raster baseline with RT disabled;
- legacy D3D9: release buffers immediately / after Present / persistent;
- explicit Remix Mesh API, raw UV / repair only degenerate UV triangles.
Seven extra combined-scene API material cases: rough, metal, alpha cutout,
alpha blend, additive, emissive, thin glass. Neutral diffuse is the API baseline.

All cases use a frozen pose, the captured camera, and synthetic materials.
Legacy routes keep the captured compact 32-byte vertices and normalized indices.
API routes expand triangles and generate flat normals; safe-UV changes only
invalid tangent UVs. These extra differences limit route-to-route attribution.
Cull is disabled for geometry diagnosis. Original textures, animation, full Toon,
material blending and map lighting are not reproduced. Glass/transparent objects
against a black background can be faint or invisible; that alone is not a pass.

API return success and completed execution are NOT image acceptance. Review
color, depth, normals, repeat-image stability and raster shape together. This
build does not fix the real-time game converter or change Source thread settings.
No performance conclusion follows from this standalone diagnostic.
