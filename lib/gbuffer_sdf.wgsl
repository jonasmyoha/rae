// Metaball clusters into the G-buffer (#392): the raymarch (sdf_surface.wgsl,
// composed in front), then the G-buffer encode — shaded by the same lighting
// pass as every triangle.
struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
  @builtin(frag_depth) depth: f32,
};
@fragment
fn fs(in: VsOut) -> FsOut {
  let march = marchMetaballs(in.ndc);
  if (!march.hit) { discard; }
  let worldPos = march.worldPos;
  let N = sceneNormal(worldPos);
  let oct = octEncode(N);
  let albedo = sceneAlbedo(worldPos);
  let rough = clamp(P.emissiveRoughness.a, 0.045, 1.0);
  let clip = F.viewProj * vec4<f32>(worldPos, 1.0);
  let clipPrev = F.prevViewProj * vec4<f32>(worldPos, 1.0);
  let motion = (clip.xy / clip.w - clipPrev.xy / clipPrev.w) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var emissive = 1.0;
  var mode = 0.0;
  let emitPeak = sdfEmitPeak();
  if (emitPeak > 0.0) {
    emissive = clamp(log(1.0 + emitPeak) / 6.91, 0.0, 1.0);
    mode = 0.33333333;
  }
  var o: FsOut;
  // z = 1: TAA-reactive (render-perf-10): the motion above is the camera's
  // only, the deforming balls themselves would trail on history
  o.gba = vec4<f32>(oct.x, oct.y, 1.0, mode);
  o.gbb = vec4<f32>(albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, clamp(P.baseColorMetallic.a, 0.0, 1.0), emissive);
  o.depth = clamp(clip.z / clip.w, 0.0, 1.0);
  return o;
}
