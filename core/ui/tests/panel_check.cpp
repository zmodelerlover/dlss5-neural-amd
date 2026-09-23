// The panel with ReShade's ImGui swapped for a recorder: every widget the panel draws is written
// down, and a click is a label the test names in advance. What the bench checklist asks of the
// panel itself -- one control per cascade tick, both languages, the footer's actions, what Factory
// Defaults keeps -- then runs in a second with no game open. What only the game can show (the
// engine, the pipe, the ini on disk) stays on the bench.
//
//   g++ -std=c++20 -I3rdparty/reshade core/ui/tests/panel_check.cpp core/ui/*.cpp core/ui/sections/*.cpp core/ui/widgets/*.cpp -o panel_check
#include "../imgui_api.h"
#include "../panel.h"
#include "../view_logic.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_drawn; // "w:" widgets, "h:" headers, "t:" text
std::set<std::string> g_click;    // labels that report a click this frame
bool g_openMore = false;          // whether the More settings tree is open
bool g_disabledSeen = false;
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
    t.Combo2 = [](const char* l, int*, const char*, int) { return Hit(l, "w:"); };
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
    t.BeginTooltip = [] { return false; };
    t.EndTooltip = [] {};
    t.BeginDisabled = [](bool d) { g_disabledSeen |= d; };
    t.EndDisabled = [] {};
    t.IsItemHovered = [](ImGuiHoveredFlags) { return false; };
    // A drag that ends on this frame: active and deactivated both answer for the clicked label.
    t.IsItemActive = [] { return g_click.count(g_last) != 0; };
    t.IsItemDeactivated = [] { return g_click.count(g_last) != 0; };
    t.CalcTextSize = [](const char*, const char*, bool, float) { return ImVec2(10, 13); };
    return t;
}

int g_failures = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

ui::PanelStatus Status64() {
    ui::PanelStatus st;
    st.hasFeedEffect = true;
    st.hotkeyName = "Ctrl+END";
    return st;
}

ui::PanelSettings Base() {
    // Every row's precondition met, so each tick can show its control; Dependencies() below holds
    // the rows that wait for another setting.
    ui::PanelSettings s;
    s.passes = 2; // Taper and Per pass only mean anything above one pass
    s.scale = 1.0f;
    s.ratioGuard = 2.0f; // ratio composition, the default
    s.useDepth = 1;
    s.useMotion = 1;
    return s;
}

std::set<std::string> Draw(ui::PanelSettings& s, const ui::PanelStatus& st,
                           ui::PanelActions* out = nullptr, bool withText = false) {
    g_drawn.clear();
    ui::PanelActions a = ui::DrawPanel(s, st);
    if (out)
        *out = a;
    std::set<std::string> seen;
    for (const std::string& d : g_drawn)
        if (withText || d[0] != 't')
            seen.insert(d);
    return seen;
}

std::set<std::string> Minus(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::set<std::string> r;
    for (const std::string& x : a)
        if (!b.count(x))
            r.insert(x);
    return r;
}

std::string Join(const std::set<std::string>& s) {
    std::string r;
    for (const std::string& x : s)
        r += (r.empty() ? "" : ", ") + x;
    return r;
}

bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        const int n = c < 0x80         ? 1
                      : (c >> 5) == 6  ? 2
                      : (c >> 4) == 14 ? 3
                      : (c >> 3) == 30 ? 4
                                       : 0;
        if (n == 0 || i + n > s.size())
            return false;
        for (int k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2)
                return false;
        i += n;
    }
    return true;
}

// Bench 1: each tick puts its own control on screen, and only its own.
void CascadeOneByOne() {
    const ui::PanelStatus st = Status64();
    ui::PanelSettings s = Base();
    const std::set<std::string> base = Draw(s, st);
    std::vector<std::set<std::string>> widgets(ui::kOptCount);
    for (int i = 0; i < ui::kOptCount; ++i) {
        const ui::OptRow& row = ui::kOpts[i];
        s = Base();
        s.optional = row.bit;
        const std::set<std::string> diff = Minus(Draw(s, st), base);
        for (const std::string& d : diff)
            if (d[0] == 'w')
                widgets[i].insert(d);
        std::printf("  %-18s -> %s\n", row.en, Join(diff).c_str());
        CHECK(!widgets[i].empty(), "'%s' ticked alone draws no control", row.en);
        // The group's header comes with it: Guides and Engine have none until something is ticked.
        const char* hdr[]{"h:Performance", "h:Image", "h:Debug", "h:Guides", "h:Engine"};
        CHECK(Draw(s, st).count(hdr[row.group]), "'%s' ticked, but its header %s is not drawn",
              row.en, hdr[row.group]);
    }
    for (int i = 0; i < ui::kOptCount; ++i)
        for (int j = i + 1; j < ui::kOptCount; ++j)
            for (const std::string& w : widgets[i])
                CHECK(!widgets[j].count(w), "'%s' and '%s' both draw %s", ui::kOpts[i].en,
                      ui::kOpts[j].en, w.c_str());

    // Nothing ticked: no Guides or Engine header at all.
    s = Base();
    const std::set<std::string> none = Draw(s, st);
    CHECK(!none.count("h:Guides") && !none.count("h:Engine"),
          "Guides/Engine header drawn with nothing ticked");
}

