#pragma once

// One package, all three APIs. The Vulkan transport and its device-creation hook are compiled
// in and cost nothing on a game that does not import Vulkan: the hook patches one import-table
// entry, and such a game has no entry to patch. Set this to 0 only to build a binary with the
// Vulkan code physically absent, which is a diagnostic, not a shipped variant.
#ifndef DLSS5_WITH_VULKAN
#define DLSS5_WITH_VULKAN 1
#endif
