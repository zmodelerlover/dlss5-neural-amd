#pragma once
// The panel's reusable pieces. Free functions that take only what they draw: in immediate-mode
// ImGui the function that draws is the component, so there is no base class and no registry.

#include "../imgui_api.h"
#include "../panel_model.h"

namespace ui {

// Anything that makes one evaluation slower is a driver-reset risk while the game is waiting on
// the GPU for it, so the warning goes next to the control rather than in a readme.
void Note(const ImVec4 &colour, const char *text);

// A section header in its own hue (theme.h).
bool SectionHeader(const ImVec4 &hue, const char *title, bool defaultOpen = false);

// The (?) after a control, with its explanation behind it.
void Help(const char *en, const char *pt);

// One line of the status column, right-aligned opposite the control to its left.
void RightLine(const ImVec4 *colour, const char *fmt, ...);

// The one line beside Enabled.
void StatusLine(const PanelStatus &status, const PanelSettings &s);

// The rebind button and its label. Returns true when it was clicked, which arms or cancels a
// capture the adapter owns: the scan needs ReShade's key state, and on the bridge it has to run
// on the present path, not here.
bool HotkeyButton(const PanelStatus &status);

// Paints a control red or amber while its CURRENT VALUE is one that has caused trouble. Scoped
// so it can be declared in an if-init and still wrap the widget:
//     if (Risk r(kDanger, cond); ImGui::SliderFloat(...))
// ImGui draws a widget's label with ImGuiCol_Text, so pushing the colour colours the control.
//
// This is the only marking left on a control. The MEASURED / TRACED / UNKNOWN / INERT tags that
// used to follow every label are gone: they were provenance, which belongs in the handoffs and
// in the comments here, and they cost eight to twelve characters on every row of a panel that
// has to stay narrow. Red and amber stay because they are about the value in front of you rather
// than about how the control came to be known.
struct Risk
{
    bool on;
    Risk(const ImVec4 &colour, bool active) : on(active)
    {
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
    }
    ~Risk()
    {
        if (on)
            ImGui::PopStyleColor();
    }
    Risk(const Risk &) = delete;
    Risk &operator=(const Risk &) = delete;
};

} // namespace ui
