// Octahedral normal encoding. A unit vector has two degrees of freedom;
// this is the area-preserving map onto two channels. The lower hemisphere
// folds outward across the |x|+|y|=1 diamond, which is what octWrap does.
// A PART of every shader that reads or writes the G-buffer normal.
fn octWrap(v: vec2<f32>) -> vec2<f32> {
  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
  return (vec2<f32>(1.0) - abs(v.yx)) * s;
}
fn octEncode(n: vec3<f32>) -> vec2<f32> {
  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
  if (n.z < 0.0) { p = octWrap(p); }
  return p * 0.5 + vec2<f32>(0.5);
}
fn octDecode(e: vec2<f32>) -> vec3<f32> {
  let f = e * 2.0 - vec2<f32>(1.0);
  var n = vec3<f32>(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  let t = max(-n.z, 0.0);
  n = vec3<f32>(n.x + select(t, -t, n.x >= 0.0),
                n.y + select(t, -t, n.y >= 0.0), n.z);
  return normalize(n);
}
