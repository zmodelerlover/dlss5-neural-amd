#pragma once
// HLSL compute, embedded as a string and compiled at startup with D3DCompile.

namespace shaders {

// Downscaling with a bilinear sampler reads four texels no matter how many the destination
// pixel actually covers. At 0.50 scale that throws away half the image and keeps its aliasing,
// and the network then spends its capacity denoising the aliasing instead of the picture --
// which is one of the reasons the result reads as a colour change rather than detail. Average
// the whole footprint instead. RenoDX exposes the same choice as its Downsample Filter.
inline constexpr char kCopyShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; };
float3 ToLinear(float3 c){ return c <= 0.04045 ? c/12.92 : pow(abs(c+0.055)/1.055, 2.4); }
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float4 c;
 if (sw > dw || sh > dh) {
  float2 r = float2(sw,sh)/float2(dw,dh);
  int2 a = int2(floor(float2(p.xy) * r));
  int2 b = min(int2(ceil(float2(p.xy+1) * r)) - 1, int2(sw-1,sh-1));
  b = min(b, a + 3);                       // 4x4 covers every scale down to 0.25
  float4 acc = 0; float n = 0;
  for (int y=a.y; y<=b.y; ++y) for (int x=a.x; x<=b.x; ++x) { acc += src.Load(int3(x,y,0)); n += 1; }
  c = acc / max(n, 1);
 } else {
  c = src.SampleLevel(smp, (float2(p.xy)+0.5)/float2(dw,dh), 0);
 }
 float3 v = c.rgb;
 if (mode != 0) v = ToLinear(saturate(v)) * k;
 dst[p.xy] = float4(v, c.a);
})";

inline constexpr char kDepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
SamplerState smp : register(s0);
cbuffer Extent : register(b0) { uint dw; uint dh; uint sw; uint sh; float dscale; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 uint2 t = min(uint2((float2(p.xy)+0.5)*float2(sw,sh)/float2(dw,dh)), uint2(sw-1,sh-1));
 // Scaled into the range the network was trained on. An emulator's projection can leave depth
 // occupying a fraction of a percent of [0,1] -- PCSX2 measures 0.002 -- and a buffer that flat
 // carries no geometry the network can use, however correct it is as depth. dscale is 1.0 for a
 // buffer that already fills the range, so this is a no-op everywhere it should be.
 dst[p.xy] = saturate(src.Load(int3(t,0)) * dscale);
})";

// The network runs at a fraction of the back buffer and only its *correction* comes back to
// full resolution -- same residual scheme RenoDX uses. Which filter carries that correction up
// decides how much of it survives: a bilinear stretch is a box blur at these ratios, so every
// high-frequency thing the network did was averaged away before it reached the screen and all
// that was left was the low-frequency part, i.e. colour and brightness. Catmull-Rom keeps the
// detail. It can ring on hard edges, so the bilinear path stays selectable, exactly like
// RenoDX's Upsample Filter. `pad` is packed: bit 0 is the filter (0 bilinear, 1 bicubic), bits
// 1+ are the debug view. The root signature is fixed at 8 constants and every shader shares that
// layout, so a new field rides in the spare word instead of widening all six.
//
// The debug views answer "is any of this reaching the screen", which a still frame cannot: the
// residual is a few percent of the signal, so at 1:1 it is invisible even when it is correct.
// Residual x8 shows the correction on its own against mid grey -- flat grey means the network
// returned its input, and structure means it did not.
// Subtracting the network's output from its input has to happen while both still describe the
// same frame, and compose is not always that moment. In async mode the engine works on its own
// timeline, so by the time compose runs the next frame has already overwritten netColour with
// fresh input and netBase with a copy of it -- the two are then identical and the correction is
// exactly zero. That is the whole of "turning Apply On Same Frame off makes the effect vanish".
// Capture the difference into its own texture at a point where the pair is known to match, and
// let compose read only that.
inline constexpr char kResidualShader[] = R"(
Texture2D<float4> nr   : register(t0);
Texture2D<float4> base : register(t1);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float extra; uint pad; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 dst[p.xy] = float4((nr.Load(int3(p.xy,0)) - base.Load(int3(p.xy,0))).rgb, 0);
})";

} // namespace shaders
