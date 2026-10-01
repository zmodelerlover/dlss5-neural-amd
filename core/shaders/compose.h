#pragma once
// HLSL compute, embedded as a string and compiled at startup with D3DCompile.

namespace shaders {

inline constexpr char kComposeShader[] = R"(
Texture2D<float4> full : register(t0);
Texture2D<float4> res  : register(t1);
Texture2D<float4> dbgs : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; float limit; float fade; float colour; float guard; };
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float3 ToLinear(float3 c){ return c <= 0.04045 ? c/12.92 : pow(abs(c+0.055)/1.055, 2.4); }
float3 ToSrgb(float3 c){ return c <= 0.0031308 ? c*12.92 : 1.055*pow(abs(c), 1.0/2.4) - 0.055; }
// The largest fraction of the correction that leaves every channel inside [0,1], applied to the
// whole triple at once. Clamping per channel instead is a hue rotation -- whichever channel hits
// the wall first decides the colour of the rest -- and that rotation is what a big correction
// showed up as: blown, wrong-coloured pixels rather than a stronger version of the same picture.
// Exactly nothing when the sum was already representable. hhkbble's cube scaling, from the
// multi-pass work on the OptiScaler DLSS-NR fork.
float3 CubeScale(float3 p, float3 t){
 float3 d = t - p;
 float a = 1.0;
 [unroll] for (int i = 0; i < 3; ++i) {
  if (d[i] > 1e-6) a = min(a, (1.0 - p[i]) / d[i]);
  else if (d[i] < -1e-6) a = min(a, (0.0 - p[i]) / d[i]);
 }
 return p + saturate(a) * d;
}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float2 uv = (float2(p.xy)+0.5)/float2(dw,dh);
 float3 fix;
 if ((pad & 1) != 0) {
  float2 f = uv * float2(sw,sh) - 0.5;
  int2 i0 = int2(floor(f));
  float2 t = f - float2(i0), t2 = t*t, t3 = t2*t;
  float wx[4] = { -0.5*t3.x + t2.x - 0.5*t.x,  1.5*t3.x - 2.5*t2.x + 1.0,
                  -1.5*t3.x + 2.0*t2.x + 0.5*t.x,  0.5*t3.x - 0.5*t2.x };
  float wy[4] = { -0.5*t3.y + t2.y - 0.5*t.y,  1.5*t3.y - 2.5*t2.y + 1.0,
                  -1.5*t3.y + 2.0*t2.y + 0.5*t.y,  0.5*t3.y - 0.5*t2.y };
  fix = 0;
  [unroll] for (int y=0; y<4; ++y) [unroll] for (int x=0; x<4; ++x) {
   int2 c4 = clamp(i0 + int2(x-1,y-1), int2(0,0), int2(sw-1,sh-1));
   fix += res.Load(int3(c4,0)).rgb * (wx[x]*wy[y]);
  }
 } else {
  fix = res.SampleLevel(smp,uv,0).rgb;
 }
 // Intensity up to 1 is a blend towards the network's picture, which is what scaling the residual
 // is. Above 1 the reference (RenoDX's composition, as the OptiScaler DLSS-NR fork carries it)
 // does not extrapolate the residual -- a lerp past its target walks the channels apart faster
 // than the luminance, and the guard cannot pull them back -- it raises the luminance ratio to a
 // power instead, further down, where the guard still binds it. The additive path keeps the old
 // meaning because it has no ratio to amplify.
 fix *= (guard <= 0.0) ? intensity : min(intensity, 1.0);
 // Normalised before anything is decided about it, so "how big is this correction" means the
 // same thing whatever the encoding is: 1.0 is white. The correction arrives in the network's
 // own space, which is the frame's linear light times the Diffuse White scale, and a limit
 // expressed in that space would mean something different at 100 nits and at 1000.
 fix /= (mode != 0) ? max(k, 1e-6) : 1.0;
 // The network works in tiles, and a tile where it extrapolated rather than saw returns a
 // correction nothing like the few percent it returns everywhere else -- one measured run came
 // back with a mean of 0.072 and a **maximum of 4.16**, in a picture whose own mean is 0.13.
 // That is where the blown blocks come from, and every extra pass runs on top of the last one's
 // blown block, so it compounds rather than averaging out.
 //
 // Scaled as a whole triple rather than clamped per channel. A per-channel clamp on an outlier
 // is a hue rotation -- the same failure the composition below exists to avoid -- so it turned
 // a blown block into a blown *coloured* block. Here the direction of the correction survives
 // and only its size gives way, and a correction already inside the limit is untouched.
 if (limit > 0.0) {
  float mag = max(abs(fix.r), max(abs(fix.g), abs(fix.b)));
  if (mag > limit) fix *= limit / mag;
 }
 // The tiles at the frame border have no neighbour on one side, and bicubic upsampling rings on
 // top of that. fade rolls the correction off over a band, using the smaller of the two
 // distances, so a corner gets both rolloffs and lands hardest.
 if (fade > 0.0) { float2 e = min(uv, 1.0 - uv) / fade; fix *= saturate(min(e.x, e.y)); }
 uint dbg = pad >> 1;
 if (dbg != 0) {
  float3 d;
  if      (dbg == 3) d = 0.5 + fix * 8.0;                     // the correction, on its own
  else if (dbg == 4) {                                        // motion, in raster pixels
   float2 m = dbgs.SampleLevel(smp,uv,0).xy;
   d = float3(0.5 + m.x/8.0, 0.5 + m.y/8.0, 0.5);   // full red or green at 8 px
  }
  else if (dbg == 5) d = dbgs.SampleLevel(smp,uv,0).xxx * max(intensity,1e-3);
  else               d = dbgs.SampleLevel(smp,uv,0).rgb;      // 1 input / 2 output / 6 network output mode
  d = saturate(d);
  dst[p.xy] = float4(d, 1.0);
  return;
 }
 float3 c = full.Load(int3(p.xy,0)).rgb;
 // The picture the network was shown, rebuilt here instead of read back. The copy shader is a
 // pure function of the pixel, so this reproduces it exactly and at full resolution -- which
 // means the only thing carried up from the reduced raster is the correction itself, and the two
 // pictures being compared below are at the same scale. Everything from here on works in that
 // normalised space, where 1.0 is white, so the encoding drops out of the arithmetic.
 float3 P = (mode != 0) ? ToLinear(saturate(c)) : c;
 float3 E = fix;
 float3 v;
 if (guard <= 0.0) {
  // Additive. What this add-on did until now, kept so the two can be compared in one session:
  // the correction is added per channel and whatever leaves the cube is clipped per channel.
  v = P + E;
 } else {
  // Ratio composition, after the OptiScaler DLSS-NR fork and RenoDX's DLSS 5 addon before it.
  //
  // The point is that nothing here adds a colour difference to a picture. The network's answer is
  // made into a whole picture of its own (M), its luminance is compared against the frame's as a
  // ratio, that ratio is bounded, and the two well-formed pictures are blended. A bounded ratio
  // cannot move hue; a difference can, and with more than one pass it did -- N passes meant N
  // times the difference, which clipped, and a clipped channel is a hue rotation. That is what
  // "3 passes looks deep fried" is.
  float3 M = CubeScale(P, P + E);
  float pl = dot(P, kLuma), ml = dot(M, kLuma);
  // A ratio against a near-black pixel is unbounded, and clamping it is not the same as taming
  // it: a shadow pixel sits around a thousandth, so a tiny edit becomes an enormous ratio, hits
  // the guard, and that pixel boils frame to frame. The same floor on both sides leaves bright
  // pixels alone -- there it vanishes against the luminance -- and lets the ratio fall smoothly
  // to one as the light goes out. No edit at all is the right answer for a pixel with no light.
  const float floorY = 1.0 / 512.0;
  float ratio = (ml + floorY) / (pl + floorY);
  // Two-sided, and one scalar taken from luminance applied to the whole triple. A per-channel
  // bound would be the hue distorter this whole path exists to avoid.
  //
  // Intensity above 1 lands here, as the reference does it: the ratio is raised to a power, which
  // cannot go negative, leaves an unchanged pixel unchanged (one to any power is one), moves
  // brightening and darkening by the same factor, and is still a ratio, so the guard bounds it.
  // At intensity 1 the exponent is 1 and this line is what it was.
  float bounded = clamp(pow(max(ratio, 1e-6), 1.0 + max(intensity - 1.0, 0.0)), 1.0 / guard, guard);
  // The network can hand back an empty picture for an input it could not read. Rescaling that
  // collapses the frame to black -- and it is the one case where the two ends of the blend below
  // do not share a luminance -- so the frame goes through on its own brightness instead.
  float3 lit = ml > 1e-5 ? M * (bounded / max(ratio, 1e-6)) : P * bounded;
  // Both ends of this blend now carry the same luminance and differ only in chroma, so Colour
  // Strength moves colour and nothing else. 0 keeps the game's own hue exactly, with only the
  // light carrying what the network decided; 1 brings the network's colour with it.
  v = lerp(P * bounded, lit, saturate(colour));
  // A last scale rather than a clip, for the same reason as CubeScale: bounded can be larger
  // than ratio, so the scaling above can push a channel past 1 again.
  float peak = max(v.r, max(v.g, v.b));
  if (peak > 1.0) v /= peak;
  v = max(v, 0.0);
 }
 float3 outc = (mode != 0) ? ToSrgb(saturate(v)) : saturate(v);
 dst[p.xy] = float4(outc, 1.0);
})";

} // namespace shaders
