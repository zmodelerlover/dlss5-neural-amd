#include "panel.h"

#include "i18n.h"
#include "imgui_api.h"
#include "sections/sections.h"

namespace ui {

// Written for a NARROW panel, because that is how this is used: the overlay is kept thin so the
// game stays visible behind it. Three consequences, and they are the whole layout rule here:
//
//   * Labels are short. A checkbox label is not wrapped or clipped by ImGui, it simply runs off
//     the right edge, so "Ligado desde o primeiro quadro" is a horizontal overflow waiting for a
//     thin panel. What the control means goes in its (?), which has room.
//   * No PushItemWidth. ImGui's default is 65% of the window, which tracks the width on its own;
//     any fixed number of ems is right at one width and wrong at every other.
//   * Anything that is a sentence goes through TextWrapped. ImGui::Text does not wrap.
//
// ponytail: no custom style, no indent, no section wrapper. ReShade's own look, which is what
// every other add-on in the same overlay uses, and one less thing to be wrong on a light theme.
PanelActions DrawPanel(PanelSettings &settings, const PanelStatus &status)
{
    PanelActions actions;
    SetLanguage(settings.language);
    DrawGeneral(settings, status, actions);
    // Everything below needs the engine. On the bridge in transport-only mode the helper is copying
    // the frame across and back with no network at all, and a control that cannot reach anything
    // should not look as if it can.
    ImGui::BeginDisabled(status.transportOnly);
    DrawPerformance(settings, status, actions);
    DrawImage(settings);
    DrawDebug(settings, status, actions);
    DrawExperimental(settings, status);
    DrawGuides(settings, status);
    DrawEngine(settings, status);
    ImGui::EndDisabled();
    DrawMoreSettings(settings, status);
    DrawFooter(status, actions);
    return actions;
}

} // namespace ui
