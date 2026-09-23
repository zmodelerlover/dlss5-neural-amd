#pragma once
// HLSL compute, embedded as a string and compiled at startup with D3DCompile.

namespace shaders {

// D3D11 compute that reads a depth-stencil as a single float and writes plain R32_FLOAT. The
// conversion has to happen on this side: R32G8X24_TYPELESS and R24G8_TYPELESS do not open on a
// second device, while R32_FLOAT does. Verbatim from session.cpp, where it is measured working.
inline constexpr char kGuideDepthCs[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
 uint w, h; dst.GetDimensions(w, h);
 if (p.x >= w || p.y >= h) return;
 dst[p.xy] = src.Load(int3(p.xy, 0));
})";

} // namespace shaders
