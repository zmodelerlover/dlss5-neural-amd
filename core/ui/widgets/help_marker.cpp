#include "widgets.h"

#include "../i18n.h"

namespace ui {

// A tooltip instead of a paragraph. Every control had its explanation printed underneath it,
// which made the panel a wall of grey text you had to read past to reach the next slider. The
// text is worth keeping -- most of it is a measured result, not a description -- so it moves
// behind the marker and the panel goes back to being a panel.
void Help(const char *en, const char *pt)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (!ImGui::IsItemHovered())
        return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(T(en, pt));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace ui