// The cascade's own rows: a click sets that bit, All sets what the route has, None clears.
void CascadeRows() {
    ui::PanelStatus st = Status64();
    g_openMore = true;
    for (int i = 0; i < ui::kOptCount; ++i) {
        ui::PanelSettings s = Base();
        g_click = {ui::kOpts[i].en};
        Draw(s, st);
        CHECK(s.optional == ui::kOpts[i].bit, "clicking '%s' left optional=0x%x", ui::kOpts[i].en,
              s.optional);
    }
    ui::PanelSettings s = Base();
    g_click = {"All"};
    Draw(s, st);
    CHECK(s.optional == ui::kOptAll, "All on the 64-bit route gave 0x%x", s.optional);
    g_click = {"None"};
    Draw(s, st);
    CHECK(s.optional == 0, "None left 0x%x", s.optional);

    // The 32-bit route has no Feed.fx: no row for it, and All does not claim the bit.
    st.hasFeedEffect = false;
    st.helperProcess = true;
    s = Base();
    g_click = {"All"};
    const std::set<std::string> seen = Draw(s, st);
    CHECK(!seen.count("w:Use Feed.fx"), "32-bit cascade offers Use Feed.fx");
    CHECK(s.optional == (ui::kOptAll & ~ui::kOptFeed), "All on the 32-bit route gave 0x%x",
          s.optional);
    g_openMore = false;
    g_click.clear();
}

// Bench 2: Portuguese is a whole other panel, and every string in it is clean UTF-8.
void Language() {
    const ui::PanelStatus st = Status64();
    g_openMore = true;
    ui::PanelSettings en = Base(), pt = Base();
    en.optional = pt.optional = ui::kOptAll;
    pt.language = 1;
    const std::set<std::string> a = Draw(en, st, nullptr, true), b = Draw(pt, st, nullptr, true);
    g_openMore = false;
    std::set<std::string> same;
    for (const std::string& x : b) {
        CHECK(ValidUtf8(x), "not UTF-8: %s", x.c_str());
        CHECK(x.find("\xC3\x83") == std::string::npos, "double-encoded UTF-8: %s", x.c_str());
        if (a.count(x) && x.size() > 4)
            same.insert(x);
    }
    // Words that are the same in both languages, or names; anything else here is untranslated.
    const std::set<std::string> allowed{"h:Debug",  "h:Experimental", "w:Temporal", "w:Tonemap",
                                        "w:Passes", "w:Ctrl+END",     "w:1/32",     "t:(?)",
                                        "t:Debug",  "t:Performance"};
    for (const std::string& x : same)
        CHECK(allowed.count(x), "same in English and Portuguese: %s", x.c_str());
}

// Bench 4, 6, 7 and the footer: each button asks for its action.
void Actions() {
    struct {
        const char* label;
        ui::PanelAction action;
        uint32_t opt;
    } cases[]{
        {"Save", ui::PanelAction::Save, 0},
        {"Reload", ui::PanelAction::Reload, 0},
        {"Factory Defaults", ui::PanelAction::FactoryDefaults, 0},
        {"Export logs to desktop", ui::PanelAction::ExportLogs, 0},
        {"Measure residual", ui::PanelAction::MeasureResidual, ui::kOptMeasure},
        {"Ctrl+END", ui::PanelAction::ToggleHotkeyCapture, 0},
        {"Scale", ui::PanelAction::LiftScaleCap, 0},
    };
    for (const auto& c : cases) {
        ui::PanelSettings s = Base();
        s.optional = c.opt;
        ui::PanelActions a;
        g_click = {c.label};
        Draw(s, Status64(), &a);
        CHECK(a.Has(c.action), "'%s' did not ask for action 0x%x (got 0x%x)", c.label,
              static_cast<unsigned>(c.action), a.bits);
    }
    g_click.clear();

    // Letting go of Scale without moving it keeps the value and still asks to lift the cap.
    ui::PanelStatus st = Status64();
    st.scaleCap = 0.75f;
    ui::PanelSettings s = Base();
    const std::set<std::string> seen = Draw(s, st, nullptr, true);
    bool note = false;
    for (const std::string& x : seen)
        note |= x.find("Let go of the slider") != std::string::npos;
    CHECK(note, "Scale held by a cap shows no 'Let go of the slider' note");
    ui::PanelActions a;
    g_click = {"Scale"};
    Draw(s, st, &a);
    g_click.clear();
    CHECK(s.scale == 1.0f && a.Has(ui::PanelAction::LiftScaleCap),
          "releasing Scale unchanged: scale=%.2f", s.scale);
}

