// The static G-buffer fragment: writes the surface attributes (composed after
// gbuffer_static_surface.wgsl).
struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};
@fragment
fn fs(in: VsOut) -> FsOut {
  let d = draws[in.inst];
  let rough = clamp(d.params.x, 0.045, 1.0);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  let surface = staticSurface(in, d);
  let octB = octEncode(surface.nrm);
  var o: FsOut;
  o.gba = vec4<f32>(octB.x, octB.y, 0.5, d.params.z);
  o.gbb = vec4<f32>(surface.albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y,
                     clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);
  return o;
}
