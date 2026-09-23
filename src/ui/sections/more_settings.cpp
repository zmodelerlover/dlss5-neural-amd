#include "sections.h"

#include "../i18n.h"
#include "../view_logic.h"
#include "../widgets/widgets.h"

namespace ui {

// The cascade. Everything this panel leaves off is one tick away, one control at a time, under the
// header it will appear beneath -- so turning something on tells you where to look for it. Nothing
// here is a second copy of anything: the same setting, the same ini key. The only thing a bit
// decides is whether a widget is drawn.
void DrawMoreSettings(PanelSettings &s, const PanelStatus &status)
{
    if (!ImGui::TreeNode(T("More settings", "Mais ajustes")))
        return;
    ImGui::TextWrapped("%s", T("Off screen by default, not off. Each of these reads and writes its "
                               "own key in amd-nr.ini either way; ticking one only puts a control "
                               "for it in the panel.",

                               "Fora da tela por padrão, não desligados. Cada um destes lê e "
                               "escreve a chave dele no amd-nr.ini de qualquer jeito; marcar um só "
                               "põe um controle para ele no painel."));

    const uint32_t available = AvailableOpts(status);
    uint32_t bits = s.optional;
    for (int grp = 0; grp < kGrpCount; ++grp)
    {
        ImGui::SeparatorText(OptGroupName(grp));
        for (int i = 0; i < kOptCount; ++i)
        {
            const OptRow &row = kOpts[i];
            if (row.group != grp || (row.bit & available) == 0)
                continue;
            bool onNow = (bits & row.bit) != 0;
            if (ImGui::Checkbox(T(row.en, row.pt), &onNow))
                bits = onNow ? (bits | row.bit) : (bits & ~row.bit);
        }
    }
    ImGui::Separator();
    if (ImGui::Button(T("All", "Todos")))
        bits = available;
    ImGui::SameLine();
    if (ImGui::Button(T("None", "Nenhum")))
        bits = 0;
    s.optional = bits;
    ImGui::TreePop();
}

} // namespace ui