// The rows that wait for another setting: ticked, they stay off screen until it is on.
void Dependencies() {
    const ui::PanelStatus st = Status64();
    struct {
        uint32_t bit;
        const char* widget;
        void (*unmet)(ui::PanelSettings&);
    } cases[]{
        {ui::kOptTaper, "w:Taper passes", [](ui::PanelSettings& s) { s.passes = 1; }},
        {ui::kOptPerPass, "h:Per pass", [](ui::PanelSettings& s) { s.passes = 1; }},
        {ui::kOptDepthInv, "w:Depth inverted", [](ui::PanelSettings& s) { s.useDepth = 0; }},
        {ui::kOptDepthStretch, "w:Stretch depth", [](ui::PanelSettings& s) { s.useDepth = 0; }},
        {ui::kOptMotionScale, "w:Motion scale", [](ui::PanelSettings& s) { s.useMotion = 0; }},
        {ui::kOptFlowGate, "w:Flow gate", [](ui::PanelSettings& s) { s.useMotion = 0; }},
        {ui::kOptFlowAccept, "w:Flow accept", [](ui::PanelSettings& s) { s.useMotion = 0; }},
    };
    for (const auto& c : cases) {
        ui::PanelSettings s = Base();
        s.optional = c.bit;
        CHECK(Draw(s, st).count(c.widget), "%s missing with its precondition met", c.widget);
        c.unmet(s);
        CHECK(!Draw(s, st).count(c.widget), "%s drawn without its precondition", c.widget);
    }
    // Game motion replaces the estimator, so its two knobs go.
    ui::PanelStatus game = st;
    game.gameMotionActive = true;
    ui::PanelSettings s = Base();
    s.optional = ui::kOptFlowGate | ui::kOptFlowAccept;
    const std::set<std::string> seen = Draw(s, game);
    CHECK(!seen.count("w:Flow gate") && !seen.count("w:Flow accept"),
          "flow knobs drawn over game motion");
}

// Bench 4: Factory Defaults keeps the preferences and nothing else.
void Factory() {
    ui::PanelSettings factory, cur;
    cur.enabled = 1;
    cur.startOn = 1;
    cur.toggleKey = 0x23;
    cur.toggleMods = 1;
    cur.disableOnAltTab = 1;
    cur.language = 1;
    cur.optional = ui::kOptDepth | ui::kOptMask;
    cur.scale = 0.5f;
    cur.passes = 3;
    factory.scale = 1.0f;
    factory.passes = 1;
    const ui::PanelSettings r = ui::KeepPreferences(factory, cur);
    CHECK(r.enabled == 1 && r.startOn == 1 && r.toggleKey == 0x23 && r.toggleMods == 1 &&
              r.disableOnAltTab == 1 && r.language == 1 && r.optional == cur.optional,
          "Factory Defaults lost a preference");
    CHECK(r.scale == 1.0f && r.passes == 1, "Factory Defaults kept tuning: scale %.2f passes %u",
          r.scale, r.passes);
}

// Bench 5 and 8: the 32-bit route's own lines, and transport-only greys out what needs the network.
void Route32() {
    ui::PanelStatus st = Status64();
    st.hasFeedEffect = false;
    st.helperProcess = true;
    st.routeDiagnostics = {"protocol 3", "passes 1"};
    ui::PanelSettings s = Base();
    const std::set<std::string> seen = Draw(s, st, nullptr, true);
    CHECK(seen.count("t:protocol 3") && seen.count("t:passes 1"),
          "route diagnostics missing under Debug");
    g_disabledSeen = false;
    Draw(s, st);
    CHECK(!g_disabledSeen, "panel greyed out with a working helper");
    st.transportOnly = true;
    Draw(s, st);
    CHECK(g_disabledSeen, "transport-only did not grey out the engine controls");
}

} // namespace

int main() {
    static const imgui_function_table table = Recorder();
    imgui_function_table_instance() = &table;

    std::printf("cascade, one tick at a time:\n");
    CascadeOneByOne();
    CascadeRows();
    Dependencies();
    Language();
    Actions();
    Factory();
    Route32();
    if (g_failures) {
        std::printf("panel_check: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("panel_check: PASS\n");
    return 0;
}
