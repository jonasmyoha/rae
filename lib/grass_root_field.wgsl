// The color a grass blade's root takes where it stands (grass_compute.wgsl
// grassRootColor), the lib default: the grass field of grass_field.wgsl
// round the frame's ground palette (G.e), mixed toward its dry grass by the
// soil's moisture (G.j), as the plain terrain paints its grass. A
// composition lists exactly one grassRootColor file; an app whose terrain
// paints its ground in another way lists its own (grass_root_painted.wgsl).
fn grassRootColor(position: vec2<f32>, biome: RaeBiome) -> vec3<f32> {
  return grassFieldGreen(position, biome);
}
