#pragma once
// HLSL compute, embedded as a string and compiled at startup with D3DCompile.

namespace shaders {

// Optical flow, in three passes. The PS2 never computed per-pixel motion, so there is nothing to
// capture: this estimates it by comparing two frames, which is what the NVIDIA route does here too
// -- its motion comes from a shader (LumeniteFX), not from the game. Estimated motion is wrong in
// the places you would expect: reflections, fire, moving shadows, and anything newly revealed.
//
// Convention, matching the Feeder's: the vector points from where a pixel is now to where it was,
// so prev_uv = cur_uv + motion. Block matching produces exactly that sign, no flip needed.
inline constexpr char kLumaShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float extra; uint pad; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float2 uv = (float2(p.xy)+0.5)/float2(dw,dh);
 float3 c = src.SampleLevel(smp,uv,0).rgb;
 dst[p.xy] = dot(c, float3(0.299,0.587,0.114));
})";

// Search radius 4 with a 3x3 patch: 81 candidates times 9 taps. A wider window finds faster
// motion but costs the square of the radius, and this runs every frame inside the frame budget.
inline constexpr char kFlowShader[] = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prv : register(t1);
Texture2D<float2> guess : register(t2);
RWTexture2D<float2> dst : register(u0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint useGuess; float gate; float step; float ratio; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 int2 base = int2(p.xy);
 int2 lim = int2(dw-1, dh-1);
 int st = max(1, (int)step);
 // Nothing to track in a flat patch: every candidate matches equally well, so the winner is
 // whichever the loop happened to try first. That is the aperture problem, and it is why a wider
 // search made the field wilder instead of better. Reject on contrast before searching at all.
 float mn = 1e9, mx = -1e9;
 for (int cy=-1; cy<=1; ++cy) for (int cx=-1; cx<=1; ++cx) {
  float v = cur.Load(int3(clamp(base+int2(cx,cy), int2(0,0), lim), 0));
  mn = min(mn, v); mx = max(mx, v);
 }
 if (mx - mn < gate) { dst[p.xy] = float2(0,0); return; }

 int2 g0 = (useGuess != 0) ? int2(round(guess.Load(int3(p.xy,0)))) : int2(0,0);
 float best = 1e9; int2 bestD = g0; float zero = 0.0;
 for (int dy=-4; dy<=4; ++dy) for (int dx=-4; dx<=4; ++dx) {
  int2 d = g0 + int2(dx,dy) * st;
  float sad = 0.0;
  for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
   int2 a = clamp(base+int2(x,y), int2(0,0), lim);
   int2 b = clamp(base+d+int2(x,y), int2(0,0), lim);
   sad += abs(cur.Load(int3(a,0)) - prv.Load(int3(b,0)));
  }
  if (d.x==0 && d.y==0) zero = sad;
  if (sad < best) { best = sad; bestD = d; }
 }
 // Only on the coarse pass: reject a winner that barely beats standing still, which is what flat
 // sky and noise produce. The refine pass trusts the guess it was handed instead.
 // And a match has to explain the image clearly better than standing still, not marginally.
 if (useGuess == 0 && best > zero * ratio) bestD = int2(0,0);
 dst[p.xy] = float2(bestD);
})";

inline constexpr char kFlowUpShader[] = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float step; uint pad; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float2 uv = (float2(p.xy)+0.5)/float2(dw,dh);
 float2 v = src.SampleLevel(smp,uv,0);
 // mode 0: the estimator's field, already in coarse pixels, scaled to raster pixels by `step`.
 // mode 1: the game's own velocity buffer, which engines store as a UV-space delta, so it
 // becomes pixels by multiplying by the target size. `step` stays a plain multiplier there --
 // it carries the sign and any remaining convention difference. See MotionScale.
 dst[p.xy] = mode ? v * float2(dw,dh) * step : v * step;
})";

} // namespace shaders
