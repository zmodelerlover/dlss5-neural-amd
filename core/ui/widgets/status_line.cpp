#include "widgets.h"

#include "../i18n.h"
#include "../theme.h"
#include "../view_logic.h"

namespace ui {

// One line, beside the Enabled box, most serious state first. The bridge has more ways of not
// running than the add-on -- the engine is a second process -- so the first thing it answers there
// is whether there is anything on the other end at all.
void StatusLine(const PanelStatus &status, const PanelSettings &s)
{
    switch (status.run)
    {
    case RunState::Unavailable:
        ImGui::TextColored(kDanger, T("Unavailable: %s", "Indisponível: %s"), status.reason.c_str());
        return;
    case RunState::NoHelper:
        ImGui::TextColored(kDanger, "%s", T("no helper", "sem ajudante"));
        return;
    case RunState::Error:
        ImGui::TextColored(kDanger, "%s", T("Error -- see the log.", "Erro -- veja o log."));
        return;
    case RunState::TransportOnly:
        ImGui::TextColored(kWarn, "TRANSPORT_ONLY");
        return;
    case RunState::EngineNotReady:
        ImGui::TextColored(kWarn, "%s", T("engine not ready", "motor não pronto"));
        return;
    case RunState::Ready:
        break;
    }
    if (s.enabled == 0)
        ImGui::TextDisabled("%s", T("off", "desligado"));
    else if (status.processed == 0)
        ImGui::TextColored(kWarn, "%s", T("no frames yet", "nenhum quadro ainda"));
    else
        ImGui::TextColored(SkippedPercent(status) >= 10.0 ? kWarn : kRunning,
                           T("%llu frames, %.0f%% skipped", "%llu quadros, %.0f%% pulados"),
                           static_cast<unsigned long long>(status.processed), SkippedPercent(status));
}

} // namespace ui
