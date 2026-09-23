#pragma once
// HLSL compute, embedded as a string and compiled at startup with D3DCompile.

namespace shaders {

inline constexpr char kComposeShader[] = R"(
Texture2D<float4> full : register(t0);
Texture2D<float4> res  : register(t1);
Texture2D<float4> dbgs : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; float limit; float fade; float colour; float guard; float gExp; float gCon; float gSat; };
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
// Neural Rendering Model B and Model C, as cg2r_post_process_kernel applies them. Not an
// impression of them: the three coefficients are read out of the descriptor table in
// nvngx_dlssnr.dll and the operations are the ones its SASS performs, in its order. The full
// derivation, with the disassembly, is in docs/styles-model-abc.md.
//
// Display-referred on purpose. Every step in the kernel ends in an FFMA.SAT, so the chain runs
// on values already clamped to [0,1] -- which is the frame as it will be shown, not the linear
// light the composition above works in.
float3 NeuralStyle(float3 c)
{
 // opts[75]: exposure in stops. MUFU.EX2 on the coefficient, then a saturating multiply.
 c = saturate(c * exp2(gExp));
 // opts[77]: contrast, as a blend towards a smoothstep S-curve. The kernel builds x*x*(3-2x)
 // and adds k times the difference, so a negative coefficient flattens rather than steepens.
 // The clamp is the kernel's own FADD.FTZ.SAT on the result, not a tidy-up: it is what the
 // saturation step below is handed, and a positive coefficient can overshoot [0,1] here.
 c = saturate(c + gCon * (c * c * (3.0 - 2.0 * c) - c));
 // opts[78]: saturation, as a multiply on HSL's S -- not HSV's. The kernel decomposes to HSL:
 // L = (max+min)/2, and S = d/(max+min) at or below the midpoint, d/(2-max-min) above it
 // (SASS 3ca0-3db0: FADD max+min, FMUL 0.5, then the two reciprocals under FSETP L > 0.5).
 // It scales S, then rebuilds through q = L<0.5 ? L(1+S) : L+S-L*S and p = 2L-q, which is
 // textbook HSL->RGB.
 //
 // Written here in the closed form of that round trip. Holding H and L while S scales moves
 // every channel along the line through L, because d = 2*S*min(L, 1-L) on both sides, so the
 // rebuilt channel is c_i' = L + (S'/S)(c_i - L) and the hue sectors cancel exactly.
 //
 // The saturate on S is the kernel's, and it is why this is not just a lerp: a coefficient
 // above zero can drive S past 1, and there it has to stop.
 //
 // HSV would anchor on max instead of L, which holds the brightest channel still and only
 // lifts the others. HSL pulls both ends towards L, so a bright saturated colour also loses
 // some of its peak. That is a visible difference on exactly the colours Models B and C are
 // there to touch, and it was HSV here until the kernel was read for it.
 float M = max(c.r, max(c.g, c.b));
 float m = min(c.r, min(c.g, c.b));
 float d = M - m;
 // The kernel's own guard is `max > min`: a grey has no hue to preserve and no saturation to
 // scale, and it is the only case where the reciprocal below would not exist.
 if (d > 1e-6) {
  float L = (M + m) * 0.5;
  float S = d / ((L > 0.5) ? (2.0 - M - m) : (M + m));
  c = L + (saturate(S * (1.0 + gSat)) / S) * (c - L);
 }
 return saturate(c);
}
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
  // Network Output is a picture, not a diagnostic: the Model's grade belongs on it, as it does on
  // the composed frame. The debug views stay ungraded so they keep showing what the buffer holds.
  if (dbg == 6 && (gExp != 0.0 || gCon != 0.0 || gSat != 0.0)) d = NeuralStyle(d);
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
 // Applied last, to the frame as it will be shown, and only when a style is selected -- with
 // Style=0 the coefficients are all zero and every operation above is its own identity, so
 // this is skipped rather than run to no effect.
 // No flag bit. Above bit 0 `pad` is the debug view here, and a bit taken from it would
 // silently switch the picture to a debug draw. The coefficients answer it themselves:
 // all three are exactly zero for Model A and for strength 0. That is also the question
 // NVIDIA's own sub_1800176E0 asks before it records this pass at all.
 if (gExp != 0.0 || gCon != 0.0 || gSat != 0.0) outc = NeuralStyle(outc);
 dst[p.xy] = float4(outc, 1.0);
})";

} // namespace shaders
