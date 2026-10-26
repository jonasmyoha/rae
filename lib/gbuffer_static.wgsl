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
  // a negative roughness marks a material without the sky's rim light
  // (GbufferRecord packedRoughness): z 0.1 tells the lighting so
  let rough = clamp(abs(d.params.x), 0.045, 1.0);
  let rimZ = select(0.5, 0.1, d.params.x < 0.0);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  let surface = staticSurface(in, d);
  // A level of detail fading in or out (fade_dither.wgsl)
  if (!fadeKeeps(d.fade, in.pos.xy, fadeFrameOffset(F.jitter.xy))) {
    discard;
  }
  let octB = octEncode(surface.nrm);
  var o: FsOut;
  o.gba = vec4<f32>(octB.x, octB.y, rimZ, d.params.z);
  o.gbb = vec4<f32>(surface.albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y,
                     clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);
  return o;
}
