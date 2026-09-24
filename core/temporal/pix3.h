#pragma once
// build.ps1 -Ffx only. The FidelityFX DX12 backend includes <pix3.h> for PIX_COLOR alone; the PIX
// entry points it loads at run time, so the WinPixEventRuntime package is not needed to build.
#ifndef PIX_COLOR
#define PIX_COLOR(red, green, blue) \
    ((UINT64)(0xff000000ull | ((UINT64)(red) << 16) | ((UINT64)(green) << 8) | (UINT64)(blue)))
#endif
