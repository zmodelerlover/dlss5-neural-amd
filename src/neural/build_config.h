#pragma once

// The preview/master branch builds D3D11/D3D12; preview/vulkan also enables
// the Vulkan transport and its device-creation hook. The neural core is shared.
#ifndef DLSS5_WITH_VULKAN
#define DLSS5_WITH_VULKAN 1
#endif
