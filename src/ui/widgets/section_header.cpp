#include "widgets.h"

namespace ui {

// ImGui draws a header's label with ImGuiCol_Text, so this is the whole of it -- no style var,
// nothing to restore beyond the one push.
bool SectionHeader(const ImVec4 &hue, const char *title, bool defaultOpen)
{
    ImGui::PushStyleColor(ImGuiCol_Text, hue);
    const bool open = ImGui::CollapsingHeader(title, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleColor();
    return open;
}

} // namespace ui
