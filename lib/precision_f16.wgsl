// The renderer's precision prelude, composed FIRST in front of a shader
// (render-perf-12): `half` is 16-bit float on a device with shader-f16
// (gpu/DeviceFeatures). Color and shading arithmetic is written in `half`;
// depth, positions and reconstruction from depth stay f32.
enable f16;
alias half = f16;
alias half2 = vec2<f16>;
alias half3 = vec3<f16>;
alias half4 = vec4<f16>;
