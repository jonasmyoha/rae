// The Hosek-Wilkie sky evaluation shared by the deferred light pass (a PART of
// lib/deferred_light.wgsl's composition). The legacy forward renderer keeps its
// own copy in compiler/runtime/runtime_sky_wgsl.h for the pipeline it builds in C.
fn preethamF(A: f32, B: f32, C: f32, D: f32, E: f32, ct: f32, g: f32) -> f32 {
  let c = max(ct, 0.01);
  let cg = cos(g);
  return (1.0 + A * exp(B / c)) * (1.0 + C * exp(D * g) + E * cg * cg);
}
fn skyProcedural(dir: vec3<f32>, toSun: vec3<f32>, t: f32) -> vec3<f32> {
  let g = acos(clamp(dot(dir, toSun), -1.0, 1.0));
  let ts = acos(clamp(max(toSun.z, 0.01), -1.0, 1.0));
  let Ay = 0.1787 * t - 1.4630; let By = -0.3554 * t + 0.4275;
  let Cy = -0.0227 * t + 5.3251; let Dy = 0.1206 * t - 2.5771;
  let Ey = -0.0670 * t + 0.3703;
  let Ax = -0.0193 * t - 0.2592; let Bx = -0.0665 * t + 0.0008;
  let Cx = -0.0004 * t + 0.2125; let Dx = -0.0641 * t - 0.8989;
  let Ex = -0.0033 * t + 0.0452;
  let Az = -0.0167 * t - 0.2608; let Bz = -0.0950 * t + 0.0092;
  let Cz = -0.0079 * t + 0.2102; let Dz = -0.0441 * t - 1.6537;
  let Ez = -0.0109 * t + 0.0529;
  let fy = preethamF(Ay,By,Cy,Dy,Ey, dir.z, g) / preethamF(Ay,By,Cy,Dy,Ey, 1.0, ts);
  let fx = preethamF(Ax,Bx,Cx,Dx,Ex, dir.z, g) / preethamF(Ax,Bx,Cx,Dx,Ex, 1.0, ts);
  let fz = preethamF(Az,Bz,Cz,Dz,Ez, dir.z, g) / preethamF(Az,Bz,Cz,Dz,Ez, 1.0, ts);
  let ts2 = ts * ts; let ts3 = ts2 * ts;
  let chi = (4.0 / 9.0 - t / 120.0) * (PI - 2.0 * ts);
  let zY = max((4.0453 * t - 4.9710) * tan(chi) - 0.2155 * t + 2.4192, 0.0);
  let zx = (0.00166*ts3 - 0.00375*ts2 + 0.00209*ts) * t * t
         + (-0.02903*ts3 + 0.06377*ts2 - 0.03202*ts + 0.00394) * t
         + (0.11693*ts3 - 0.21196*ts2 + 0.06052*ts + 0.25886);
  let zy = (0.00275*ts3 - 0.00610*ts2 + 0.00317*ts) * t * t
         + (-0.04214*ts3 + 0.08970*ts2 - 0.04153*ts + 0.00516) * t
         + (0.15346*ts3 - 0.26756*ts2 + 0.06670*ts + 0.26688);
  // zY is in kcd/m2; 0.05 is Sky.rae's skyUnitsPerKcd (keep them equal)
  let xx = zx * fx; let yy = max(zy * fz, 0.0001); let YY = zY * fy * 0.05;
  let X = xx * YY / yy; let Z = (1.0 - xx - yy) * YY / yy;
  let rgb = vec3<f32>( 3.2406*X - 1.5372*YY - 0.4986*Z,
                      -0.9689*X + 1.8758*YY + 0.0415*Z,
                       0.0557*X - 0.2040*YY + 1.0570*Z);
  return max(rgb, vec3<f32>(0.0));
}
fn skyStylised(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {
  let h = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);
  let bands = max(L.skyZenith.w, 1.0);
  let s = h * bands;
  let i = floor(s);
  let band = (i + smoothstep(0.35, 0.65, s - i)) / bands;
  var c = mix(L.skyHorizon.rgb, L.skyZenith.rgb, band);
  let g = clamp(dot(dir, toSun), 0.0, 1.0);
  c = c + L.sunColor.rgb * pow(g, 6.0) * 0.35;
  return c;
}
fn hosekChannel(base: i32, cosTheta: f32, cosGamma: f32, gamma: f32) -> f32 {
  let c0 = L.hosek[base];
  let c1 = L.hosek[base + 1];
  let c2 = L.hosek[base + 2];
  let expM = exp(c1.x * gamma);
  let rayM = cosGamma * cosGamma;
  let mieDen = 1.0 + c2.x * c2.x - 2.0 * c2.x * cosGamma;
  var mieM = 0.0;
  if (mieDen > 0.0) { mieM = (1.0 + rayM) / (mieDen * sqrt(mieDen)); }
  let ct = max(cosTheta, 0.0);
  let zen = sqrt(ct);
  let widening = 1.0 + c0.x * exp(c0.y / (ct + 0.01));
  let body = c0.z + c0.w * expM + c1.y * rayM + c1.z * mieM + c1.w * zen;
  return widening * body;
}
fn skyHosek(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {
  let cosTheta = clamp(dir.z, -1.0, 1.0);
  let cosGamma = clamp(dot(dir, toSun), -1.0, 1.0);
  let gamma = acos(cosGamma);
  let r = hosekChannel(0, cosTheta, cosGamma, gamma) * L.hosek[2].y;
  let g = hosekChannel(3, cosTheta, cosGamma, gamma) * L.hosek[5].y;
  let b = hosekChannel(6, cosTheta, cosGamma, gamma) * L.hosek[8].y;
  let below = smoothstep(0.0, 0.06, dir.z);
  return mix(L.skyHorizon.rgb, max(vec3<f32>(r, g, b), vec3<f32>(0.0)), below);
}
// The moon's disc at `dir`: a sphere lit on the side that faces the sun, so
// its phase is the geometry of the two directions (full when the sun is
// behind the viewer, dark when it is behind the moon), plus a faint earthshine
// on the dark side. 0 off the disc.
fn moonDisc(dir: vec3<f32>, toMoon: vec3<f32>, toSun: vec3<f32>, radius: f32) -> f32 {
  let ca = dot(dir, toMoon);
  let cutoff = cos(radius);
  if (ca < cutoff) { return 0.0; }
  let q = (dir - toMoon * ca) / max(sin(radius), 1e-5);
  let z = sqrt(max(1.0 - dot(q, q), 0.0));
  let normal = q - toMoon * z;
  let lit = smoothstep(-0.04, 0.12, dot(normal, toSun));
  let edge = smoothstep(cutoff, mix(cutoff, 1.0, 0.12), ca);
  // A little mottling: the maria are darker than the highlands
  let mare = 0.82 + 0.18 * sin(q.x * 9.0 + 1.3) * sin(q.y * 7.0 - 0.6);
  return edge * (lit * mare + 0.025);
}
fn skyColor(dir: vec3<f32>) -> vec3<f32> {
  let kind = L.skyParams.x;
  // The sky's own sun; a sky uploaded without one falls back to the key light
  var toSun = normalize(-L.sunDir.xyz);
  if (dot(L.skySun.xyz, L.skySun.xyz) > 0.25) { toSun = normalize(L.skySun.xyz); }
  var c = L.clearColor.rgb;
  if (kind > 1.5 && kind < 2.5) { c = skyProcedural(dir, toSun, L.skyParams.y); }
  if (kind > 2.5 && kind < 3.5) { c = skyStylised(dir, toSun); }
  if (kind > 3.5) { c = skyHosek(dir, toSun); }
  if (kind > 1.5) {
    let ca = dot(dir, toSun);
    let cutoff = cos(max(L.skyParams.w, 0.001));
    let disc = smoothstep(cutoff, mix(cutoff, 1.0, 0.35), ca);
    c = c + L.sunColor.rgb * disc * L.skyHorizon.w;
  }
  c = c * L.skyParams.z;
  // The moon and the night sky come after the daylight fade, so they show at
  // night: a dark blue night that brightens and glows round the moon with its
  // light, and the disc, faint by day against the bright sky
  if (L.moon.w > 0.0 && kind > 1.5) {
    let toMoon = normalize(L.moon.xyz);
    let above = smoothstep(-0.01, 0.03, dir.z);
    let night = 1.0 - clamp(L.skyParams.z, 0.0, 1.0);
    let moonlight = L.night.x;
    let height = clamp(dir.z, 0.0, 1.0);
    let nightSky = mix(vec3<f32>(0.020, 0.028, 0.050), vec3<f32>(0.006, 0.010, 0.026), height)
      * (0.35 + 2.4 * moonlight);
    let glow = pow(max(dot(dir, toMoon), 0.0), 48.0) * 0.12 * moonlight;
    c = c + (nightSky + vec3<f32>(0.55, 0.62, 0.80) * glow) * night * above;
    let disc = moonDisc(dir, toMoon, toSun, max(L.skySun.w, 0.001));
    c = c + vec3<f32>(0.92, 0.94, 1.0) * disc * L.moon.w * above;
  }
  return c;
}
