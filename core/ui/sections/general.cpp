#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../view_logic.h"
#include "../widgets/widgets.h"

namespace ui {

namespace {

void Switches(PanelSettings &s, const PanelStatus &status)
{
    bool on = s.enabled != 0;
    if (ImGui::Checkbox(T("Enabled", "Ligado"), &on))
        s.enabled = on ? 1u : 0u;
    ImGui::SameLine();
    StatusLine(status, s);

    bool start = s.startOn != 0;
    if (ImGui::Checkbox(T("On at startup", "Ligar ao abrir o jogo"), &start))
        s.startOn = start ? 1u : 0u;
    Help("Whether Enabled is already ticked when the game opens, instead of waiting for the "
         "hotkey every time.",
         "Se o Ligado já vem marcado quando o jogo abre, em vez de esperar a tecla de atalho "
         "toda vez.");

    bool altTab = s.disableOnAltTab != 0;
    if (ImGui::Checkbox(T("Off on alt-tab", "Desligar no alt-tab"), &altTab))
        s.disableOnAltTab = altTab ? 1u : 0u;
    Help("Switches the effect off when the game stops being the window in front, and leaves it "
         "off -- turn it back on with the hotkey. A minimised window is always sat out, "
         "separately, and that one does resume on its own.",

         "Desliga o efeito quando o jogo deixa de ser a janela da frente, e deixa desligado -- "
         "religue na tecla de atalho. Janela minimizada é outra coisa: sempre pulada, e essa "
         "volta sozinha.");
}

// The status column. It used to be a collapsing section of its own, which spent a header and a
// click on five lines of text that never need either -- and which, being at the bottom, reported
// the skip rate somewhere you would only look after you already suspected it.
//
// Now it runs down the right edge, opposite the switches, in vertical space those rows already
// occupy. Reading order is the point: the left column is what you change, the right column is what
// happened, and the two are side by side.
void StatusColumn(const PanelStatus &status)
{
    const bool skipping = Skipping(status);
    RightLine(skipping ? &kWarn : nullptr, "%s", status.routeNote.c_str());
    if (status.outWidth != 0)
        RightLine(nullptr, T("%ux%u to %ux%u", "%ux%u para %ux%u"), status.outWidth,
                  status.outHeight, status.netWidth, status.netHeight);
    // What used to be an eleven-control Guides tab. Every one of those switches is on by default
    // and picks itself: depth and motion are taken when the game hands them over and estimated
    // when it does not, the companion effect is used when it is installed. The switches are still
    // there, one cascade entry away, for a target where the detector picks the wrong buffer; this
    // line is what a person actually needs, which is what the network is being fed.
    RightLine(nullptr, T("depth %s, motion %s", "profundidade %s, movimento %s"),
              GuideSourceName(status.depthSource), GuideSourceName(status.motionSource));
    if (status.stillPct >= 0)
        RightLine(nullptr, T("depth %.4f..%.4f, %d%% still", "profundidade %.4f..%.4f, %d%% parado"),
                  static_cast<double>(status.depthMin), static_cast<double>(status.depthMax),
                  status.stillPct);
    if (skipping)
        Note(kWarn, T("The network is not finishing inside a frame, and that is the flicker. "
                      "Lower Scale and set Passes to 1.",
                      "A rede não está terminando dentro do quadro, e é isso o piscar. Baixe a "
                      "Escala e ponha Passes em 1."));
}

} // namespace

void DrawGeneral(PanelSettings &s, const PanelStatus &status, PanelActions &actions)
{
    Switches(s, status);
    if (HotkeyButton(status))
        actions.Add(PanelAction::ToggleHotkeyCapture);
    int lang = s.language;
    if (ImGui::Combo(T("Language", "Idioma"), &lang, "English\0Português\0"))
        s.language = lang;
    StatusColumn(status);
}

} // namespace ui
