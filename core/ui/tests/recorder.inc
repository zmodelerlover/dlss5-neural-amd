// The recorder panel_check draws the panel into: ReShade's ImGui function table with every widget
// written down instead of drawn, and a click, a hover or an open More settings tree set by the test
// in advance. Included by panel_check.cpp inside its anonymous namespace; kept apart so the checks
// fit their file.

std::vector<std::string> g_drawn; // "w:" widgets, "h:" headers, "t:" text
std::set<std::string> g_click;    // labels that report a click this frame
bool g_openMore = false;          // whether the More settings tree is open
bool g_disabledSeen = false;
bool g_hover = false;             // every item hovered, so each (?) draws its text
std::map<std::string, std::vector<std::string>> g_items; // a combo's choices, by its label
std::string g_last;

bool Hit(const char* label, const char* kind) {
    g_last = label;
    g_drawn.push_back(std::string(kind) + label);
    return g_click.count(label) != 0;
}
void Txt(const char* fmt, va_list args) {
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    g_drawn.push_back(std::string("t:") + buf);
}

imgui_function_table Recorder() {
    imgui_function_table t{};
    t.GetFontSize = [] { return 13.0f; };
    t.PushStyleColor = [](ImGuiCol, ImU32) {};
    t.PushStyleColor2 = [](ImGuiCol, const ImVec4&) {};
    t.PopStyleColor = [](int) {};
    t.PushTextWrapPos = [](float) {};
    t.PopTextWrapPos = [] {};
    t.GetContentRegionAvail = [] { return ImVec2(400, 800); };
    t.GetCursorPosX = [] { return 0.0f; };
    t.SetCursorPosX = [](float) {};
    t.Separator = [] {};
    t.SameLine = [](float, float) {};
    t.NewLine = [] {};
    t.PushID = [](const char*) {};
    t.PushID4 = [](int) {};
    t.PopID = [] {};
    t.TextV = [](const char* f, va_list a) { Txt(f, a); };
    t.TextColoredV = [](const ImVec4&, const char* f, va_list a) { Txt(f, a); };
    t.TextDisabledV = [](const char* f, va_list a) { Txt(f, a); };
    t.TextWrappedV = [](const char* f, va_list a) { Txt(f, a); };
    t.TextUnformatted = [](const char* s, const char*) {
        g_drawn.push_back(std::string("t:") + s);
    };
    t.SeparatorText = [](const char* s) { g_drawn.push_back(std::string("t:") + s); };
    t.Button = [](const char* l, const ImVec2&) { return Hit(l, "w:"); };
    t.SmallButton = [](const char* l) { return Hit(l, "w:"); };
    t.Checkbox = [](const char* l, bool* v) {
        if (!Hit(l, "w:"))
            return false;
        *v = !*v;
        return true;
    };
    t.Combo = [](const char* l, int*, const char* const[], int, int) { return Hit(l, "w:"); };
    t.Combo2 = [](const char* l, int*, const char* items, int) {
        std::vector<std::string>& seen = g_items[l];
        seen.clear();
        for (const char* i = items; *i; i += seen.back().size() + 1)
            seen.push_back(i);
        return Hit(l, "w:");
    };
    t.SliderFloat = [](const char* l, float*, float, float, const char*, ImGuiSliderFlags) {
        return Hit(l, "w:");
    };
    t.SliderInt = [](const char* l, int*, int, int, const char*, ImGuiSliderFlags) {
        return Hit(l, "w:");
    };
    t.TreeNode = [](const char* l) {
        Hit(l, "h:");
        return (std::string(l) != "More settings" && std::string(l) != "Mais ajustes") ||
               g_openMore;
    };
    t.TreePop = [] {};
    t.CollapsingHeader = [](const char* l, ImGuiTreeNodeFlags) {
        Hit(l, "h:");
        return true;
    };
    t.CollapsingHeader2 = [](const char* l, bool*, ImGuiTreeNodeFlags) {
        Hit(l, "h:");
        return true;
    };
    t.BeginTooltip = [] { return g_hover; };
    t.EndTooltip = [] {};
    t.BeginDisabled = [](bool d) { g_disabledSeen |= d; };
    t.EndDisabled = [] {};
    t.IsItemHovered = [](ImGuiHoveredFlags) { return g_hover; };
    // A drag that ends on this frame: active and deactivated both answer for the clicked label.
    t.IsItemActive = [] { return g_click.count(g_last) != 0; };
    t.IsItemDeactivated = [] { return g_click.count(g_last) != 0; };
    t.CalcTextSize = [](const char*, const char*, bool, float) { return ImVec2(10, 13); };
    return t;
}
