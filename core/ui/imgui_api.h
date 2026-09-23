#pragma once
// ReShade's ImGui for an add-on: imgui.h declares the API, reshade_overlay.hpp defines it as calls
// through the function table ReShade hands over in register_addon. The table pointer is a static
// inside an inline function, so every translation unit of one module shares the one instance --
// the files in core/ui/ can be compiled separately and still draw into the overlay ReShade set up.
// No windows.h on this path, which is what lets the panel be syntax-checked on any compiler.
#include <imgui.h>

// reshade_overlay.hpp has no include guard, and reshade.hpp includes it. A file that has already
// included reshade.hpp -- the add-on and the bridge both do, first thing -- already has it, and a
// second copy redefines every function in it. So: reshade.hpp before this header, or not at all.
#ifndef RESHADE_API_VERSION
#include <reshade_overlay.hpp>
#endif
