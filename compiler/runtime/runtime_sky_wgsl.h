/* Sky shader source, shared by BOTH renderers.
 *
 * Extracted verbatim from the deferred lighting pass so the forward path can
 * have the same sky rather than a second implementation of it (#418). The
 * functions read their inputs off a uniform named `L`; each pass declares its
 * own struct for that, and only needs to CONTAIN the fields used here:
 *
 *     sunDir, sunColor, clearColor, skyParams, skyZenith, skyHorizon, hosek[9]
 *
 * A pass may carry more (the deferred LightU does) — WGSL resolves the field
 * offsets per struct, so the extra members are free. What must NOT differ is
 * the meaning of the fields, which is why they are listed here rather than
 * left to be rediscovered at each call site.
 *
 * Included as a string constant rather than compiled: these are WGSL sources
 * assembled at pipeline-creation time, the same way every other shader in this
 * runtime is built.
 */
#ifndef RAE_SKY_WGSL_H
#define RAE_SKY_WGSL_H

#define RAE_SKY_WGSL \
/* ---- sky (#400 procedural, #404 stylised) -------------------------- \
 * The SAME model as lib/sky.rae, evaluated per pixel. Two copies of a \
 * formula is a real cost, and it is paid deliberately: the CPU one \
 * answers "what ambient does this scene get" once per frame, this one \
 * answers "what colour is this pixel of sky", and a round trip through a \
 * texture to share them would cost more than the duplication. Test 584 \
 * pins the CPU one; the relationships it asserts are what this must also \
 * show, and a divergence is visible as a background that does not match \
 * the lighting. \
 */ \
"fn preethamF(A: f32, B: f32, C: f32, D: f32, E: f32, ct: f32, g: f32) -> f32 {\n" \
/* Clamped away from zero: the model diverges at the horizon, and an \
 * infinity here reaches the tonemapper and takes the frame with it. */ \
"  let c = max(ct, 0.01);\n" \
"  let cg = cos(g);\n" \
"  return (1.0 + A * exp(B / c)) * (1.0 + C * exp(D * g) + E * cg * cg);\n" \
"}\n" \
"fn skyProcedural(dir: vec3<f32>, toSun: vec3<f32>, t: f32) -> vec3<f32> {\n" \
"  let g = acos(clamp(dot(dir, toSun), -1.0, 1.0));\n" \
"  let ts = acos(clamp(max(toSun.z, 0.01), -1.0, 1.0));\n" \
"  let Ay = 0.1787 * t - 1.4630; let By = -0.3554 * t + 0.4275;\n" \
"  let Cy = -0.0227 * t + 5.3251; let Dy = 0.1206 * t - 2.5771;\n" \
"  let Ey = -0.0670 * t + 0.3703;\n" \
"  let Ax = -0.0193 * t - 0.2592; let Bx = -0.0665 * t + 0.0008;\n" \
"  let Cx = -0.0004 * t + 0.2125; let Dx = -0.0641 * t - 0.8989;\n" \
"  let Ex = -0.0033 * t + 0.0452;\n" \
"  let Az = -0.0167 * t - 0.2608; let Bz = -0.0950 * t + 0.0092;\n" \
"  let Cz = -0.0079 * t + 0.2102; let Dz = -0.0441 * t - 1.6537;\n" \
"  let Ez = -0.0109 * t + 0.0529;\n" \
"  let fy = preethamF(Ay,By,Cy,Dy,Ey, dir.z, g) / preethamF(Ay,By,Cy,Dy,Ey, 1.0, ts);\n" \
"  let fx = preethamF(Ax,Bx,Cx,Dx,Ex, dir.z, g) / preethamF(Ax,Bx,Cx,Dx,Ex, 1.0, ts);\n" \
"  let fz = preethamF(Az,Bz,Cz,Dz,Ez, dir.z, g) / preethamF(Az,Bz,Cz,Dz,Ez, 1.0, ts);\n" \
"  let ts2 = ts * ts; let ts3 = ts2 * ts;\n" \
"  let chi = (4.0 / 9.0 - t / 120.0) * (PI - 2.0 * ts);\n" \
"  let zY = max((4.0453 * t - 4.9710) * tan(chi) - 0.2155 * t + 2.4192, 0.0);\n" \
"  let zx = (0.00166*ts3 - 0.00375*ts2 + 0.00209*ts) * t * t\n" \
"         + (-0.02903*ts3 + 0.06377*ts2 - 0.03202*ts + 0.00394) * t\n" \
"         + (0.11693*ts3 - 0.21196*ts2 + 0.06052*ts + 0.25886);\n" \
"  let zy = (0.00275*ts3 - 0.00610*ts2 + 0.00317*ts) * t * t\n" \
"         + (-0.04214*ts3 + 0.08970*ts2 - 0.04153*ts + 0.00516) * t\n" \
"         + (0.15346*ts3 - 0.26756*ts2 + 0.06670*ts + 0.26688);\n" \
"  let xx = zx * fx; let yy = max(zy * fz, 0.0001); let YY = zY * fy * 0.05;\n" \
"  let X = xx * YY / yy; let Z = (1.0 - xx - yy) * YY / yy;\n" \
"  let rgb = vec3<f32>( 3.2406*X - 1.5372*YY - 0.4986*Z,\n" \
"                      -0.9689*X + 1.8758*YY + 0.0415*Z,\n" \
"                       0.0557*X - 0.2040*YY + 1.0570*Z);\n" \
"  return max(rgb, vec3<f32>(0.0));\n" \
"}\n" \
/* STYLISED: a vertical ramp between two authored colours, quantised the \
 * same way the toon terminator is. Bands rather than a smooth gradient is \
 * the point — a cel scene under a photographic gradient reads as models \
 * pasted onto a photograph, which is the failure §3C of the design doc \
 * exists to prevent. The colours come from the scene's palette, so the \
 * sky and the character's shadow tint agree by construction. */ \
