#pragma once
// The bridge's settings on the wire, and the panel's copy of them. Both are generated from
// settings_fields.inc, so each field has the same name and the same type on both sides and the copy
// is a plain assignment. Kept here rather than in src/ui/: the panel knows nothing about the pipe.
#include "bridge_ipc.h"
#include "../ui/panel_model.h"

namespace x86bridge {

static_assert(ui::kMaxPasses == 3, "WireSettings carries three passes");

inline ui::PanelSettings ToPanel(const WireSettings &w)
{
    ui::PanelSettings p;
#define X(type, name, low, high) p.name = w.name;
#include "settings_fields.inc"
#undef X
    for (unsigned i = 0; i < 3; ++i)
    {
        p.passOverride[i] = w.passOverride[i];
        p.passStructure[i] = w.passStructure[i];
        p.passTone[i] = w.passTone[i];
        p.passSkin[i] = w.passSkin[i];
    }
    return p;  // useFeedEffect stays 0: the flag does not cross the bridge
}

// Onto a copy of what was on the wire, so the revision and anything the panel does not carry are
// kept as they were.
inline WireSettings FromPanel(WireSettings w, const ui::PanelSettings &p)
{
#define X(type, name, low, high) w.name = p.name;
#include "settings_fields.inc"
#undef X
    for (unsigned i = 0; i < 3; ++i)
    {
        w.passOverride[i] = p.passOverride[i];
        w.passStructure[i] = p.passStructure[i];
        w.passTone[i] = p.passTone[i];
        w.passSkin[i] = p.passSkin[i];
    }
    return w;
}

} // namespace x86bridge
