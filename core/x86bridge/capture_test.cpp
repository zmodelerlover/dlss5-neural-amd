// The hotkey capture state machine, driven against a keyboard this test holds itself.
//
// Both defects this replaced are asserted here: the overlay key binding itself because the scan
// started while it was still held, and a modifier alone being taken for a binding.
#include "../shared/hotkey_capture.h"

#include <cassert>
#include <cstdio>
#include <set>

int main()
{
    std::set<int> down;  // the keyboard, as ReShade would report it
    auto key = [&](int vk) { return down.count(vk) != 0; };

    hotkey::Capture capture;
    int bound = 0, mods = 0;

    // The key that opened the overlay is still held when the button is clicked. Nothing may be
    // bound until the keyboard is empty.
    down.insert(VK_HOME);
    capture.Arm();
    assert(!capture.Poll(key, bound, mods));
    assert(!capture.Poll(key, bound, mods));
    assert(capture.armed && !capture.ready);

    down.erase(VK_HOME);
    assert(!capture.Poll(key, bound, mods));
    assert(capture.ready);

    // Ctrl+F9: the modifiers held at that moment are part of the binding.
    down.insert(VK_CONTROL);
    down.insert(VK_F9);
    assert(capture.Poll(key, bound, mods));
    assert(bound == VK_F9 && mods == 1);
    assert(!capture.armed);

    // Disarmed: the next keypress belongs to the game.
    down.clear();
    down.insert(VK_F10);
    assert(!capture.Poll(key, bound, mods));
    assert(bound == VK_F9 && mods == 1);

    // A modifier on its own is never a binding, and does not disarm.
    down.clear();
    capture.Arm();
    assert(!capture.Poll(key, bound, mods));
    down.insert(VK_SHIFT);
    assert(!capture.Poll(key, bound, mods));
    assert(capture.armed);

    // Escape disarms and leaves the current binding alone.
    down.clear();
    assert(!capture.Poll(key, bound, mods));
    down.insert(VK_ESCAPE);
    assert(!capture.Poll(key, bound, mods));
    assert(!capture.armed && bound == VK_F9 && mods == 1);

    std::puts("PASS hotkey capture waits for release, keeps modifiers, and cancels on Esc");
}
