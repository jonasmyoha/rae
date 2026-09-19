/* gpu3d shadows — what C still keeps of the cascaded shadow-map casters
 * (#382, #925).
 *
 * Since #925 the cascade targets, pipelines, bind groups, uniforms, the caster
 * queue and the per-cascade depth passes are the renderer's Rae ShadowCache
 * (lib/ShadowMaps.rae + lib/ShadowMapsSdf.rae over the gpu/ manager); the
 * cascade FITTING is Rae too (lib/Shadow3d.rae, test 579). The caster shaders
 * are lib/shadow_static.wgsl, lib/shadow_skinned.wgsl and lib/shadow_sdf.wgsl,
 * declared with `shader(files:)` in those modules and validated at build
 * (docs/shaders-and-the-compiler.md). What is left here is the sampling
 * function the LEGACY forward renderer's shaders still splice in.
 *
 * ONE PIPELINE PER VERTEX FORMAT. Static (32-byte) and skinned (80-byte,
 * #374) geometry each need a depth-only variant, the skinned one running
 * the same palette skinning with no fragment stage. Miss this and the
 * character silently casts no shadow while everything else does — which
 * reads as a shadow bug, not as a missing pipeline.
 *
 * NOT REVERSE-Z. The main pass uses reverse-Z because it buys float
 * precision against perspective's 1/z. Cascades are orthographic, where
 * depth is linear, so reverse-Z would buy nothing and only risk a
 * convention mismatch. Standard Less depth test, clear to 1.0.
 */



/* ----- metaball clusters as shadow CASTERS ---------------------------
 *
 * Metaballs have no triangles, so the cascade pass cannot rasterise them
 * and they were the one thing in example 110 that floated without a
 * shadow. The design doc assigns SDF shadowing to Layer C (#386), which
 * cone-traces the GI representation — but that is for RECEIVING soft
 * far-field shadows and needs a representation that does not exist yet.
 * Making a metaball CAST into an existing cascade is much smaller: march
 * the same field from the light instead of from the eye, and write depth.
 *
 * ORTHOGRAPHIC RAYS. Under a directional light every ray is parallel, so
 * the ray direction is constant and only the origin varies per shadow-map
 * pixel. That is why this needs the cascade matrix's INVERSE: to turn a
 * shadow-map pixel back into a world-space point on the near plane.
 *
 * The field evaluation is the same transcription as the G-buffer's, for
 * the same reason — WGSL has no include. A divergence here would make a
 * blob cast a shadow shaped differently from the blob, which is worse
 * than no shadow at all. The per-cascade uniform (2 mat4 + lightDir +
 * params + NDC bounds, 176 bytes) and the cluster AABB projection that
 * fills it are Rae (ShadowMapsSdf.rae).
 */


/* The shader sources the Rae ShadowCache builds its depth-only pipelines from. */
