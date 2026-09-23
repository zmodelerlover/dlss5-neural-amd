#pragma once

// One package, all three APIs. The Vulkan transport and its device-creation hook are compiled
// in and cost nothing on a game that does not import Vulkan: the hook patches one import-table
// entry, and such a game has no entry to patch. Set this to 0 only to build a binary with the
// Vulkan code physically absent, which is a diagnostic, not a shipped variant.
#ifndef AMDNR_WITH_VULKAN
#define AMDNR_WITH_VULKAN 1
#endif

// The OpenGL transport, on the same terms. It costs even less than the Vulkan one in a process
// that is not an OpenGL host: there is no device to hook and no import to patch, because in
// OpenGL nothing has to be arranged before the context exists. The route resolves opengl32.dll by
// hand at the first present and does nothing at all until one arrives from a GL device.
#ifndef AMDNR_WITH_OPENGL
#define AMDNR_WITH_OPENGL 1
#endif
