#include "widgets.h"

#include "../theme.h"

#include <cstdarg>
#include <cstdio>

namespace ui {

// Status, run-state and what the network is being fed, printed down the right edge opposite the
// switches on the left. It used to be a collapsing section of its own, which spent a header and
// a click on four lines of text that never need a click. Right-aligned into space the rows on
// the left already occupy, so it costs no height at all.
//
// On a panel too thin to hold both, the line is drawn on its own row underneath instead of
// overlapping the control to its left: thin is the normal case here, so it has to degrade rather
// than collide.
void RightLine(const ImVec4 *colour, const char *fmt, ...)
{
    char text[192];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    // Measured off what is left on the line rather than off the window: ReShade's add-on ImGui is
    // a function table and GetWindowContentRegionMax is not in it, so this uses the three calls
    // that are -- and they give the same answer without needing to know the indent.
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine();
    if (const float avail = ImGui::GetContentRegionAvail().x; avail >= width)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    else
        ImGui::NewLine();
    // Not TextDisabled: this column is the only place the run reports itself, so it reads at
    // full weight rather than at the 50% grey a hint gets.
    ImGui::TextColored(colour != nullptr ? *colour : kReport, "%s", text);
}

} // namespace ui
