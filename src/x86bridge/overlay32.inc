// UI adapted from src/neural/neural.cpp, copyright (c) 2026 cLohan, MIT.
// All original upstream files remain unchanged. This file has only local UI state.
#include <cstdarg>
namespace overlay32 {
template<class T> struct Cell { T value{}; T load()const{return value;} void store(T v){value=v;} };
struct Shadow {
    Cell<bool> enabled;
    Cell<bool> startOn;
    Cell<bool> disableOnAltTab;
    Cell<bool> serialPasses;
    Cell<bool> inlineMode;
    Cell<bool> passTaper;
    Cell<bool> guardTracksPasses;
    Cell<bool> bicubic;
    Cell<bool> networkOutput;
    Cell<bool> useMotion;
    Cell<bool> useHistory;
    Cell<bool> useDepth;
    Cell<bool> useGameGuides;
    Cell<int> toggleKey;
    Cell<int> toggleMods;
    Cell<int> language;
    Cell<int> passes;
    Cell<int> encoding;
    Cell<int> debugView;
    Cell<int> autoMask;
    Cell<int> temporalMode;
    Cell<int> tonemap;
    Cell<int> toneChannels;
    Cell<float> scale;
    Cell<float> diffuseWhite;
    Cell<float> intensity;
    Cell<float> residualLimit;
    Cell<float> residualFade;
    Cell<float> ratioGuard;
    Cell<float> colourStrength;
    Cell<float> structure;
    Cell<float> skin;
    Cell<float> tone;
    Cell<int> depthInverted;
    Cell<bool> depthNormalise;
    Cell<uint32_t> optional;
    Cell<int> style;
    Cell<float> styleStrength;
    Cell<float> motionScale;
    Cell<float> flowGate;
    Cell<float> flowRatio;
    Cell<float> engineScale;
    Cell<bool> passOverride[3],noBridge,noBackBuffer;
    Cell<float> passStructure[3],passTone[3],passSkin[3];
    Cell<float> probeDepthMin,probeDepthMax,probeMotionMean,probeMotionMax;
    Cell<int> probeStillPct,stage;
    int events=0;
    unsigned outWidth=0,outHeight=0,netWidth=0,netHeight=0;
    bool gameDepthActive=false,gameMotionActive=false;
} g;
void Log(const char*,...){ } // Original widget diagnostic calls never perform I/O here.
void Import(const x86bridge::WireSettings& s,const x86bridge::WireStatus& status){
#define X(type,name,low,high) g.name.store(s.name);
#include "settings_fields.inc"
#undef X
    for(unsigned i=0;i<3;++i){g.passOverride[i].store(s.passOverride[i]!=0);g.passStructure[i].store(s.passStructure[i]);g.passTone[i].store(s.passTone[i]);g.passSkin[i].store(s.passSkin[i]);}
    g.outWidth=status.outWidth;g.outHeight=status.outHeight;g.netWidth=status.netWidth;g.netHeight=status.netHeight;
    g.gameDepthActive=status.depthActive!=0;g.gameMotionActive=status.motionActive!=0;
    g.probeStillPct.store(status.probeValid?status.stillPct:-1);g.probeDepthMin.store(status.depthMin);g.probeDepthMax.store(status.depthMax);g.probeMotionMean.store(status.motionMean);g.probeMotionMax.store(status.motionMax);
    g.stage.store(status.stage);g.events=status.events;g.noBridge.store(status.noBridge!=0);g.noBackBuffer.store(status.noBackBuffer!=0);
}
x86bridge::WireSettings Export(){
    auto s=controls.shadow;
#define X(type,name,low,high) s.name=static_cast<type>(g.name.load());
#include "settings_fields.inc"
#undef X
    for(unsigned i=0;i<3;++i){s.passOverride[i]=g.passOverride[i].load();s.passStructure[i]=g.passStructure[i].load();s.passTone[i]=g.passTone[i].load();s.passSkin[i]=g.passSkin[i].load();}
    return s;
}
void CandidateStatus();
void Note(const ImVec4 &colour, const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

const ImVec4 kWarn { 1.0f, 0.80f, 0.30f, 1.0f };
const ImVec4 kDanger { 1.0f, 0.45f, 0.35f, 1.0f };

// Every user-visible string in the overlay goes through this. Two literals at the call site
// instead of an id, a table and a lookup: the translation is then impossible to get out of sync
// with the text it translates, and adding a control cannot leave a dangling key behind.
// ponytail: a real string table earns its keep at a third language, not at two.
const char *T(const char *en, const char *pt)
{
    return g.language.load() == 0 ? en : pt;
}

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

// Paints a control red or amber while its CURRENT VALUE is one that has caused trouble. Scoped
// so it can be declared in an if-init and still wrap the widget:
//     if (Risk r(kDanger, cond); ImGui::SliderFloat(...))
// ImGui draws a widget's label with ImGuiCol_Text, so pushing the colour colours the control.
struct Risk
{
    bool on;
    Risk(const ImVec4 &colour, bool active) : on(active)
    {
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
    }
    ~Risk()
    {
        if (on)
            ImGui::PopStyleColor();
    }
    Risk(const Risk &) = delete;
    Risk &operator=(const Risk &) = delete;
};

std::string HotkeyName()
{
    return hotkey::Name(g.toggleKey.load(), g.toggleMods.load());
}

// The hues, and the section header they colour. The same values as the 64-bit panel on purpose:
// somebody who plays a 32-bit game and a 64-bit one should not have to learn the overlay twice.
const ImVec4 kOk { 0.38f, 0.86f, 0.48f, 1.00f };
const ImVec4 kHueImage { 0.40f, 0.78f, 1.00f, 1.00f };          // blue -- the picture
const ImVec4 kHuePerf { 0.35f, 1.00f, 0.55f, 1.00f };           // green -- speed
const ImVec4 kHueDebug { 0.78f, 0.60f, 1.00f, 1.00f };          // violet -- instrumentation
const ImVec4 kHueExperimental { 1.00f, 0.72f, 0.22f, 1.00f };   // amber -- provisional
const ImVec4 kHueGuides { 0.25f, 0.95f, 0.95f, 1.00f };         // cyan -- what feeds the network
const ImVec4 kHueEngine { 1.00f, 0.55f, 0.42f, 1.00f };         // salmon -- somebody else's struct

// A section header in its own hue. ImGui draws a header's label with ImGuiCol_Text, so this is
// the whole of it -- no style var, nothing to restore beyond the one push.
bool SectionHeader(const ImVec4 &hue, const char *title, bool defaultOpen = false)
{
    ImGui::PushStyleColor(ImGuiCol_Text, hue);
    const bool open = ImGui::CollapsingHeader(title, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleColor();
    return open;
}

// Which optional controls have a widget. One bit each, carried over the wire and written by the
// helper as HiddenShown in amd-nr.ini -- the same bit numbers the 64-bit panel uses, so the key
// means the same thing whichever route wrote it.
//
// Feed.fx has a bit over there and no row here: the companion effect is read through ReShade's
// effect runtime, which lives in this process, while the network lives in the helper. There is
// nothing on this route for the flag to reach.
enum Opt : uint32_t
{
    kOptCompose      = 1u << 0,
    kOptGuard        = 1u << 1,
    kOptGuardPerPass = 1u << 2,
    kOptLocalTone    = 1u << 3,
    kOptTaper        = 1u << 4,
    kOptPerPass      = 1u << 5,
    kOptBicubic      = 1u << 6,
    kOptGameGuides   = 1u << 8,
    kOptDepth        = 1u << 9,
    kOptDepthInv     = 1u << 10,
    kOptDepthStretch = 1u << 11,
    kOptHistory      = 1u << 12,
    kOptMotion       = 1u << 13,
    kOptMotionScale  = 1u << 14,
    kOptFlowGate     = 1u << 15,
    kOptFlowAccept   = 1u << 16,
    kOptMask         = 1u << 17,
    kOptTemporal     = 1u << 18,
    kOptTonemap      = 1u << 19,
    kOptToneChannels = 1u << 20,
    kOptOutputScale  = 1u << 21,
    kOptMeasure      = 1u << 22,
};

// Which header a row belongs under, in the cascade and in the panel alike.
enum OptGroup { kGrpPerf, kGrpImage, kGrpDebug, kGrpGuides, kGrpEngine, kGrpCount };
const char *OptGroupName(int grp)
{
    static const char *en[] { "Performance", "Image", "Debug", "Guides", "Engine" };
    static const char *pt[] { "Desempenho", "Imagem", "Debug", "Guias", "Motor" };
    return T(en[grp], pt[grp]);
}

struct OptRow
{
    uint32_t bit;
    int group;
    const char *en, *pt;
};
const OptRow kOpts[] {
    { kOptCompose,      kGrpImage,  "Composition",       "Composição" },
    { kOptGuard,        kGrpImage,  "Highlight guard",   "Trava de realce" },
    { kOptGuardPerPass, kGrpImage,  "Guard per pass",    "Trava por passe" },
    { kOptLocalTone,    kGrpImage,  "Local tone",        "Tom local" },
    { kOptTaper,        kGrpPerf,   "Taper passes",      "Diminuir passes" },
    { kOptPerPass,      kGrpPerf,   "Per-pass settings", "Ajustes por passe" },
    { kOptBicubic,      kGrpPerf,   "Bicubic upsample",  "Upsample bicúbico" },
    { kOptMeasure,      kGrpDebug,  "Measure residual",  "Medir resíduo" },
    { kOptGameGuides,   kGrpGuides, "Read from the game","Ler do jogo" },
    { kOptDepth,        kGrpGuides, "Depth",             "Profundidade" },
    { kOptDepthInv,     kGrpGuides, "Depth inverted",    "Profundidade invertida" },
    { kOptDepthStretch, kGrpGuides, "Stretch depth",     "Esticar profundidade" },
    { kOptHistory,      kGrpGuides, "History",           "Histórico" },
    { kOptMotion,       kGrpGuides, "Motion",            "Movimento" },
    { kOptMotionScale,  kGrpGuides, "Motion scale",      "Escala do movimento" },
    { kOptFlowGate,     kGrpGuides, "Flow gate",         "Portão do fluxo" },
    { kOptFlowAccept,   kGrpGuides, "Flow accept",       "Aceite do fluxo" },
    { kOptMask,         kGrpEngine, "Character mask",    "Máscara de personagem" },
    { kOptTemporal,     kGrpEngine, "Temporal",          "Temporal" },
    { kOptTonemap,      kGrpEngine, "Tonemap",           "Tonemap" },
    { kOptToneChannels, kGrpEngine, "Tone channels",     "Canais de tom" },
    { kOptOutputScale,  kGrpEngine, "Output scale",      "Escala de saída" },
};

bool Shown(uint32_t bit)
{
    return (g.optional.load() & bit) != 0;
}

// Whether a whole group has anything turned on, which is what decides if its header is drawn.
bool GroupShown(int grp)
{
    const uint32_t on = g.optional.load();
    for (const OptRow &r : kOpts)
        if (r.group == grp && (on & r.bit) != 0)
            return true;
    return false;
}

// Everything the cascade can turn on, which is what the All button writes. Built from the table
// rather than spelled out, so a row this route does not have is not claimed by it.
uint32_t AllOpts()
{
    uint32_t bits = 0;
    for (const OptRow &r : kOpts)
        bits |= r.bit;
    return bits;
}

// What the network is really running at. The cap is the helper's own: it lowers the scale there
// when one evaluation takes long enough to risk the display driver, and reports it in the status.
// Comparing the raster against the slider instead would report a working configuration as "not
// applied yet" for as long as the cap held.
float EffectiveScale()
{
    const float wanted = g.scale.load(), cap = controls.status.scaleCap;
    return cap > 0.0f && cap < wanted ? cap : wanted;
}

// Status, run-state and what the network is being fed, printed down the right edge opposite the
// switches on the left. Right-aligned into space the rows on the left already occupy, so it costs
// no height at all; on a panel too thin to hold both, the line drops onto its own row underneath
// instead of colliding with the control to its left. Thin is the normal case here.
void RightLine(const ImVec4 *colour, const char *fmt, ...)
{
    char text[192];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    // Measured off what is left on the line rather than off the window: ReShade's add-on ImGui is
    // a function table and GetWindowContentRegionMax is not in it.
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine();
    if (const float avail = ImGui::GetContentRegionAvail().x; avail >= width)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    else
        ImGui::NewLine();
    // Not TextDisabled: this column is the only place the run reports itself, so it reads at full
    // weight rather than at the 50% grey a hint gets.
    ImGui::TextColored(colour != nullptr ? *colour : ImVec4(0.72f, 0.78f, 0.86f, 1.00f),
                       "%s", text);
}

// One line, beside the Enabled box. This route has more ways of not running than the 64-bit one
// -- the engine is a second process -- so the first thing it answers is whether there is anything
// on the other end at all.
void StatusLine()
{
    const auto &s = controls.status;
    if (!s.connected)
        ImGui::TextColored(kDanger, "%s", T("no helper", "sem ajudante"));
    else if (s.failed)
        ImGui::TextColored(kDanger, "%s", T("error -- see the log", "erro -- veja o log"));
    else if (s.transportOnly)
        ImGui::TextColored(kWarn, "TRANSPORT_ONLY");
    else if (!s.engineReady)
        ImGui::TextColored(kWarn, "%s", T("engine not ready", "motor não pronto"));
    else if (!g.enabled.load())
        ImGui::TextDisabled("%s", T("off", "desligado"));
    else if (s.processed == 0)
        ImGui::TextColored(kWarn, "%s", T("no frames yet", "nenhum quadro ainda"));
    else
    {
        const uint64_t seen = s.processed + s.skipped;
        const double pct = seen != 0 ? 100.0 * static_cast<double>(s.skipped) / seen : 0.0;
        ImGui::TextColored(pct >= 10.0 ? kWarn : ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           T("%llu frames, %.0f%% skipped", "%llu quadros, %.0f%% pulados"),
                           static_cast<unsigned long long>(s.processed), pct);
    }
}

// The 32-bit panel, rebuilt 22/09/2026 to be the one the 64-bit route got. It used to carry the
// forty controls and the MEASURED/TRACED/UNKNOWN/INERT tags that the rebuild removed, so a 32-bit
// game showed the old menu over the new image: the composition, the guard, the residual limits
// and the colour control were all already crossing the bridge, with nothing in the panel to reach
// them.
//
// Same fifteen controls, same cascade, same wording. What differs is only what this route really
// does differently, and each of those is commented where it appears:
//
//   * Timing switches the bridge's pipelining, not the engine's inline mode -- the helper always
//     runs same-frame inside itself.
//   * Save, Reload, Factory defaults and Measure are requests posted to the helper, because the
//     ini and the engine live in the other process. The overlay never talks on the pipe.
//   * The right column opens with what this bridge is doing rather than with a per-game profile,
//     which is knowledge the 64-bit add-on has and this frontend deliberately does not.
//   * Feed.fx has no row: the effect runtime is here, the network is over there.
//
// Written for a NARROW panel, which is how this is used. Short labels, no PushItemWidth, and
// anything that is a sentence goes through TextWrapped.
void Draw()
{
    bool on = g.enabled.load();
    if (ImGui::Checkbox(T("Enabled", "Ligado"), &on))
    {
        g.enabled.store(on);
        Log("menu: %s", on ? "on" : "off");
    }
    ImGui::SameLine();
    StatusLine();

    bool start = g.startOn.load();
    if (ImGui::Checkbox(T("On at startup", "Ligar ao abrir o jogo"), &start))
    {
        g.startOn.store(start);
        Log("menu: start enabled %d", start ? 1 : 0);
    }
    Help("Whether Enabled is already ticked when the game opens, instead of waiting for the "
         "hotkey every time.",
         "Se o Ligado já vem marcado quando o jogo abre, em vez de esperar a tecla de atalho "
         "toda vez.");

    bool altTab = g.disableOnAltTab.load();
    if (ImGui::Checkbox(T("Off on alt-tab", "Desligar no alt-tab"), &altTab))
    {
        g.disableOnAltTab.store(altTab);
        Log("menu: disable on alt-tab %d", altTab ? 1 : 0);
    }
    Help("Switches the effect off when the game stops being the window in front, and leaves it "
         "off -- turn it back on with the hotkey. A minimised window is always sat out, "
         "separately, and that one does resume on its own.",

         "Desliga o efeito quando o jogo deixa de ser a janela da frente, e deixa desligado -- "
         "religue na tecla de atalho. Janela minimizada é outra coisa: sempre pulada, e essa "
         "volta sozinha.");

    {
        // Rebinding by capturing a real keypress rather than by typing a virtual-key code. The
        // scan itself runs on the present path in frontend32.cpp: this callback is not called
        // every frame, and a scan that only advanced inside it never outlived the cancel timer.
        // This button only arms it.
        const std::string hotkey = HotkeyName();
        if (ImGui::Button(controls.capture.armed ? T("press a key", "aperte uma tecla")
                                                 : hotkey.c_str(),
                          ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f)))
            controls.capture.Toggle();
        ImGui::SameLine();
        ImGui::TextUnformatted(T("Hotkey", "Tecla"));
        Help("Click, then press the combination you want. Esc cancels. A key with no modifier "
             "fires during normal play, so pick one the game does not use.",
             "Clique e aperte a combinação que quiser. Esc cancela. Uma tecla sem modificador "
             "dispara durante o jogo, então escolha uma que o jogo não use.");
    }

    int lang = g.language.load();
    if (ImGui::Combo(T("Language", "Idioma"), &lang, "English\0Português\0"))
    {
        g.language.store(lang);
        Log("menu: language %d", lang);
    }

    // The status column. The left column is what you change, the right column is what happened,
    // and the two are side by side in vertical space the switches already occupy.
    {
        const auto &s = controls.status;
        const uint64_t seen = s.processed + s.skipped;
        const double pct = seen != 0 ? 100.0 * static_cast<double>(s.skipped) / seen : 0.0;
        // A skipped frame goes out as the game drew it. A few percent is invisible; a third of the
        // frames is a correction that arrives every other frame, which reads as flicker.
        const bool skipping = seen > 300 && pct >= 10.0;
        // Where the 64-bit panel prints the profile it picked for this game. This frontend has no
        // profile table and is not getting one -- what it can say instead is which of the two
        // things it does it is doing, which is the answer to "why is this faster than it was".
        RightLine(skipping ? &kWarn : nullptr, "%s",
                  ::g.async ? T("32-bit bridge, async", "ponte 32 bits, assíncrona")
                            : T("32-bit bridge, same frame", "ponte 32 bits, mesmo quadro"));
        if (g.outWidth != 0)
            RightLine(nullptr, T("%ux%u to %ux%u", "%ux%u para %ux%u"), g.outWidth, g.outHeight,
                      g.netWidth, g.netHeight);
        // What used to be a Guides tab of eleven switches. Every one of them is on by default and
        // picks itself; the switches are still there, one cascade entry away, for a target where
        // the detector took the wrong buffer. This line is what a person actually needs, which is
        // what the network is being fed.
        RightLine(nullptr, T("depth %s, motion %s", "profundidade %s, movimento %s"),
                  g.gameDepthActive ? T("game", "jogo") : T("none", "nenhuma"),
                  g.gameMotionActive ? T("game", "jogo") : T("estimated", "estimado"));
        if (g.probeStillPct.load() >= 0)
            RightLine(nullptr, T("depth %.4f..%.4f, %d%% still",
                                 "profundidade %.4f..%.4f, %d%% parado"),
                      static_cast<double>(g.probeDepthMin.load()),
                      static_cast<double>(g.probeDepthMax.load()), g.probeStillPct.load());
        if (skipping)
            Note(kWarn, T("The network is not finishing inside a frame, and that is the flicker. "
                          "Lower Scale and set Passes to 1.",
                          "A rede não está terminando dentro do quadro, e é isso o piscar. Baixe "
                          "a Escala e ponha Passes em 1."));
    }

    // Everything below needs the engine. In transport-only mode the helper is copying the frame
    // across and back with no network at all, and a control that cannot reach anything should not
    // look as if it can.
    ImGui::BeginDisabled(controls.status.transportOnly != 0);

    if (SectionHeader(kHuePerf, T("Performance", "Desempenho"), true))
    {
        const float sc = g.scale.load();
        // Reads and writes the frontend's own flag rather than a shadow field, so it cannot drift
        // from what the bridge is actually doing and needs no round trip to the helper. This is
        // the one control that means something different from its 64-bit namesake: inside the
        // helper the network always runs same-frame, and what switches here is whether this
        // process waits for the answer or composes the previous one.
        //
        // Live. Switching is safe in both directions because CollectPending runs before anything
        // else each present, so neither mode ever starts with work outstanding.
        int timing=::g.async?1:0;
        if (Risk r(kDanger, !::g.async && sc > 1.0f);
            ImGui::Combo(T("Timing", "Momento"), &timing,
                         T("Same frame\0Async\0", "Mesmo quadro\0Assíncrono\0")))
            ::SetAsync(timing==1);
        Help("Same frame waits for this frame's own result: correct, and every millisecond the "
             "network costs is a millisecond of frame time.\n\n"
             "Async posts the frame and composes the answer in "
             "the next present, so the helper works while the game builds its next frame. It is "
             "faster and it costs one frame of lag. It does not smear: the whole picture is replaced, "
             "so you see the previous frame finished rather than a mix of two. Changing this takes "
             "effect at once and is saved to amd-nr.ini.",

             "Mesmo quadro espera o resultado deste quadro: correto, e cada milissegundo que a "
             "rede custa é milissegundo de tempo de quadro.\n\n"
             "No modo assíncrono o quadro é enviado e a "
             "resposta é composta no present seguinte, então o ajudante trabalha enquanto o jogo "
             "monta o próximo quadro. É mais rápido e custa um quadro de atraso. Não borra: a imagem "
             "inteira é substituída, então você vê o quadro anterior pronto, não uma mistura de dois. "
             "A troca vale na hora e fica salva no amd-nr.ini.");

        // Keep the value being dragged separate from the value the helper is told about.
        // SliderFloat changes on every mouse movement; publishing each intermediate float made the
        // helper rebuild a complete network raster every frame, while the runtime and driver kept
        // the retired allocations resident. Commit once, when the edit ends.
        static float editingScale = g.scale.load();
        static bool editingScaleActive = false;
        if (!editingScaleActive)
            editingScale = g.scale.load();
        float v = editingScale;
        if (Risk r(kDanger, v > 1.0f && !::g.async);
            ImGui::SliderFloat(T("Scale", "Escala"), &v, 0.25f, 2.0f, "%.2f", 0))
            editingScale = v;
        if (ImGui::IsItemActive())
            editingScaleActive = true;
        if (ImGui::IsItemDeactivated())
        {
            if (editingScaleActive && editingScale != g.scale.load())
            {
                // A new value reaching the helper is also what lifts an automatic cap it put on.
                // Unlike the 64-bit route, letting go on the value you started from sends nothing
                // and lifts nothing: only a revision travels, and an unchanged control makes none.
                g.scale.store(editingScale);
                Log("menu: resolution scale %.2f", static_cast<double>(editingScale));
            }
            editingScaleActive = false;
        }
        Help("Width and height the network runs at, relative to the game frame. 0.50 uses a "
             "quarter of the pixels. Lower is faster and the network answers differently, not "
             "just softer; above 1.00 it costs GPU time and memory for a frame that is already "
             "at full resolution.",

             "Largura e altura em que a rede roda, em relação ao quadro do jogo. 0.50 usa um "
             "quarto dos pixels. Menor é mais rápido e a rede responde diferente, não só mais "
             "suave; acima de 1.00 custa tempo de GPU e memória para um quadro que já está em "
             "resolução cheia.");
        // Against what is actually being run at, not what the slider says: under a cap those two
        // disagree by design.
        if (g.outWidth != 0)
        {
            const float running = EffectiveScale();
            const UINT wantW = (std::max)(64u, static_cast<UINT>(g.outWidth * running + 0.5f));
            const UINT wantH = (std::max)(64u, static_cast<UINT>(g.outHeight * running + 0.5f));
            if (wantW != g.netWidth || wantH != g.netHeight)
                ImGui::TextColored(kWarn, T("%ux%u, asked %ux%u", "%ux%u, pediu %ux%u"),
                                   g.netWidth, g.netHeight, wantW, wantH);
            else
                ImGui::TextDisabled("%ux%u", g.netWidth, g.netHeight);
        }
        if (const float cap = controls.status.scaleCap; cap > 0.0f && cap < g.scale.load())
            Note(kWarn,
                 T("Held below the slider by this card's own limit: one network run took long "
                   "enough to reset the display driver and take the game with it. Move the "
                   "slider to ask for the full scale again.",
                   "Segurado abaixo do slider pelo limite desta placa: uma passada da rede levou "
                   "tempo bastante para resetar o driver de vídeo e levar o jogo junto. Mexa no "
                   "slider para pedir a escala cheia de novo."));

        int passes = g.passes.load();
        if (Risk r(kDanger, !::g.async && passes > 1 && sc > 1.0f);
            ImGui::SliderInt(T("Passes", "Passes"), &passes, 1, 3, "%d", 0))
        {
            g.passes.store(passes);
            Log("menu: pass count %d", passes);
        }
        Help("Runs the network over its own output one to three times, and each run costs another "
             "inference. It can strengthen material detail; it also compounds grain and halos, "
             "because each pass is editing the last one's work.\n\n"
             "1 is the default. Watch the skipped percentage at the top when you raise it: a "
             "network that stops finishing inside a frame is the flicker.",

             "Roda a rede sobre a própria saída de uma a três vezes, e cada rodada custa outra "
             "inferência. Pode reforçar detalhe de material; também acumula granulado e halo, "
             "porque cada passe está editando o trabalho do anterior.\n\n"
             "1 é o padrão. Olhe a porcentagem de pulados lá em cima ao subir: rede que para de "
             "terminar dentro do quadro é o piscar.");

        if (Shown(kOptTaper) && passes > 1)
        {
            bool taper = g.passTaper.load();
            if (ImGui::Checkbox(T("Taper passes", "Diminuir passes"), &taper))
            {
                g.passTaper.store(taper);
                Log("menu: pass taper %s", taper ? "on" : "off");
            }
            Help("Halves Structure on each later pass: 1.0, 0.5, 0.25. Local tone already drops "
                 "to zero after pass 1. Per-pass overrides win over this. Can reduce accumulated "
                 "grain and outlines; compare in your scene.",
                 "Reduz Estrutura pela metade a cada passe: 1.0, 0.5, 0.25. Tom local já cai a "
                 "zero depois do passe 1. Ajustes por passe têm prioridade sobre isto. Pode "
                 "reduzir granulado e contorno acumulados; compare na sua cena.");
        }

        // Per-pass profiles. A later pass is looking at a picture an earlier one already edited,
        // so the same numbers again ask it to sharpen its own sharpening -- and that is the half
        // of "3 passes looks deep fried" the composition cannot reach from outside, because it
        // happens inside the network.
        if (Shown(kOptPerPass) && passes > 1 && ImGui::TreeNode(T("Per pass", "Por passe")))
        {
            for (int i = 0; i < passes; ++i)
            {
                char label[32];
                std::snprintf(label, sizeof(label), T("Pass %d", "Passe %d"), i + 1);
                if (!ImGui::TreeNode(label))
                    continue;
                ImGui::PushID(i);
                bool own = g.passOverride[i].load();
                if (ImGui::Checkbox(T("Own settings", "Ajustes próprios"), &own))
                {
                    g.passOverride[i].store(own);
                    Log("menu: pass %d profile %s", i + 1, own ? "on" : "off");
                }
                ImGui::BeginDisabled(!own);
                float pv = g.passStructure[i].load();
                if (ImGui::SliderFloat(T("Structure", "Estrutura"), &pv, 0.0f, 3.0f, "%.2f", 0))
                    g.passStructure[i].store(pv);
                pv = g.passTone[i].load();
                if (ImGui::SliderFloat(T("Tone", "Tom"), &pv, 0.0f, 3.0f, "%.2f", 0))
                    g.passTone[i].store(pv);
                pv = g.passSkin[i].load();
                if (ImGui::SliderFloat(T("Skin", "Pele"), &pv, -1.0f, 3.0f, "%.2f", 0))
                    g.passSkin[i].store(pv);
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::TreePop();
            }
            Note(kWarn, T("A pass without its own settings follows the values above. The useful "
                          "shape is a taper, because each pass edits the last one's work. -1 Skin "
                          "is the engine's automatic.",
                          "Um passe sem ajustes próprios segue os valores acima. O formato útil é "
                          "uma queda, porque cada passe edita o trabalho do anterior. Pele -1 é o "
                          "automático do motor."));
            ImGui::TreePop();
        }

        if (Shown(kOptBicubic))
        {
            bool bic = g.bicubic.load();
            if (ImGui::Checkbox(T("Bicubic upsample", "Upsample bicúbico"), &bic))
            {
                g.bicubic.store(bic);
                Log("menu: residual upsample %s", bic ? "bicubic" : "bilinear");
            }
            Help("Below Scale 1.00 only the correction comes back up to full resolution. "
                 "Stretching it bilinearly is a blur that throws away everything but colour and "
                 "brightness, which alone made the whole effect look like a colour filter. "
                 "Catmull-Rom keeps the rest. Turn it off if hard edges ring. Nothing at all at "
                 "Scale 1.00.",

                 "Abaixo de Escala 1.00 só a correção volta para a resolução cheia. Esticar ela "
                 "bilinearmente é um borrão que joga fora tudo menos cor e brilho, e isso sozinho "
                 "já fazia o efeito inteiro parecer um filtro de cor. Catmull-Rom mantém o resto. "
                 "Desligue se arestas duras ficarem com halo. Nada em Escala 1.00.");
        }
    }

    if (SectionHeader(kHueImage, T("Image", "Imagem"), true))
    {
        int enc = g.encoding.load();
        if (ImGui::Combo(T("Encoding", "Codificação"), &enc, "sRGB\0Linear\0scRGB-nl\0"))
        {
            g.encoding.store(enc);
            Log("menu: encoding %d", enc);
        }
        Help("sRGB for an ordinary SDR game, and that is nearly always the right answer. Linear "
             "and scRGB-nl are experimental conversion paths for an HDR frame.\n\n"
             "Restart the game after changing this: the runtime caches its input conversion when "
             "it creates its staging resources.",

             "sRGB para jogo SDR comum, e essa é quase sempre a resposta certa. Linear e scRGB-nl "
             "são conversões experimentais, para quadro HDR.\n\n"
             "Reinicie o jogo depois de mudar: o runtime guarda a conversão de entrada quando "
             "cria os recursos dele.");

        // Greyed out and meaningless on sRGB, which is the default, so it used to be a permanent
        // dead slider for nearly every user. It only exists once the frame is linear.
        if (g.encoding.load() != 0)
        {
            float white = g.diffuseWhite.load();
            if (ImGui::SliderFloat(T("Diffuse white", "Branco difuso"), &white, 80.0f, 1000.0f,
                                   "%.0f nits", 0))
                g.diffuseWhite.store(white);
            Help("How many nits a value of 1.0 means, which sets the scale of the linear image "
                 "the network is handed. 100 for linear BT.709, 203 for scRGB-nl, 250 for PQ. A "
                 "wrong scale here looks like the network over- or under-reacting everywhere at "
                 "once.",

                 "Quantos nits um valor de 1.0 significa, o que define a escala da imagem linear "
                 "entregue à rede. 100 para linear BT.709, 203 para scRGB-nl, 250 para PQ. Escala "
                 "errada aqui parece a rede reagindo demais ou de menos em tudo ao mesmo tempo.");
        }

        float v = g.intensity.load();
        if (ImGui::SliderFloat(T("Intensity", "Intensidade"), &v, 0.0f, 2.0f, "%.2f", 0))
            g.intensity.store(v);
        Help("The weight of the whole effect. 0.00 is the same picture as switching the add-on "
             "off, which makes it the fastest A/B there is; 1.00 is the network at full strength. "
             "Above 1 it pushes past what the network returned, still bounded, so it cannot "
             "rotate hue.\n\n"
             "It is a mix, not a parameter of the network: it shows more or less of what the "
             "network did, it never makes it do more.",

             "O peso do efeito inteiro. 0.00 dá a mesma imagem que desligar o add-on, o que faz "
             "dele o A/B mais rápido que existe; 1.00 é a rede em força cheia. Acima de 1 empurra "
             "além do que a rede devolveu, ainda limitado, então não consegue rodar matiz.\n\n"
             "É uma mistura, não um parâmetro da rede: mostra mais ou menos do que a rede fez, "
             "nunca faz ela fazer mais.");

        v = g.colourStrength.load();
        if (ImGui::SliderFloat(T("Colour", "Cor"), &v, 0.0f, 1.0f, "%.2f", 0))
            g.colourStrength.store(v);
        Help("Whether the network's colour arrives with its light. This is the control for \"it "
             "changed the colours\": at 0 every pixel keeps the game's own hue and only its "
             "brightness carries what the network decided -- by construction, it cannot change "
             "colour there. 1 brings the network's colour with it.",

             "Se a cor da rede vem junto com a luz dela. É este o controle para \"mudou as "
             "cores\": em 0 cada pixel fica com a matiz do próprio jogo e só o brilho carrega o "
             "que a rede decidiu -- por construção, ali ele não consegue mudar cor. 1 traz a cor "
             "da rede junto.");

        v = g.structure.load();
        if (ImGui::SliderFloat(T("Structure", "Estrutura"), &v, 0.0f, 3.0f, "%.2f", 0))
            g.structure.store(v);
        Help("How much detail the network is asked to put back, and the one control measured to "
             "matter: at 0 the correction collapses 25x, which is the proof that what reaches the "
             "screen comes from the network at all.\n\n"
             "1.00 is the engine's own default. It saturates above that -- 1 to 3 is about 6% "
             "more correction -- so 3 is the end of it, not the middle.",

             "Quanto detalhe a rede é pedida para devolver, e o único controle medido como "
             "relevante: em 0 a correção despenca 25x, o que é a prova de que o que chega na tela "
             "vem da rede.\n\n"
             "1.00 é o padrão do próprio motor. Satura acima disso -- de 1 para 3 é uns 6% mais "
             "correção -- então 3 é o fim dele, não o meio.");

        // -1 is the value the engine boots with and it is a MODE -- "derive it from local
        // structure" -- not a strength, so it never belonged on the same axis as 0..3. The x86
        // ini used to be written with Skin=1 on a fresh install, which wrote that automatic off
        // before anybody had touched a control.
        bool skinAuto = g.skin.load() < 0.0f;
        if (ImGui::Checkbox(T("Auto skin", "Pele automática"), &skinAuto))
        {
            g.skin.store(skinAuto ? -1.0f : 1.0f);
            Log("menu: skin structure %s", skinAuto ? "automatic (-1)" : "manual (1.0)");
        }
        Help("Structure aimed at skin, through the engine's own character mask. Ticked is the "
             "engine deriving it from Structure, which is what it boots doing.\n\n"
             "Untick to set it by hand. Measured worth about 1.5% of the correction, which is "
             "inside the +/-6% run-to-run noise on this bench -- so do not expect to see this one "
             "move the picture.",

             "Estrutura mirada na pele, através da máscara de personagem do próprio motor. "
             "Marcada é o motor derivando da Estrutura, que é como ele liga.\n\n"
             "Desmarque para ajustar na mão. Medido valendo uns 1,5% da correção, o que está "
             "dentro do ruído de +/-6% entre execuções desta bancada -- então não espere ver este "
             "mover a imagem.");
        if (!skinAuto)
        {
            float sk = g.skin.load();
            if (ImGui::SliderFloat(T("Skin", "Pele"), &sk, 0.0f, 3.0f, "%.2f", 0))
                g.skin.store((std::max)(sk, 0.0f));
        }

        // The composition's own bounds. Real controls with real effects, but their defaults are
        // what seven games' worth of reference logs agree on, so the ordinary reason to touch one
        // is to answer a question rather than to tune a picture -- which is why each waits for
        // somebody to ask for it in the cascade at the bottom of the panel.
        if (Shown(kOptCompose))
        {
            int comp = g.ratioGuard.load() > 0.0f ? 1 : 0;
            const char *items[] = { T("Additive", "Aditiva"), T("Ratio", "Razão") };
            if (ImGui::Combo(T("Compose", "Composição"), &comp, items, 2))
            {
                g.ratioGuard.store(comp == 1 ? 2.0f : 0.0f);
                Log("menu: composition %s", comp == 1 ? "ratio" : "additive");
            }
            Help("How the network's answer is put back onto the frame. Ratio is the default and "
                 "the only one that survives more than one pass.\n\n"
                 "Additive adds the correction channel by channel and clips what leaves the "
                 "range, and a clipped channel is a hue rotation -- which is why two passes read "
                 "as more saturation rather than more detail. Ratio compares luminance as a "
                 "bounded ratio and blends two finished pictures, which cannot move hue. Kept "
                 "switchable so both can be seen in one session.",

                 "Como a resposta da rede volta para o quadro. Razão é o padrão e a única que "
                 "sobrevive a mais de um passe.\n\n"
                 "Aditiva soma a correção canal por canal e corta o que sai da faixa, e canal "
                 "cortado é rotação de matiz -- por isso dois passes aparecem como mais saturação "
                 "em vez de mais detalhe. Razão compara luminância como razão limitada e mistura "
                 "duas imagens inteiras, o que não consegue mover matiz. Deixado trocável para "
                 "dar para ver os dois na mesma sessão.");
        }
        // Both live inside the same BeginDisabled, because neither means anything on the additive
        // composition -- which is what the guard being 0 is.
        ImGui::BeginDisabled(g.ratioGuard.load() <= 0.0f);
        if (Shown(kOptGuard))
        {
            float gv = g.ratioGuard.load();
            if (gv > 0.0f && ImGui::SliderFloat(T("Guard", "Trava"), &gv, 1.0f, 8.0f, "%.1fx", 0))
                g.ratioGuard.store(gv);
            Help("The most compose may move a pixel, as a multiple of what it already was, in "
                 "both directions. One scalar taken from luminance and applied to the whole "
                 "triple, so it bounds brightness without touching hue.\n\n"
                 "2.0x is the reference fork's default and leaves detail intact -- 254 "
                 "composition lines across seven games and nine machines all read 2.0x. Raise it "
                 "only if bright areas look clipped.",

                 "O máximo que a composição pode mover um pixel, como múltiplo do que ele já era, "
                 "nos dois sentidos. Um escalar só, tirado da luminância e aplicado no trio "
                 "inteiro, então limita brilho sem tocar em matiz.\n\n"
                 "2.0x é o padrão do fork de referência e não come detalhe -- 254 linhas de "
                 "composição em sete jogos e nove máquinas todas dizem 2.0x. Só aumente se áreas "
                 "claras parecerem estouradas.");
        }
        if (Shown(kOptGuardPerPass))
        {
            bool track = g.guardTracksPasses.load();
            if (ImGui::Checkbox(T("Guard per pass", "Trava por passe"), &track))
            {
                g.guardTracksPasses.store(track);
                Log("menu: guard follows pass count %d", track ? 1 : 0);
            }
            Help("Adds one multiple of headroom per extra pass, so 2.0x becomes 3.0x at two "
                 "passes. Off, because the reference fork does not actually do it: their ini says "
                 "MaxRatio defaults to 2.0 and never mentions the count. A fixed bound is also "
                 "the honest way to see what an extra pass is contributing.",

                 "Acrescenta um múltiplo de folga por passe extra, então 2.0x vira 3.0x em dois "
                 "passes. Desligado, porque o fork de referência não faz isso: a ini deles diz "
                 "que MaxRatio tem padrão 2.0 e nunca cita a contagem. Um limite fixo também é o "
                 "jeito honesto de ver o que um passe extra está somando.");
        }
        ImGui::EndDisabled();
        if (Shown(kOptLocalTone))
        {
            float tv = g.tone.load();
            if (ImGui::SliderFloat(T("Local tone", "Tom local"), &tv, 0.0f, 3.0f, "%.2f", 0))
                g.tone.store(tv);
            Help("LocalToneStrength, the first control slot of the network, and -- clamped to "
                 "0..1 -- the scale of the Model's grade. RenoDX ships it at 1.\n\n"
                 "An earlier sweep measured it inert on the network side, byte for byte. That "
                 "reading was taken with the fifth control slot at an off-menu value and has not "
                 "been repeated since.",

                 "LocalToneStrength, o primeiro slot de controle da rede e -- limitado a 0..1 -- "
                 "a escala do grade do Modelo. O RenoDX manda 1.\n\n"
                 "Uma varredura anterior mediu isto inerte do lado da rede, byte a byte. Essa "
                 "leitura foi feita com o quinto slot de controle num valor fora do menu e não "
                 "foi repetida desde então.");
        }
    }

    // Diagnostics, and the two controls that answer "my picture has artefacts". They live here
    // rather than under Image because Residual x8 is how you see what either one did.
    if (SectionHeader(kHueDebug, "Debug"))
    {
        int dbg = g.debugView.load();
        if (ImGui::Combo(T("View", "Visão"), &dbg,
                         T("Off\0Network input\0Network output\0Residual x8\0Motion\0Depth x500\0",
                           "Desligado\0Entrada da rede\0Saída da rede\0Resíduo x8\0Movimento\0Profundidade x500\0")))
        {
            g.debugView.store(dbg);
            Log("menu: debug view %d", dbg);
        }
        Help("Replaces the screen with one stage of the pipeline.\n\n"
             "Residual x8 is the one that answers \"is it doing anything\": the correction alone "
             "against mid grey. Flat grey means the network changed nothing; structure following "
             "edges and texture means it is working.\n\n"
             "Network input black means nothing downstream can work. Network output identical to "
             "the input means the network handed back what it was given.",

             "Substitui a tela por um estágio do pipeline.\n\n"
             "Resíduo x8 é a que responde \"está fazendo alguma coisa\": a correção sozinha "
             "contra cinza médio. Cinza chapado significa que a rede não mudou nada; estrutura "
             "seguindo arestas e textura significa que está funcionando.\n\n"
             "Entrada da rede preta significa que nada depois disso pode funcionar. Saída da rede "
             "idêntica à entrada significa que a rede devolveu o que recebeu.");

        float v = g.residualLimit.load();
        if (ImGui::SliderFloat(T("Limit", "Limite"), &v, 0.0f, 0.50f,
                               v <= 0.0f ? T("off", "desligado") : "%.3f", 0))
            g.residualLimit.store(v);
        Help("The control for blown blocks. Caps how far the correction may push one pixel, as a "
             "fraction of white -- a measured run came back with a mean of 0.072 and a maximum of "
             "4.16, and that maximum is a tile where the network extrapolated rather than saw.\n\n"
             "0.25 by default, over three times the typical correction, so an ordinary pixel "
             "never meets it. Lower it until the blocks go; too low flattens everything, which "
             "Residual x8 shows immediately.",

             "O controle dos blocos estourados. Limita o quanto a correção pode empurrar um "
             "pixel, como fração do branco -- uma medição deu média 0,072 com máximo de 4,16, e "
             "esse máximo é um bloco onde a rede extrapolou em vez de ver.\n\n"
             "0,25 por padrão, mais de três vezes a correção típica, então pixel normal nunca "
             "encosta. Baixe até os blocos sumirem; baixo demais achata tudo, o que o Resíduo x8 "
             "mostra na hora.");

        v = g.residualFade.load();
        if (ImGui::SliderFloat(T("Edges", "Bordas"), &v, 0.0f, 0.25f,
                               v <= 0.0f ? T("off", "desligado") : "%.3f", 0))
            g.residualFade.store(v);
        Help("The control for glitching corners. Rolls the correction off over a band at the "
             "frame border, as a fraction of the frame: 0.02 is about 20 pixels at 1080p.\n\n"
             "Border tiles have no neighbour on one side, so what the network returns there is "
             "invented rather than seen. A corner sits inside two bands at once, which is why it "
             "goes first. Off by default -- turn it on only if you see it.",

             "O controle dos cantos com glitch. Vai apagando a correção numa faixa na borda do "
             "quadro, como fração do quadro: 0,02 é uns 20 pixels em 1080p.\n\n"
             "Bloco de borda não tem vizinho de um lado, então o que a rede devolve ali é "
             "inventado, não visto. Um canto está dentro de duas faixas ao mesmo tempo, por isso "
             "estraga primeiro. Desligado por padrão -- ligue só se você vir.");

        if (Shown(kOptMeasure))
        {
            // A request, not a call: the engine is in the other process, and this callback owns
            // no transaction. OnPresent posts it with the rest of the control traffic.
            if (ImGui::Button(T("Measure residual", "Medir resíduo")))
            {
                controls.measure=true;
                Log("menu: residual measurement re-armed");
            }
            Help("Writes a 'measure, residual' line to the log: the size of the correction, and "
                 "how much of it follows the image's own detail. Change one control, press this, "
                 "compare the two numbers. It is the only way to tell a control that does "
                 "something from one that does not.",

                 "Escreve uma linha 'measure, residual' no log: o tamanho da correção, e quanto "
                 "dela segue o detalhe da própria imagem. Mude um controle, aperte isto, compare "
                 "os dois números. É o único jeito de separar um controle que faz algo de um que "
                 "não faz.");
        }

        // What the old Status section carried that this route has and the 64-bit one does not:
        // there are two processes, two logs, and a guide detector whose pick is worth seeing.
        ImGui::Separator();
        ImGui::TextDisabled("Protocol v3 | %s | %s", ::g.async ? "async" : "same frame",
                            controls.status.transportOnly ? "TRANSPORT_ONLY" : "neural");
        ImGui::TextDisabled("passes loaded/active=%u/%u", controls.status.loadedPasses,
                            controls.status.activePasses);
        CandidateStatus();
        ImGui::TextDisabled("Logs: amd-nr-x86.log / amd-nr-x86-host.log");
    }

    if (SectionHeader(kHueExperimental, "Experimental"))
    {
        Note(kWarn, T("Work in progress -- not yet behaving the way they do on NVIDIA.",
                      "Em desenvolvimento -- ainda não se comportam como na NVIDIA."));

        // Both namings, because both are in circulation for the same three values of the same
        // NGX field: RenoDX calls them Model A/B/C, Deep Fried Chicken calls them Default,
        // Natural and Cinematic. A guide written against one and an overlay showing the other is
        // a person changing the wrong control.
        int style = g.style.load();
        if (ImGui::Combo(T("Model", "Modelo"), &style,
                         T("A - Default\0B - Natural\0C - Cinematic\0",
                           "A - Default\0B - Natural\0C - Cinematic\0")))
        {
            g.style.store(style < 0 ? 0 : style > 2 ? 2 : style);
            Log("menu: style %d", style);
        }
        Help("The Neural Rendering Model -- the same three DLSSNR.Style selects on NVIDIA, but "
             "not yet doing the same thing here, which is why it sits under Experimental.\n\n"
             "On NVIDIA a model is two things: an input of the network, which is what moves "
             "lighting and detail, and a grade on the finished frame. This runtime has no slot "
             "for the first that anything here can reach, so a model is its grade only: B darkens "
             "slightly, flattens contrast and removes a tenth of the saturation; C removes 15 "
             "percent of the saturation. A colour change rather than a detail change, and a "
             "smaller difference than on NVIDIA.\n\n"
             "Two names for each: RenoDX writes Model A, B and C; Deep Fried Chicken writes "
             "Default, Natural and Cinematic, in that order.",

             "O Modelo de Renderização Neural -- os mesmos três que o DLSSNR.Style seleciona na "
             "NVIDIA, mas ainda não fazendo a mesma coisa aqui, e é por isso que está em "
             "Experimental.\n\n"
             "Na NVIDIA um modelo é duas coisas: uma entrada da rede, que é o que move iluminação "
             "e detalhe, e um grade no quadro pronto. Este runtime não tem slot para a primeira "
             "que alguma coisa daqui alcance, então um modelo é só o grade dele: B escurece de "
             "leve, achata o contraste e tira um décimo da saturação; C tira 15 por cento da "
             "saturação. Mudança de cor em vez de mudança de detalhe, e diferença menor que na "
             "NVIDIA.\n\n"
             "Dois nomes para cada um: o RenoDX escreve Model A, B e C; o Deep Fried Chicken "
             "escreve Default, Natural e Cinematic, nessa ordem.");

        if (style != 0)
        {
            float ss = g.styleStrength.load();
            if (ImGui::SliderFloat(T("Strength", "Força"), &ss, 0.0f, 1.0f, "%.2f"))
                g.styleStrength.store(ss < 0.0f ? 0.0f : ss > 1.0f ? 1.0f : ss);
            Help("Scales the model's grade towards neutral. 1 is the full grade, 0 removes the "
                 "colour change entirely. Ours -- NVIDIA has no separate knob for this.",
                 "Escalona o grade do modelo em direção ao neutro. 1 é o grade inteiro, 0 tira a "
                 "mudança de cor por completo. É nosso -- a NVIDIA não tem controle separado para "
                 "isto.");
        }

        bool raw = g.networkOutput.load();
        if (ImGui::Checkbox(T("Network output", "Saída da rede"), &raw))
        {
            g.networkOutput.store(raw);
            Log("menu: network output mode %d", raw ? 1 : 0);
        }
        Help("Shows the network's answer directly instead of composing it onto the game's frame. "
             "Preferred by eye in one game, where the composition trailed behind fast movement, "
             "and never measured against the composition anywhere else.\n\n"
             "It turns off every bound on the correction: nothing limits how far a pixel may "
             "move, and hue is whatever the network returned. That is the trade. A skipped "
             "evaluation still presents the game's own frame on this route.",

             "Mostra a resposta da rede direto, em vez de compô-la sobre o quadro do jogo. "
             "Preferido a olho em um jogo, onde a composição deixava rastro atrás de movimento "
             "rápido, e nunca medido contra a composição em nenhum outro lugar.\n\n"
             "Desliga todo limite sobre a correção: nada limita o quanto um pixel pode andar, e o "
             "matiz é o que a rede devolveu. Essa é a troca. Uma avaliação pulada continua "
             "apresentando o quadro do próprio jogo nesta rota.");
    }

    // The two sections that only exist under the cascade. Everything in them is automatic, or
    // ships at the engine's own default, or is a question waiting for a measurement -- which is
    // exactly why they are not on screen by default, and exactly why they are still here. The
    // right column shows what the guides ended up being; this is where they can be overruled.
    if (GroupShown(kGrpGuides) && SectionHeader(kHueGuides, T("Guides", "Guias")))
    {
        if (Shown(kOptGameGuides))
        {
            bool guides = g.useGameGuides.load();
            if (ImGui::Checkbox(T("Read from the game", "Ler do jogo"), &guides))
            {
                g.useGameGuides.store(guides);
                Log("menu: game guides %s", guides ? "on" : "off");
            }
            Help("D3D11 only. Watches which depth-stencil and which two-channel float target the "
                 "game binds most often, copies each once a frame and carries them across the "
                 "bridge. Turn it off to fall back to estimated motion and no depth -- which is "
                 "what you want when the detector picked the wrong buffer.",

                 "Só D3D11. Observa qual depth-stencil e qual render target float de dois canais "
                 "o jogo mais liga, copia cada um uma vez por quadro e leva pela ponte. Desligue "
                 "para voltar a movimento estimado e nenhuma profundidade -- que é o que você "
                 "quer quando o detector pegou o buffer errado.");
        }

        bool depth = g.useDepth.load();
        if (Shown(kOptDepth))
        {
            if (ImGui::Checkbox(T("Depth", "Profundidade"), &depth))
            {
                g.useDepth.store(depth);
                Log("menu: depth %s", depth ? "on" : "off");
            }
            Help("Hands the depth buffer to the engine and sets the flag that says it is valid. "
                 "Depth mainly buys stability and disocclusion, not sharper texture.",
                 "Entrega o buffer de profundidade ao motor e liga a flag que diz que ele é "
                 "válido. Profundidade compra estabilidade e desoclusão, não textura mais "
                 "afiada.");
        }

        if (depth && Shown(kOptDepthInv))
        {
            bool inv = g.depthInverted.load() != 0;
            if (ImGui::Checkbox(T("Depth inverted", "Profundidade invertida"), &inv))
            {
                g.depthInverted.store(inv ? 1 : 0);
                Log("menu: depth inverted %d", inv ? 1 : 0);
            }
            Help("Which way round the buffer grows. 1 is the runtime's own default. RenoDX sends "
                 "0, measured with a dummy depth, so that 0 says nothing about any real game. "
                 "Nothing here has told the two apart on a real buffer yet -- try both on a scene "
                 "with depth and watch the residual.",

                 "De que lado o buffer cresce. 1 é o padrão do próprio runtime. O RenoDX manda 0, "
                 "medido com profundidade falsa, então esse 0 não diz nada sobre jogo nenhum. "
                 "Nada aqui ainda separou os dois num buffer real -- teste os dois numa cena com "
                 "profundidade e olhe o resíduo.");
        }

        if (depth && Shown(kOptDepthStretch))
        {
            bool dn = g.depthNormalise.load();
            if (Risk r(kWarn, dn);
                ImGui::Checkbox(T("Stretch depth", "Esticar profundidade"), &dn))
            {
                g.depthNormalise.store(dn);
                Log("menu: depth normalise %s", dn ? "on" : "off");
            }
            Help("Multiplies the depth guide by 1/max so a buffer occupying a fraction of 0..1 "
                 "fills the range instead. It shipped ON and it is off now, because on this bench "
                 "it reads as the wrong operation: the emulator's depth already has its bulk at "
                 "the top of its own tiny range (probe: mean 0.00197 against max 0.00200), so "
                 "scaling by 1/max lands nearly every pixel at 0.99 rather than spreading "
                 "anything out -- which under the engine's inverted convention reads as \"the "
                 "whole scene is against the camera\".\n\n"
                 "Compare the 'measure, residual' line with it on and off before trusting it.",

                 "Multiplica a guia de profundidade por 1/max, para que um buffer que ocupa uma "
                 "fração de 0..1 passe a preencher a faixa. Vinha LIGADA e agora vem desligada, "
                 "porque nesta bancada parece ser a operação errada: a profundidade do emulador "
                 "já tem o grosso dos pixels no topo da própria faixa minúscula (probe: média "
                 "0,00197 contra máximo 0,00200), então escalar por 1/max joga quase todo pixel "
                 "em 0,99 em vez de espalhar -- o que sob a convenção invertida do motor lê como "
                 "\"a cena inteira está colada na câmera\".\n\n"
                 "Compare a linha 'measure, residual' com ela ligada e desligada antes de "
                 "confiar.");
        }

        if (Shown(kOptHistory))
        {
            bool hist = g.useHistory.load();
            if (Risk r(kWarn, hist); ImGui::Checkbox(T("History", "Histórico"), &hist))
            {
                g.useHistory.store(hist);
                Log("menu: history %s", hist ? "on" : "off");
            }
            Help("Hands the engine last frame's output to carry forward. Motion vectors say where "
                 "a pixel was; without history there is nothing for them to point at.\n\n"
                 "Experimental: it writes a pointer into the runtime at a fixed offset, and a "
                 "wrong one there hangs the game rather than failing. If the picture smears or "
                 "the game stops responding, this is the first thing to turn off.",

                 "Entrega ao motor a saída do quadro anterior. Vetores de movimento dizem onde um "
                 "pixel estava; sem histórico não há para onde eles apontarem.\n\n"
                 "Experimental: escreve um ponteiro no runtime num offset fixo, e um ponteiro "
                 "errado ali congela o jogo em vez de falhar. Se a imagem borrar ou o jogo parar "
                 "de responder, esta é a primeira coisa a desligar.");
        }

        bool mv = g.useMotion.load();
        if (Shown(kOptMotion))
        {
            if (ImGui::Checkbox(T("Motion", "Movimento"), &mv))
            {
                g.useMotion.store(mv);
                Log("menu: motion %s", mv ? "on" : "off");
            }
            if (g.gameMotionActive)
                Help("Read from the game's own velocity buffer -- the real thing, per pixel.",
                     "Lidos do próprio buffer de velocidade do jogo -- a coisa real, por pixel.");
            else
                Help("Estimated by comparing consecutive frames, because this target has no "
                     "velocity buffer. It is wrong wherever pixels move without the geometry "
                     "moving: reflections, fire, moving shadows, anything appearing from behind "
                     "something else.",

                     "Estimados comparando quadros consecutivos, porque este alvo não tem buffer "
                     "de velocidade. É errado onde pixels se movem sem a geometria se mover: "
                     "reflexos, fogo, sombras em movimento, e qualquer coisa que aparece de trás "
                     "de outra.");
        }

        if (mv && Shown(kOptMotionScale))
        {
            float v = g.motionScale.load();
            if (ImGui::SliderFloat(T("Motion scale", "Escala do mov."), &v, -2.0f, 2.0f, "%.2f", 0))
                g.motionScale.store(v);
            Help("Multiplies the motion field on its way into the engine, whichever field that "
                 "is. 0 says nothing moved, 1 is as measured, -1 flips the direction, 0.5 suits a "
                 "buffer stored in NDC.\n\n"
                 "Set it by eye: put the Debug view on Motion and pan the camera. The field "
                 "should follow steadily. Shimmer means it is too high -- turn it down rather "
                 "than turning motion off, which is the blunt version of the same thing.",

                 "Multiplica o campo de movimento no caminho para o motor, seja qual for o campo. "
                 "0 diz que nada se moveu, 1 é como foi medido, -1 inverte a direção, 0,5 serve "
                 "para um buffer em NDC.\n\n"
                 "Ajuste no olho: ponha a Visão de debug em Movimento e gire a câmera. O campo "
                 "tem que acompanhar, firme. Cintilar quer dizer alto demais -- baixe, em vez de "
                 "desligar o movimento, que é a versão bruta da mesma coisa.");
        }

        if (mv && !g.gameMotionActive && Shown(kOptFlowGate))
        {
            float v = g.flowGate.load();
            if (ImGui::SliderFloat(T("Flow gate", "Portão do fluxo"), &v, 0.002f, 0.10f, "%.3f", 0))
                g.flowGate.store(v);
            Help("The first of two filters deciding which pixels may move, applied BEFORE the "
                 "search. If the brightest and darkest luma in the 3x3 block differ by less than "
                 "this, the pixel is declared still and no search happens.\n\n"
                 "A flat block -- clear sky, a painted wall -- matches equally well at every "
                 "offset, so the winner is whichever the loop tried first. That is the aperture "
                 "problem, and it is why widening the search made the field wilder instead of "
                 "better.\n\n"
                 "Raise it and more of the screen is frozen: cleaner, but the network is told "
                 "nothing moved. 0.020 froze 99% of a dark scene on the bench. The number is a "
                 "luma difference on 0..1, so 0.020 is 2% contrast. Estimated motion only.",

                 "O primeiro de dois filtros que decidem quais pixels podem se mover, aplicado "
                 "ANTES da busca. Se a luma mais clara e a mais escura do bloco 3x3 diferem menos "
                 "que isto, o pixel é declarado parado e nenhuma busca acontece.\n\n"
                 "Um bloco chapado -- céu limpo, parede pintada -- casa igualmente bem em todo "
                 "deslocamento, então o vencedor é o primeiro que o laço testou. É o problema da "
                 "abertura, e é por isso que alargar a busca deixou o campo mais doido em vez de "
                 "melhor.\n\n"
                 "Aumente e mais da tela fica congelada: mais limpo, mas a rede é informada de "
                 "que nada se moveu. 0,020 congelou 99% de uma cena escura na bancada. O número é "
                 "diferença de luma em 0..1, então 0,020 é 2% de contraste. Só vale para "
                 "movimento estimado.");
        }

        if (mv && !g.gameMotionActive && Shown(kOptFlowAccept))
        {
            float v = g.flowRatio.load();
            if (ImGui::SliderFloat(T("Flow accept", "Aceite do fluxo"), &v, 0.50f, 1.00f, "%.2f", 0))
                g.flowRatio.store(v);
            Help("The second filter, applied AFTER the search. The estimator keeps the best of 81 "
                 "candidate offsets and also records the error of not moving at all; the winner "
                 "is believed only if its error is below the standing-still error times this.\n\n"
                 "So it runs backwards from what you would guess: 1.00 is the most PERMISSIVE and "
                 "0.50 is the strictest. Lower means more of the screen is forced still.\n\n"
                 "Gate rejects a block before searching, on the grounds there is nothing in it. "
                 "Accept rejects a result after searching, on the grounds the answer is not "
                 "convincing. Estimated motion only.",

                 "O segundo filtro, aplicado DEPOIS da busca. O estimador fica com o melhor de 81 "
                 "deslocamentos candidatos e também guarda o erro de não se mover; o vencedor só "
                 "é aceito se o erro dele for menor que o erro de ficar parado vezes isto.\n\n"
                 "Ou seja, anda ao contrário do que se imagina: 1.00 é o mais PERMISSIVO e 0,50 é "
                 "o mais rígido. Mais baixo significa mais tela forçada a parada.\n\n"
                 "O Portão rejeita um bloco antes de buscar, com o argumento de que não há nada "
                 "nele. O Aceite rejeita um resultado depois de buscar, com o argumento de que a "
                 "resposta não convence. Só vale para movimento estimado.");
        }
    }

    if (GroupShown(kGrpEngine) && SectionHeader(kHueEngine, T("Engine", "Motor")))
    {
        ImGui::TextWrapped("%s", T("The engine's own option struct. Every offset came from decompiling "
                                   "the runtime's ini reader, not from guesswork -- but an offset being "
                                   "real says nothing about what writing it does. Every default here is "
                                   "the engine's own, so an untouched section changes nothing.",

                                   "A struct de opções do próprio motor. Todo offset veio de decompilar "
                                   "o leitor de ini do runtime, não de chute -- mas um offset ser real "
                                   "não diz nada sobre o que escrever nele faz. Todo padrão aqui é o do "
                                   "próprio motor, então esta seção intocada não muda nada."));

        if (Shown(kOptMask))
        {
            bool mask = g.autoMask.load() != 0;
            if (Risk r(kDanger, !mask);
                ImGui::Checkbox(T("Character mask", "Máscara de personagem"), &mask))
            {
                g.autoMask.store(mask ? 1 : 0);
                Log("menu: automask %d", mask ? 1 : 0);
            }
            Help("UseAutoMask. The engine's semantic character mask, and what makes skin "
                 "structure apply to characters rather than to the whole frame. Defaults to 1; "
                 "this add-on never used to write it, so it has always been on.",
                 "UseAutoMask. A máscara semântica de personagem do motor, e o que faz a "
                 "estrutura da pele valer para personagens em vez do quadro inteiro. Padrão 1; "
                 "este add-on nunca escrevia esse campo, então sempre esteve ligado.");
            if (!mask)
                Note(kDanger, T("Off removes the effect from the whole frame, not just from "
                                "characters -- seen in game. The engine derives its structure and "
                                "tone parameters through this mask, so with it off there is "
                                "nothing left to derive them from.",
                                "Desligada tira o efeito do quadro inteiro, não só dos "
                                "personagens -- visto no jogo. O motor deriva os parâmetros de "
                                "estrutura e tom através dela, então desligada não sobra de onde "
                                "derivar."));
        }

        if (Shown(kOptTemporal))
        {
            int tmode = g.temporalMode.load();
            if (ImGui::Combo(T("Temporal", "Temporal"), &tmode,
                             T("Auto\0Off\0On\0", "Automático\0Desligado\0Ligado\0")))
            {
                g.temporalMode.store(tmode);
                Log("menu: temporal %d", tmode);
            }
            Help("Temporal accumulation. This add-on had the byte labelled 'motion is valid' -- a "
                 "guess that turned out wrong; the engine's ini reader reads the key Temporal "
                 "into it. That explains a measurement nobody could account for: Temporal=1 was "
                 "the only run where the engine reported non-zero motion, which is what "
                 "accumulating over time is for. Auto turns it on whenever a motion field exists.",

                 "Acumulação temporal. Este add-on rotulava esse byte como 'movimento válido' -- "
                 "um chute errado; o leitor de ini do motor lê a chave Temporal nele. Isso "
                 "explica uma medição que ninguém justificava: Temporal=1 foi a única execução em "
                 "que o motor reportou movimento diferente de zero, que é para isso que acumular "
                 "no tempo serve. Automático liga sempre que existe campo de movimento.");
        }

        if (Shown(kOptTonemap))
        {
            int tone = g.tonemap.load();
            if (ImGui::SliderInt(T("Tonemap", "Tonemap"), &tone, -1, 3, "%d", 0))
                g.tonemap.store(tone);
            Help("-1 is automatic for the selected Encoding: with sRGB it sends 0 (off), other "
                 "encodings keep the runtime's own detection. 0..3 override it explicitly.\n\n"
                 "Restart the game after changing this.",
                 "-1 é automático pela Codificação escolhida: em sRGB manda 0 (desligado), as "
                 "outras mantêm a detecção do próprio runtime. 0..3 forçam um valor.\n\n"
                 "Reinicie o jogo depois de mudar.");
        }

        // The slider is 0..1 and not 0..3 because of what the write actually does. Bits 2 and 4
        // stopped being tone channels in v0.2.17: bit 4 is the timeout guard and bit 2 chooses
        // last frame's residual over nothing, so the record path writes `(value & ~2) | 4` and
        // four slider positions collapsed to two -- 2 was byte-for-byte identical to 0, and 3 to
        // 1.
        if (Shown(kOptToneChannels))
        {
            int ch = g.toneChannels.load() & 1;
            if (ImGui::SliderInt(T("Tone channels", "Canais de tom"), &ch, 0, 1, "%d", 0))
                g.toneChannels.store(ch & 1);
            Help("ToneChannels bit 0, the only bit of this field left that means what the name "
                 "says. An ini key of the engine that nothing here knew existed until the reader "
                 "was decompiled. Default 0, effect unknown -- it is here to be A/B'd against the "
                 "residual.\n\n"
                 "The field is four bits wide but the other two are spoken for: the record path "
                 "holds bit 4 set, because with the whole word at 0 the runtime zeroes local tone "
                 "and local structure before they reach the network, whatever the sliders above "
                 "say.",

                 "ToneChannels bit 0, o único bit deste campo que ainda quer dizer o que o nome "
                 "diz. Uma chave de ini do motor que ninguém aqui sabia que existia até o leitor "
                 "ser decompilado. Padrão 0, efeito desconhecido -- está aqui para ser testada em "
                 "A/B contra o resíduo.\n\n"
                 "O campo tem quatro bits mas os outros dois têm dono: o caminho de gravação "
                 "segura o bit 4 ligado, porque com a palavra inteira em 0 o runtime zera tom "
                 "local e estrutura local antes deles chegarem na rede, digam o que disserem os "
                 "sliders.");
        }

        if (Shown(kOptOutputScale))
        {
            float es = g.engineScale.load();
            if (Risk r(kDanger, es < 0.03124f || es > 0.03126f);
                ImGui::SliderFloat(T("Output scale", "Escala de saída"), &es, 0.0001f, 1.0f,
                                   "%.5f", 0))
                g.engineScale.store((std::max)(es, 0.0001f));
            ImGui::SameLine();
            // A 0..1 slider at five decimals cannot be dragged back onto exactly 1/32.
            if (ImGui::SmallButton("1/32"))
            {
                g.engineScale.store(0.03125f);
                Log("menu: output scale back to the runtime's default 0.03125");
            }
            Help("The runtime's ini key 'Scale', default 1/32. Not a control of the network: it "
                 "sits after the four control floats and goes to the post kernel that writes the "
                 "output.\n\n"
                 "Near 0 the network's answer never reaches the frame -- measured with a residual "
                 "mean of 0.00024 against an input of 0.45, with enabled and disabled identical "
                 "and every frame reported as processed. The slider will not go to 0 and the ini "
                 "refuses it too; Intensity 0 is the way to see the game's own frame.",

                 "A chave 'Scale' do ini do runtime, padrão 1/32. Não é controle da rede: fica "
                 "depois dos quatro floats de controle e vai para o kernel de pós que escreve a "
                 "saída.\n\n"
                 "Perto de 0 a resposta da rede nunca chega no quadro -- medido com resíduo médio "
                 "de 0,00024 contra entrada de 0,45, com ligado e desligado idênticos e todo "
                 "quadro dado como processado. O slider não vai a 0, e o ini também recusa; "
                 "Intensidade 0 é o jeito de ver o quadro do próprio jogo.");
        }

        ImGui::TextDisabled(T("Restart-only: Stage=%d Events=%d NoBridge=%d NoBackBuffer=%d",
                              "Só na reinicialização: Stage=%d Events=%d NoBridge=%d "
                              "NoBackBuffer=%d"),
                            g.stage.load(), g.events, g.noBridge.load() ? 1 : 0,
                            g.noBackBuffer.load() ? 1 : 0);
        Help("Diagnostics that decide what the helper builds at startup, so they cannot be "
             "changed live -- set them in amd-nr.ini. Stage=1 stops before D3D12 loads, 2 before "
             "the engine, 3 is everything. Events is a bitmask: 1 bind, 2 draw, 4 clear, 8 "
             "destroy_swapchain, 16 overlay.",

             "Diagnósticos que decidem o que o ajudante constrói na inicialização, então não "
             "mudam ao vivo -- ajuste no amd-nr.ini. Stage=1 para antes da D3D12 carregar, 2 "
             "antes do motor, 3 é tudo. Events é máscara de bits: 1 bind, 2 draw, 4 clear, 8 "
             "destroy_swapchain, 16 overlay.");
    }

    ImGui::EndDisabled();

    // The cascade. Everything this panel leaves off is one tick away, one control at a time,
    // under the header it will appear beneath -- so turning something on tells you where to look
    // for it. Nothing here is a second copy of anything: the same wire field, the same ini key on
    // the helper's side. The only thing a bit decides is whether a widget is drawn.
    if (ImGui::TreeNode(T("More settings", "Mais ajustes")))
    {
        ImGui::TextWrapped("%s", T("Off screen by default, not off. Each of these reads and writes its "
                                   "own key in amd-nr.ini either way; ticking one only puts a control "
                                   "for it in the panel.",

                                   "Fora da tela por padrão, não desligados. Cada um destes lê e "
                                   "escreve a chave dele no amd-nr.ini de qualquer jeito; marcar um só "
                                   "põe um controle para ele no painel."));

        uint32_t bits = g.optional.load();
        const uint32_t before = bits;
        for (int grp = 0; grp < kGrpCount; ++grp)
        {
            ImGui::SeparatorText(OptGroupName(grp));
            for (const OptRow &row : kOpts)
            {
                if (row.group != grp)
                    continue;
                bool onNow = (bits & row.bit) != 0;
                if (ImGui::Checkbox(T(row.en, row.pt), &onNow))
                    bits = onNow ? (bits | row.bit) : (bits & ~row.bit);
            }
        }
        ImGui::Separator();
        if (ImGui::Button(T("All", "Todos")))
            bits = AllOpts();
        ImGui::SameLine();
        if (ImGui::Button(T("None", "Nenhum")))
            bits = 0;
        if (bits != before)
        {
            g.optional.store(bits);
            Log("menu: optional controls now 0x%06x", bits);
        }
        ImGui::TreePop();
    }

    ImGui::Separator();
    // Four requests, all of them posted rather than done: the ini and the engine belong to the
    // helper, and nothing in this callback is allowed to talk on the pipe.
    if (ImGui::Button(T("Save", "Salvar")))
        controls.save=true;
    Help("Writes everything to amd-nr.ini next to the exe. You do not have to press it -- "
         "every control saves itself when you let go. This writes now, and logs that it happened.",
         "Escreve tudo no amd-nr.ini ao lado do exe. Você não precisa apertar -- cada controle se "
         "salva sozinho quando você solta. Isto escreve agora, e deixa registro no log.");
    ImGui::SameLine();
    if (ImGui::Button(T("Reload", "Recarregar")))
    {
        controls.reload=true;
        Log("menu: settings reloaded from the ini");
    }
    Help("Re-reads amd-nr.ini, discarding anything changed here since the last save. This is how "
         "a setting edited in the file is picked up without restarting the game.",
         "Relê o amd-nr.ini, descartando qualquer coisa mudada aqui desde o último salvamento. É "
         "assim que um ajuste editado no arquivo é aplicado sem reiniciar o jogo.");

    // Asking somebody for a log means asking them to find the emulator's install directory first
    // -- and on this route there are two logs, in two directories. The save goes with it so the
    // ini beside them is the run they describe.
    if (ImGui::Button(T("Export logs to desktop", "Exportar logs pra área de trabalho")))
    {
        controls.save=true;
        controls.exportLogs=true;
    }
    Help("Copies amd-nr-x86.log, the helper's amd-nr-x86-host.log, the runtime's "
         "dlssnr_on_amd.log, ReShade.log and amd-nr.ini into a dated folder on your desktop. The "
         "settings go with them because a log without them cannot be compared against "
         "anything.\n\n"
         "The logs are truncated every time the game starts, so export before relaunching.",

         "Copia o amd-nr-x86.log, o amd-nr-x86-host.log do ajudante, o dlssnr_on_amd.log do "
         "runtime, o ReShade.log e o amd-nr.ini para uma pasta datada na sua área de trabalho. Os "
         "ajustes vão junto porque um log sem eles não dá para comparar com nada.\n\n"
         "Os logs são truncados toda vez que o jogo abre, então exporte antes de relançar.");
    if (controls.exportFailed)
        ImGui::TextColored(kDanger, "%s", T("Could not write to the desktop.",
                                            "Não deu para escrever na área de trabalho."));
    else if (!controls.exportedPath.empty())
        ImGui::TextColored(kOk, T("Exported to %ls", "Exportado para %ls"),
                           controls.exportedPath.c_str());

    ImGui::SameLine();
    if(ImGui::Button(T("Factory Defaults", "Padrões de Fábrica")))controls.factory=true;
    Help("Restores the tuning to what it ships with. Enabled, startup, hotkey and language are "
         "preserved. It lands in the ini with everything else, on its own.",
         "Restaura os ajustes de fábrica. Ligado, inicialização, atalho e idioma são preservados. "
         "Vai para o ini junto com o resto, sozinho.");

    // The legend, which used to be six paragraphs and a four-entry tag table. One wrapped line:
    // the tags are gone, and red and amber are the only marks a control can carry now.
    ImGui::TextDisabled("%s", T("Red: risks the display driver at this value. Amber: past what was "
                                "measured here.",
                                "Vermelho: neste valor arrisca o driver de vídeo. Âmbar: além do "
                                "que foi medido aqui."));
}

} // namespace overlay32
void OverlayCandidateStatus(){
    ImGui::Text("Depth candidate=%d %ux%u format=%u last_capture_valid=%d",g.guideDepth.chosen.Get()!=nullptr,g.guideDepth.width,g.guideDepth.height,static_cast<unsigned>(g.guideDepth.format),g.guideDepth.ready);
    ImGui::Text("Motion candidate=%d %ux%u format=%u last_capture_valid=%d",g.guideMotion.chosen.Get()!=nullptr,g.guideMotion.width,g.guideMotion.height,static_cast<unsigned>(g.guideMotion.format),g.guideMotion.ready);
}
namespace overlay32 { void CandidateStatus(){OverlayCandidateStatus();} }
void OnOverlay32(effect_runtime* runtime){
    // Never block on Present, IPC, or GPU. Present also serializes teardown against this state.
    std::unique_lock<std::mutex> guard(g.lock,std::try_to_lock);
    if(!guard.owns_lock()){ImGui::TextDisabled("x86 bridge is processing the current frame...");return;}
    controls.overlayAt=GetTickCount64();
    // The only place ReShade hands a runtime over, and the key scan on the present path needs
    // one to read its key state.
    controls.runtime=runtime;
    controls.syncRequested=true;
    if(!controls.synced){
        ImGui::TextDisabled(g.failed?"x64 host unavailable. See amd-nr-x86.log.":"Synchronizing with x64 host...");
        if(g.failed&&ImGui::Button("Retry host synchronization"))g.failed=false;
        return;
    }
    overlay32::Import(controls.shadow,controls.status);
    overlay32::Draw();
    auto edited=overlay32::Export();
    if(std::memcmp(&edited,&controls.shadow,sizeof(edited))!=0){
        edited.settings_revision=controls.shadow.settings_revision+1;controls.shadow=edited;OperationalSettings();
    }
    // Autosave. Armed when a control settles rather than while it is being dragged: the host rewrites
    // the whole ini once per key, so a slider held down would be a file rewrite per frame. The Save
    // button stays -- it is what puts the line in the host's log -- but nothing is lost without it.
    if(controls.shadow.settings_revision!=controls.savedRevision&&!ImGui::IsAnyItemActive())
        controls.save=true;
}
