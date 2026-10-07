#pragma once
// The DXGI factory, resolved where it is first wanted rather than imported. The same reason as
// d3d12.dll's in neural.cpp (LoadGraphicsApi): an import is resolved when ReShade loads the add-on,
// in the middle of ReShade's own start-up, and the add-on then decides when dxgi.dll arrives in a
// game that has not loaded it yet. Dragon Age: Inquisition, ReShade as d3d11.dll, quit at start with
// DXGI_ERROR_INVALID_CALL and wrote no amd-nr.log while the add-on imported CreateDXGIFactory1 and 2
// (issue #22), as NFS 2015 did over d3d12.dll. Only the OpenGL and Vulkan routes and the optical
// flow (ffx_d3d12_module.h points the SDK's CreateDXGIFactory2 here) ask for a factory, and by then
// the game has dxgi.dll loaded or has no D3D at all, so this is a reference count.
//
// CreateDXGIFactory2 with no flags is CreateDXGIFactory1 (Windows 8.1 and later; D3D12 needs 10).
// tools/import_table_check.py fails the build's add-on if dxgi.dll is back in its import table.
#include <windows.h>

inline HRESULT DxgiCreateFactory(UINT flags, REFIID riid, void **out)
{
    using Create = HRESULT(WINAPI *)(UINT, REFIID, void **);
    static const Create create = [] {
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (dxgi == nullptr)
            dxgi = LoadLibraryW(L"dxgi.dll");
        return dxgi == nullptr ? nullptr
                               : reinterpret_cast<Create>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
    }();
    *out = nullptr;
    return create != nullptr ? create(flags, riid, out) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
