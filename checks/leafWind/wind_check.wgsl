@group(0) @binding(0) var<storage, read_write> results: array<u32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let time = f32(id.x) * 0.37;
  let pivot = vec3<f32>(7.0, -2.0, 5.0);
  let first = pivot + vec3<f32>(0.7, 0.0, 0.0);
  let second = pivot + vec3<f32>(0.0, 0.5, 0.2);
  let normal = vec3<f32>(0.0, 0.0, 1.0);
  let wind = vec4<f32>(1.0, 0.3, 0.8, time);
  let flexibility = select(1.0, -1.0, id.x >= 32u);
  let a = windLeaf(first, normal, pivot, vec3<f32>(0.0), flexibility, wind);
  let b = windLeaf(second, normal, pivot, vec3<f32>(0.0), flexibility, wind);
  let attachment = windLeaf(pivot, normal, pivot, vec3<f32>(0.0), flexibility, wind);
  let calm = windLeaf(first, normal, pivot, vec3<f32>(0.0), flexibility, vec4<f32>(0.0, 0.0, 1.0, time));
  let rigid = windLeaf(first, normal, pivot, vec3<f32>(0.0), 0.0, wind);
  let preserved = abs(distance(a.position, b.position) - distance(first, second)) < 0.00001
    && abs(distance(a.position, pivot) - distance(first, pivot)) < 0.00001;
  let normals = abs(length(a.normal) - 1.0) < 0.00001
    && abs(dot(a.position - pivot, a.normal) - dot(first - pivot, normal)) < 0.00001;
  let still = all(attachment.position == pivot) && all(calm.position == first) && all(rigid.position == first);
  let next = windLeaf(first, normal, pivot, vec3<f32>(0.0), flexibility,
    vec4<f32>(wind.xyz, time + 0.001));
  let continuous = distance(next.position, a.position) < 0.002;
  results[id.x] = select(0u, 1u, preserved && normals && still && continuous);
  // A fresh breeze must move a 70 cm spray tip visibly, not just millimetres.
  let threshold = select(0.00001, 0.05, id.x >= 32u);
  results[64 + id.x] = select(0u, 1u, distance(a.position, first) > threshold);
}