"fn skyStylised(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {\n" \
"  let h = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);\n" \
"  let bands = max(L.skyZenith.w, 1.0);\n" \
"  let s = h * bands;\n" \
"  let i = floor(s);\n" \
"  let band = (i + smoothstep(0.35, 0.65, s - i)) / bands;\n" \
"  var c = mix(L.skyHorizon.rgb, L.skyZenith.rgb, band);\n" \
/* A warm glow toward the sun, kept broad and soft so it reads as painted \
 * light rather than as a lens artefact. */ \
"  let g = clamp(dot(dir, toSun), 0.0, 1.0);\n" \
"  c = c + L.sunColor.rgb * pow(g, 6.0) * 0.35;\n" \
"  return c;\n" \
"}\n" \
/* HOSEK-WILKIE: the analytic formula, nine coefficients per channel, cooked \
 * on the CPU from the fitted dataset (third_party/hosek_wilkie). Preetham \
 * above is kept as the table-free fallback; this is the better fit near the \
 * horizon and at low sun, and it is the only one of the two that responds to \
 * ground albedo. */ \
"fn hosekChannel(base: i32, cosTheta: f32, cosGamma: f32, gamma: f32) -> f32 {\n" \
"  let c0 = L.hosek[base];\n" \
"  let c1 = L.hosek[base + 1];\n" \
"  let c2 = L.hosek[base + 2];\n" \
"  let expM = exp(c1.x * gamma);\n" \
"  let rayM = cosGamma * cosGamma;\n" \
"  let mieDen = 1.0 + c2.x * c2.x - 2.0 * c2.x * cosGamma;\n" \
"  var mieM = 0.0;\n" \
"  if (mieDen > 0.0) { mieM = (1.0 + rayM) / (mieDen * sqrt(mieDen)); }\n" \
"  let ct = max(cosTheta, 0.0);\n" \
"  let zen = sqrt(ct);\n" \
/* The +0.01 is the model's own horizon guard: cos(theta) reaches 0 there \
 * and the widening term would divide by zero. */ \
/* CLAMPED, and that clamp is load-bearing. The fit is defined for a sun and a \
 * view above the horizon; one hundredth below it this divisor reaches zero and \
 * the term overflows to +/-inf. The horizon blend below cannot mask that, \
 * because mix(a, b, 0) still evaluates a*(1-0) + b*0 and inf*0 is NaN — so an \
 * infinity generated in a direction that is supposed to contribute NOTHING \
 * poisoned the pixel anyway, zeroing whichever channels it hit. That is the \
 * red/yellow line one pixel tall that appeared at the anti-solar horizon around \
 * 05:55 and cleared by 06:10: the band is dir.z in (-0.01, 0), about half a \
 * degree, and which channels blow up depends on the cooked coefficients, hence \
 * red where two channels went and yellow where one did. */ \
"  let widening = 1.0 + c0.x * exp(c0.y / (ct + 0.01));\n" \
"  let body = c0.z + c0.w * expM + c1.y * rayM + c1.z * mieM + c1.w * zen;\n" \
"  return widening * body;\n" \
"}\n" \
"fn skyHosek(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {\n" \
"  let cosTheta = clamp(dir.z, -1.0, 1.0);\n" \
"  let cosGamma = clamp(dot(dir, toSun), -1.0, 1.0);\n" \
"  let gamma = acos(cosGamma);\n" \
"  let r = hosekChannel(0, cosTheta, cosGamma, gamma) * L.hosek[2].y;\n" \
"  let g = hosekChannel(3, cosTheta, cosGamma, gamma) * L.hosek[5].y;\n" \
"  let b = hosekChannel(6, cosTheta, cosGamma, gamma) * L.hosek[8].y;\n" \
/* Below the horizon the fit is not defined; fade to the ground-ish horizon \
 * colour rather than letting it diverge into the lower hemisphere. */ \
"  let below = smoothstep(0.0, 0.06, dir.z);\n" \
"  return mix(L.skyHorizon.rgb, max(vec3<f32>(r, g, b), vec3<f32>(0.0)), below);\n" \
"}\n" \
"fn skyColor(dir: vec3<f32>) -> vec3<f32> {\n" \
"  let kind = L.skyParams.x;\n" \
"  let toSun = normalize(-L.sunDir.xyz);\n" \
"  var c = L.clearColor.rgb;\n" \
"  if (kind > 1.5 && kind < 2.5) { c = skyProcedural(dir, toSun, L.skyParams.y); }\n" \
"  if (kind > 2.5 && kind < 3.5) { c = skyStylised(dir, toSun); }\n" \
"  if (kind > 3.5) { c = skyHosek(dir, toSun); }\n" \
/* The sun DISC, added on top for every kind that has a sky. Separate \
 * from the models above because it is ~1e5 times their radiance: folding \
 * it in would make the whole expression's dynamic range about the disc. \
 * A hard edge with one smoothstep of falloff, so it survives TAA. */ \
"  if (kind > 1.5) {\n" \
"    let ca = dot(dir, toSun);\n" \
"    let cutoff = cos(max(L.skyParams.w, 0.001));\n" \
"    let disc = smoothstep(cutoff, mix(cutoff, 1.0, 0.35), ca);\n" \
"    c = c + L.sunColor.rgb * disc * L.skyHorizon.w;\n" \
"  }\n" \
"  return c * L.skyParams.z;\n" \
"}\n"

#endif
