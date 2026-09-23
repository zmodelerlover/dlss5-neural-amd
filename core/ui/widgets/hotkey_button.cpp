#include "widgets.h"

#include "../i18n.h"

namespace ui {

// Rebinding by capturing a real keypress, rather than by typing a virtual-key code. The button
// only arms or cancels the capture; the scan itself belongs to the adapter.
bool HotkeyButton(const PanelStatus &status)
{
    const bool clicked =
        ImGui::Button(status.hotkeyArmed ? T("press a key", "aperte uma tecla") : status.hotkeyName.c_str(),
                      ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f));
    ImGui::SameLine();
    ImGui::TextUnformatted(T("Hotkey", "Tecla"));
    Help("Click, then press the combination you want. Esc cancels. A key with no modifier "
         "fires during normal play, so pick one the game does not use.",
         "Clique e aperte a combinação que quiser. Esc cancela. Uma tecla sem modificador "
         "dispara durante o jogo, então escolha uma que o jogo não use.");
    return clicked;
}

} // namespace ui
