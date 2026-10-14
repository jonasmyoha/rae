// The renderer's precision prelude for a device WITHOUT shader-f16
// (render-perf-12): the same shaders, with `half` meaning f32.
alias half = f32;
alias half2 = vec2<f32>;
alias half3 = vec3<f32>;
alias half4 = vec4<f32>;
