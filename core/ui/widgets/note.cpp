#include "widgets.h"

namespace ui {

void Note(const ImVec4 &colour, const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

} // namespace ui
