#pragma once
// Toggle-shortcut capture, shared by the 64-bit add-on and the 32-bit bridge.
//
// The key state has to come from ReShade -- effect_runtime::is_key_down -- and never from
// GetAsyncKeyState. ReShade hooks GetAsyncKeyState and answers 0 for every virtual key from 8 up
// while the overlay is blocking keyboard input, which is exactly while this panel is on screen
// (reshade/source/input_windows.cpp):
//
//     if ((vKey & 0xF8) != 0) { if (is_blocking_any_keyboard_input()) return 0; }
//
// So a scan built on GetAsyncKeyState reads an empty keyboard for as long as the user can see the
// button they just clicked. That is what "the hotkey cannot be changed" was: armed, and then
// nothing at all, for ever. Measured in Half-Life 2, 15/09/2026.
#include <Windows.h>

#include <cstdio>
#include <string>

namespace hotkey {

// A modifier is part of a binding, never the binding itself.
inline bool IsModifier(int vk)
{
    return vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LSHIFT || vk == VK_RSHIFT;
}

// Armed by the button in the overlay, then polled with ReShade's key state until a key arrives.
struct Capture
{
    bool armed = false;

    // Set once every non-modifier key has been released after arming. Without it the key that
    // opened the overlay -- still held at the moment the button is clicked -- is the one that gets
    // bound, and ReShade's own overlay key becomes the effect toggle.
    bool ready = false;

    void Arm() { armed = true; ready = false; }
    void Cancel() { armed = false; }
    void Toggle() { if (armed) Cancel(); else Arm(); }

    // down(vk) is ReShade's key state. Returns true exactly once, with the new binding, when one
    // is made. Escape disarms and keeps the current binding, so it returns false.
    template <class Down>
    bool Poll(Down down, int &key, int &mods)
    {
        if (!armed)
            return false;

        // Scanning from 0x08 leaves out the mouse buttons, which are what clicked the button.
        int held = 0;
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (!IsModifier(vk) && down(vk))
            {
                held = vk;
                break;
            }

        if (!ready)
        {
            ready = held == 0;
            return false;
        }
        if (held == 0)
            return false;

        armed = false;
        if (held == VK_ESCAPE)
            return false;

        key = held;
        mods = (down(VK_CONTROL) ? 1 : 0) | (down(VK_MENU) ? 2 : 0) | (down(VK_SHIFT) ? 4 : 0);
        return true;
    }
};

// The bound key, spelled the way the user's keyboard layout spells it.
//
// Win32 already knows every key's localised name, so there is no table here to fall out of date
// or to be wrong on a non-US layout. The one trap is the extended-key bit: without it
// GetKeyNameText answers with the numpad twin, and End prints as "Num 1".
inline std::string Name(int key, int mods)
{
    std::string out;
    if (mods & 1) out += "Ctrl+";
    if (mods & 2) out += "Alt+";
    if (mods & 4) out += "Shift+";
    if (key == 0)
        return out.empty() ? "unbound" : out + "?";

    UINT sc = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    switch (key)
    {
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 0x100;
        break;
    default:
        break;
    }
    wchar_t wide[64] {};
    if (sc != 0 && GetKeyNameTextW(static_cast<LONG>(sc) << 16, wide, 64) > 0)
    {
        char name[128] {};
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), nullptr, nullptr) > 0)
            return out + name;
    }
    char fallback[16] {};
    std::snprintf(fallback, sizeof(fallback), "VK 0x%02X", key);
    return out + fallback;
}

} // namespace hotkey
