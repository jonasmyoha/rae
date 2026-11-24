// A light value a blend, a blur or a 16-bit float can hold: no infinity and no NaN, nothing below
// zero, at most HDR_TOP (a half float ends at 65504, rg11b10ufloat at 64512). One bad pixel is
// otherwise spread by a blur: the bloom made a block of the screen of it, black once tone-mapped.
// By the bits: a compiler may take x != x, or a clamp of a NaN, for something else.
const HDR_TOP: f32 = 60000.0;
fn hdrFinite(c: vec3<f32>) -> vec3<f32> {
  let bits = bitcast<vec3<u32>>(c);
  let special = (bits & vec3<u32>(0x7f800000u)) == vec3<u32>(0x7f800000u);
  let infinite = bits == vec3<u32>(0x7f800000u);
  let held = select(c, vec3<f32>(0.0), special);
  return select(clamp(held, vec3<f32>(0.0), vec3<f32>(HDR_TOP)), vec3<f32>(HDR_TOP), infinite);
}
