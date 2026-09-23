// hostcheck -- the smallest host each graphics API allows, presenting one fixed frame forever.
//
// framecheck proves the network pipeline without a host; this proves the transports, which only
// exist inside one. Run with ReShade beside the exe (dxgi.dll, d3d9.dll for D3D9, opengl32.dll
// for OpenGL; the Vulkan layer is registered system-wide), the add-on and the capture add-on in
// its add-on path. Built x86 (build.ps1 -Arch x86), the same host drives the 32-bit bridge:
// amd-nr.addon32 with its amd-nr-host64.exe. The capture add-on writes the composed back buffer
// to HOSTCHECK_OUT, and the host quits when that file appears.
//
//   amd-nr-hostcheck.exe d3d9|d3d11|d3d12|vulkan|opengl frame.ppm
//
// The window sits off every monitor and never takes focus: nothing shows on the desktop.
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace hostcheck {

struct Image
{
    UINT w = 0, h = 0;
    std::vector<unsigned char> rgba; // top to bottom
};

bool LoadPpm(const wchar_t *path, Image &img)
{
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    UINT maxValue = 0;
    in >> magic >> img.w >> img.h >> maxValue;
    if (!in || magic != "P6" || maxValue != 255 || in.get() != '\n')
        return false;
    std::vector<unsigned char> rgb(static_cast<size_t>(img.w) * img.h * 3);
    in.read(reinterpret_cast<char *>(rgb.data()), rgb.size());
    if (static_cast<size_t>(in.gcount()) != rgb.size())
        return false;
    img.rgba.resize(static_cast<size_t>(img.w) * img.h * 4);
    for (size_t i = 0, n = static_cast<size_t>(img.w) * img.h; i < n; ++i) {
        std::memcpy(&img.rgba[i * 4], &rgb[i * 3], 3);
        img.rgba[i * 4 + 3] = 255;
    }
    return true;
}

HWND MakeWindow(UINT w, UINT h, bool ownDc)
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hostcheck";
    wc.style = ownDc ? CS_OWNDC : 0;
    RegisterClassW(&wc);
    RECT r { 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    AdjustWindowRect(&r, WS_POPUP, FALSE);
    // Far off to the left of any monitor arrangement.
    HWND wnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"hostcheck",
                               WS_POPUP, -32000, -32000, r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    return wnd;
}

// Presents until the capture exists, or gives up after a minute and a half.
int Loop(const std::function<bool()> &present)
{
    const char *out = std::getenv("HOSTCHECK_OUT");
    const ULONGLONG deadline = GetTickCount64() + 90000;
    while (GetTickCount64() < deadline) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        if (!present()) {
            std::fprintf(stderr, "present failed\n");
            return 3;
        }
        if (out != nullptr && GetFileAttributesA((std::string(out) + ".txt").c_str()) !=
                                  INVALID_FILE_ATTRIBUTES)
            return 0;
    }
    std::fprintf(stderr, "no capture within a minute and a half\n");
    return 4;
}

int RunD3D9(const Image &img);
int RunD3D11(const Image &img);
int RunD3D12(const Image &img);
int RunVulkan(const Image &img);
int RunOpenGL(const Image &img);

} // namespace hostcheck

#include "host_d3d9.inc"
#include "host_d3d11.inc"
#include "host_d3d12.inc"
#include "host_opengl.inc"
#include "host_vulkan.inc"

int wmain(int argc, wchar_t **argv)
{
    using namespace hostcheck;
    Image img;
    if (argc != 3 || !LoadPpm(argv[2], img)) {
        std::fprintf(stderr, "usage: amd-nr-hostcheck.exe d3d9|d3d11|d3d12|vulkan|opengl frame.ppm\n");
        return 2;
    }
    const std::wstring api = argv[1];
    const int result = api == L"d3d9"     ? RunD3D9(img)
                       : api == L"d3d11"  ? RunD3D11(img)
                       : api == L"d3d12"  ? RunD3D12(img)
                       : api == L"vulkan" ? RunVulkan(img)
                       : api == L"opengl" ? RunOpenGL(img)
                                          : 2;
    std::fflush(nullptr);
    // As in framecheck: the runtime has no shutdown API and its HIP worker may still hold
    // references, so let the OS release everything together instead of running DLL detach.
    TerminateProcess(GetCurrentProcess(), result);
    return result;
}
