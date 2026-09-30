#pragma once
// Force-included into the FidelityFX SDK's sources alone (build.ps1 -Ffx), never into the add-on's.
//
// The SDK's DX12 backend serializes every root signature through GetModuleHandleW(L"D3D12.dll"). On a
// D3D11 game the add-on brings D3D12 in privately as dx12p.dll (LoadPrivateD3D12 in neural.cpp), so no
// module has that name, the serialization fails, and the optical flow's createPipelineStates drops the
// error: the context comes up with null pipelines and its first dispatch calls SetPipelineState(null)
// inside D3D12Core -- the crash three players sent from D3D11 games. Here the SDK's "D3D12.dll" is the
// private copy whenever it is loaded, the module the add-on's device came from; anything else it asks
// for goes to the real GetModuleHandleW.
#include <windows.h>

static HMODULE AmdNrFfxGetModuleHandleW(LPCWSTR name)
{
    if (name != nullptr && _wcsicmp(name, L"D3D12.dll") == 0)
        if (HMODULE own = GetModuleHandleW(L"dx12p.dll"))  // kPrivateD3D12 in neural.cpp
            return own;
    return GetModuleHandleW(name);
}
#define GetModuleHandleW AmdNrFfxGetModuleHandleW
