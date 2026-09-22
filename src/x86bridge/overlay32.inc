// UI adapted from src/neural/neural.cpp, copyright (c) 2026 cLohan, MIT.
// All original upstream files remain unchanged. This file has only local UI state.
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

// How well each control is actually known, printed next to it. The question "which of these
// does anything in the game" had to be answered by reading the source or three handoffs, and
// the honest answer is not the same for any two controls. So it goes in the panel:
//
//   MEASURED  a 'measure, residual' reading moved when this changed. It does something.
//   TRACED    the write reaches a consumer -- a decompiled reader, or our own shader -- but no
//             reading here has ever separated it from its default.
//   UNKNOWN   written into a real engine field whose effect nobody here has established.
//   INERT     swept and measured to change nothing. Kept only to be re-checked elsewhere.
enum Known { kMeasured, kTraced, kUnknown, kInert };
void Tag(Known k)
{
    static const ImVec4 colours[] { { 0.4f, 1.0f, 0.4f, 1.0f },  { 0.55f, 0.75f, 1.0f, 1.0f },
                                    { 1.0f, 0.80f, 0.30f, 1.0f }, { 0.6f, 0.6f, 0.6f, 1.0f } };
    static const char *en[] { "MEASURED", "TRACED", "UNKNOWN", "INERT" };
    static const char *pt[] { "MEDIDO", "RASTREADO", "DESCONHECIDO", "INERTE" };
    ImGui::SameLine();
    ImGui::TextColored(colours[k], "%s", T(en[k], pt[k]));
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
    const int key = g.toggleKey.load(), mods = g.toggleMods.load();
    std::string out;
    if (mods & 1) out += "Ctrl+";
    if (mods & 2) out += "Alt+";
    if (mods & 4) out += "Shift+";
    if (key == 0)
        return out.empty() ? "unbound" : out + "?";

    UINT sc = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    switch (key)
    {
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 0x100;
        break;
    default:
        break;
    }
    wchar_t wide[64] {};
    if (sc != 0 && GetKeyNameTextW(static_cast<LONG>(sc) << 16, wide, 64) > 0)
    {
        char name[128] {};
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), nullptr, nullptr) > 0)
            return out + name;
    }
    char fallback[16] {};
    std::snprintf(fallback, sizeof(fallback), "VK 0x%02X", key);
    return out + fallback;
}

void StatusLine(){
    const auto& s=controls.status;
    ImGui::Text("%s | Host %s | Engine %s",g.enabled.load()?T("Enabled","Ligado"):T("Off","Desligado"),s.connected?"connected":"disconnected",s.transportOnly?"unavailable (transport)":s.failed?"failed":s.unavailable?"unavailable":s.engineReady?"ready":"not initialized");
    ImGui::Text(T("Processed: %llu | Skipped: %llu", "Processados: %llu | Pulados: %llu"),static_cast<unsigned long long>(s.processed),static_cast<unsigned long long>(s.skipped));
}
void Draw()
{
    bool on = g.enabled.load();
    if (ImGui::Checkbox(T("Enabled", "Ligado"), &on))
    {
        g.enabled.store(on);
        Log("menu: %s", on ? "on" : "off");
    }
    ImGui::SameLine();
    const std::string hotkey = HotkeyName();
    ImGui::TextDisabled("(%s)", hotkey.c_str());
    ImGui::SameLine();
    StatusLine();

    {
        bool start = g.startOn.load();
        if (ImGui::Checkbox(T("Enabled from the first frame", "Ligado desde o primeiro quadro"),
                            &start))
        {
            g.startOn.store(start);
            Log("menu: start enabled %d", start ? 1 : 0);
        }
        Help("Whether Enabled above is already ticked when the game opens, instead of waiting "
             "for the hotkey every time. `StartOn=1` in amd-nr.ini.\n\n"
             "For a game that takes a while to get back into, pressing a key every launch is "
             "work for nothing. Set this once and it stays set.",

             "Se o Ligado aí em cima já vem marcado quando o jogo abre, em vez de esperar a "
             "tecla de atalho toda vez. `StartOn=1` no amd-nr.ini.\n\n"
             "Em jogo que dá trabalho pra voltar, apertar tecla todo lançamento é trabalho à "
             "toa. Marque uma vez e fica.");

        bool altTab = g.disableOnAltTab.load();
        if (ImGui::Checkbox(T("Disable the effect on alt-tab",
                              "Desativar o efeito ao dar alt-tab"), &altTab))
        {
            g.disableOnAltTab.store(altTab);
            Log("menu: disable on alt-tab %d", altTab ? 1 : 0);
        }
        Help("Switch the effect off the moment the game stops being the window in front, and "
             "leave it off. Coming back to the game, it is still off: turn it on with the hotkey "
             "or the box above when you want it. `DisableOnAltTab=1` in amd-nr.ini.\n\n"
             "This does not resume by itself, on purpose. Sitting out a minimised window is a "
             "separate safety that is always on and does resume, because frames nobody can see "
             "are not a decision anyone needs to make.",

             "Desliga o efeito no instante em que o jogo deixa de ser a janela da frente, e "
             "deixa desligado. Ao voltar pro jogo ele continua desligado: você liga na tecla de "
             "atalho ou na caixa aí em cima quando quiser. `DisableOnAltTab=1` no "
             "amd-nr.ini.\n\n"
             "Não volta sozinho, de propósito. Pular quadros de janela minimizada é outra coisa, "
             "uma proteção que está sempre ligada e essa sim volta sozinha — quadro que ninguém "
             "vê não é decisão de usuário.");

        // Rebinding by capturing a real keypress, rather than by typing a virtual-key code.
        // Only advances while the overlay is open, which is where the button is.
        ImGui::TextUnformatted(T("Toggle hotkey", "Tecla de atalho"));
        ImGui::SameLine();
        if (ImGui::Button(controls.capture.armed
                              ? T("press a key (Esc cancels)", "aperte uma tecla (Esc cancela)")
                              : hotkey.c_str()))
            controls.capture.Toggle();
        Help("Click, then press the combination you want. Modifiers held at that moment are part "
             "of the binding. Esc cancels and keeps the current one.\n\n"
             "Saved to the ini as ToggleKey (a virtual-key code) and ToggleMods (1 Ctrl, 2 Alt, "
             "4 Shift, added together). A key with no modifier is allowed and will fire during "
             "normal play, so pick one the game does not use.",

             "Clique e aperte a combinação que quiser. Os modificadores segurados nesse momento "
             "fazem parte do atalho. Esc cancela e mantém o atual.\n\n"
             "Salvo no ini como ToggleKey (código de tecla virtual) e ToggleMods (1 Ctrl, 2 Alt, "
             "4 Shift, somados). Uma tecla sem modificador é permitida e vai disparar durante o "
             "jogo normal, então escolha uma que o jogo não use.");

        // The key scan itself runs on the present path in frontend32.cpp, not here: this
        // callback is not called every frame, and a scan that only advances inside it never
        // outlived the cancel timer. This button only arms it.
    }

    int lang = g.language.load();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::Combo(T("Language", "Idioma"), &lang, "English\0Português (Brasil)\0"))
    {
        g.language.store(lang);
        Log("menu: language %d", lang);
    }

    ImGui::BeginDisabled(controls.status.transportOnly!=0);
    if (ImGui::CollapsingHeader(T("Image", "Imagem"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        int enc = g.encoding.load();
        if (ImGui::Combo(T("Encoding", "Codificação"), &enc, "sRGB\0Linear\0scRGB-nl\0"))
        {
            g.encoding.store(enc);
            Log("menu: encoding %d", enc);
        }
        Help("Conversion applied before the runtime and reversed during composition.\n\n"
             "Use sRGB for an SDR frame: it passes the code values through unchanged, and "
             "automatic Tonemap stays off even though the transport texture is FP16. "
             "Diffuse White has no effect in this mode. Linear and scRGB-nl are experimental "
             "conversion paths, not evidence of the model's training domain.\n\n"
             "Restart the game after changing Encoding or Tonemap; the runtime caches its "
             "input conversion when it creates staging resources.",

             "Conversão aplicada antes do runtime e revertida na composição.\n\n"
             "Use sRGB para um quadro SDR: os valores passam sem conversão, e o Tonemap "
             "automático fica desligado mesmo com transporte em FP16. Branco Difuso não "
             "tem efeito nesse modo. Linear e scRGB-nl são conversões experimentais; não "
             "comprovam o domínio em que o modelo foi treinado.\n\n"
             "Reinicie o jogo após alterar Codificação ou Tonemap; o runtime guarda a "
             "conversão ao criar os recursos de entrada.");
        Tag(kMeasured);

        ImGui::BeginDisabled(g.encoding.load() == 0);
        float white = g.diffuseWhite.load();
        if (ImGui::SliderFloat(T("Diffuse White", "Branco Difuso"), &white, 80.0f, 1000.0f,
                               "%.0f nits", 0))
            g.diffuseWhite.store(white);
        ImGui::EndDisabled();
        Help("How many nits a value of 1.0 means, which sets the scale of the linear image the "
             "network is handed.\n\n"
             "The documented automatics are 100 for linear BT.709, 203 for scRGB-nl and 250 for "
             "PQ or scRGB linear. Anything else is a guess, and a wrong scale here looks like the "
             "network over- or under-reacting everywhere at once.\n\n"
             "Inert while Encoding is sRGB.",

             "Quantos nits um valor de 1.0 significa, o que define a escala da imagem linear "
             "entregue à rede.\n\n"
             "Os automáticos documentados são 100 para linear BT.709, 203 para scRGB-nl e 250 "
             "para PQ ou scRGB linear. Qualquer outro valor é chute, e uma escala errada aqui "
             "parece a rede reagindo demais ou de menos em tudo ao mesmo tempo.\n\n"
             "Inerte enquanto a Codificação for sRGB.");
        Tag(kTraced);

        float v = g.intensity.load();
        if (ImGui::SliderFloat(T("Overall Intensity", "Intensidade Geral"), &v, 0.0f, 2.0f,
                               "%.2f", 0))
            g.intensity.store(v);
        Help("Blends the network's correction over the game's image, after the fact.\n\n"
             "This is a mix, not a parameter of the network: it can show more or less of what the "
             "network did, never make it do more. 0.00 is the same picture as switching the "
             "add-on off, which makes it the fastest A/B there is.",

             "Mistura a correção da rede sobre a imagem do jogo, depois do fato.\n\n"
             "Isto é uma mistura, não um parâmetro da rede: pode mostrar mais ou menos do que a "
             "rede fez, nunca fazer ela fazer mais. 0.00 dá a mesma imagem que desligar o add-on, "
             "o que faz dele o A/B mais rápido que existe.");
        Tag(kMeasured);

        // How the correction is put back onto the frame. This is the arrangement Pass Count
        // needed: additive composition made a second pass mean twice the difference, clipped per
        // channel, and a clipped channel is a hue rotation rather than more detail.
        {
            int comp = g.ratioGuard.load() > 0.0f ? 1 : 0;
            const char *items[] = { T("Additive (old)", "Aditiva (antiga)"),
                                    T("Ratio (bounded)", "Razão (limitada)") };
            if (ImGui::Combo(T("Composition", "Composição"), &comp, items, 2))
            {
                g.ratioGuard.store(comp == 1 ? 2.0f : 0.0f);
                Log("menu: composition %s", comp == 1 ? "ratio" : "additive");
            }
            Help("How the network's answer is put back onto the frame.\n\n"
                 "Additive adds the correction to the picture, channel by channel, and clips "
                 "whatever leaves the range. That is what this add-on always did, and it is why "
                 "Pass Count was useless: two passes is twice the difference and three is three "
                 "times it, and a clipped channel is a hue rotation -- so the count did not read "
                 "as more detail, it read as more saturation and then as a mess.\n\n"
                 "Ratio turns the answer into a picture of its own, compares its luminance "
                 "against the frame's as a ratio, bounds that ratio, and blends two finished "
                 "pictures. A bounded ratio cannot move hue. This is what the OptiScaler DLSS-NR "
                 "fork does, and RenoDX's DLSS 5 addon before it.\n\n"
                 "Kept switchable so both can be seen in one session. Ratio is the default.",

                 "Como a resposta da rede volta para o quadro.\n\n"
                 "Aditiva soma a correção na imagem, canal por canal, e corta o que sair da "
                 "faixa. É o que este add-on sempre fez, e é por isso que o Número de Passes não "
                 "servia: dois passes é o dobro da diferença e três é o triplo, e canal cortado é "
                 "rotação de matiz -- então a contagem não aparecia como mais detalhe, aparecia "
                 "como mais saturação e depois como sujeira.\n\n"
                 "Razão transforma a resposta numa imagem própria, compara a luminância dela com "
                 "a do quadro como razão, limita essa razão, e mistura duas imagens inteiras. Uma "
                 "razão limitada não move matiz. É o que o fork DLSS-NR do OptiScaler faz, e o "
                 "addon DLSS 5 do RenoDX antes dele.\n\n"
                 "Deixado trocável para dar para ver os dois na mesma sessão. Razão é o padrão.");
            Tag(kTraced);
        }

        ImGui::BeginDisabled(g.ratioGuard.load() <= 0.0f);
        v = g.colourStrength.load();
        if (ImGui::SliderFloat(T("Colour Strength", "Força da Cor"), &v, 0.0f, 1.0f, "%.2f", 0))
            g.colourStrength.store(v);
        Help("Whether the network's colour arrives with its light.\n\n"
             "0 keeps the game's own hue exactly: every pixel is the original colour and only its "
             "brightness carries what the network decided. This is the setting for \"it changed "
             "the colours\" -- at 0 it cannot, by construction. 1 brings the network's colour "
             "with it.\n\n"
             "It cannot shift hue on its own either way: both ends of the blend carry the same "
             "luminance, so this moves chroma and nothing else.\n\n"
             "Inert on Additive composition.",

             "Se a cor da rede vem junto com a luz dela.\n\n"
             "0 mantém a matiz do jogo exatamente: cada pixel fica com a cor original e só o "
             "brilho carrega o que a rede decidiu. É este o controle para \"mudou as cores\" -- "
             "em 0 ele não consegue mudar, por construção. 1 traz a cor da rede junto.\n\n"
             "Também não desloca matiz sozinho nos dois sentidos: as duas pontas da mistura "
             "carregam a mesma luminância, então isto mexe em croma e mais nada.\n\n"
             "Inerte na composição Aditiva.");
        Tag(kTraced);

        v = g.ratioGuard.load();
        if (v > 0.0f && ImGui::SliderFloat(T("Highlight Guard", "Trava de Realce"), &v, 1.0f, 8.0f,
                                           "%.1fx", 0))
            g.ratioGuard.store(v);
        Help("The most compose may move any pixel, as a multiple of what it already was, in both "
             "directions. A pixel may not be brightened past this nor darkened past its "
             "reciprocal.\n\n"
             "One scalar, taken from luminance and applied to the whole triple, so it bounds "
             "brightness without touching hue. Lights are where the network has least to say and "
             "where an unbounded answer does the most damage.\n\n"
             "2.0x is the reference fork's default and leaves detail intact. Raise it only if "
             "bright areas look clipped.",

             "O máximo que a composição pode mover um pixel, como múltiplo do que ele já era, nos "
             "dois sentidos. Um pixel não pode ser clareado além disto nem escurecido além do "
             "inverso.\n\n"
             "Um escalar só, tirado da luminância e aplicado no trio inteiro, então limita brilho "
             "sem tocar em matiz. Luzes são onde a rede tem menos a dizer e onde uma resposta sem "
             "limite estraga mais.\n\n"
             "2.0x é o padrão do fork de referência e não come detalhe. Só aumente se áreas "
             "claras parecerem estouradas.");
        Tag(kTraced);

        bool track = g.guardTracksPasses.load();
        if (ImGui::Checkbox(T("Guard follows Pass Count", "Trava acompanha o Número de Passes"),
                            &track))
        {
            g.guardTracksPasses.store(track);
            Log("menu: guard follows pass count %d", track ? 1 : 0);
        }
        Help("Adds one multiple of headroom per extra pass, so 2.0x becomes 3.0x at two passes "
             "and 4.0x at three. Off by default.\n\n"
             "The guard is applied once, to the finished composition, while the passes compound "
             "the ratio inside it. Left fixed, the third pass spends most of its contribution "
             "against the clamp -- it costs a whole extra network run and most of it is thrown "
             "away. The reference fork's tooltip suggests raising the guard by hand with the "
             "count; this does it for you.\n\n"
             "It is off because the reference does not actually do it. Nine OptiScaler logs off "
             "RTX machines carry 254 composition lines across seven games, at one pass and at "
             "two, and every one of them reads guard 2.0x -- the single 1.5x in the set is "
             "someone dragging the slider down. Their ini says MaxRatio defaults to 2.0 and "
             "never mentions the count. A fixed bound is also the honest way to see what an "
             "extra pass is contributing.",

             "Acrescenta um múltiplo de folga por passe extra, então 2.0x vira 3.0x em dois "
             "passes e 4.0x em três. Desligado por padrão.\n\n"
             "A trava é aplicada uma vez, na composição pronta, enquanto os passes acumulam a "
             "razão dentro dela. Fixa, o terceiro passe gasta quase toda a contribuição dele "
             "contra o limite -- custa uma rodada inteira da rede e joga a maior parte fora. O "
             "tooltip do fork de referência sugere subir a trava na mão junto com a contagem; "
             "isto faz por você.\n\n"
             "Está desligado porque o fork de referência não faz isso de verdade. Nove logs de "
             "OptiScaler de máquinas RTX trazem 254 linhas de composição em sete jogos, em um "
             "passe e em dois, e todas dizem guard 2.0x -- o único 1.5x do conjunto é alguém "
             "baixando o slider. A ini deles diz que MaxRatio tem padrão 2.0 e nunca cita a "
             "contagem. Um limite fixo também é o jeito honesto de ver o que um passe extra "
             "está somando.");
        Tag(kMeasured);
        ImGui::EndDisabled();

        v = g.residualLimit.load();
        if (ImGui::SliderFloat(T("Residual Limit", "Limite do Resíduo"), &v, 0.0f, 0.50f,
                               v <= 0.0f ? T("off", "desligado") : "%.3f", 0))
            g.residualLimit.store(v);
        Help("Caps how far the correction may push a single pixel, as a fraction of white. 0 is "
             "off and nothing is capped.\n\n"
             "This is the control for the blown blocks. A measured two-pass run on a reference capture "
             "reported a mean correction of 0.072 with a MAXIMUM OF 4.16 -- four times brighter "
             "than white, in a picture whose own mean is 0.13. That is not something the network "
             "saw; it is a tile where it extrapolated. Every extra pass then runs on top of that "
             "blown tile, so it compounds instead of averaging away.\n\n"
             "The whole correction is scaled, not clamped per channel: clamping one channel of a "
             "triple is a hue rotation, which turned a blown block into a blown coloured one.\n\n"
             "0.25 by default, which is over three times the typical correction, so an ordinary "
             "pixel never meets it. Lower it until the blocks go; too low flattens the whole "
             "effect, which Residual x8 shows immediately.",

             "Limita o quanto a correção pode empurrar um pixel, como fração do branco. 0 é "
             "desligado e nada é limitado.\n\n"
             "É este o controle dos blocos estourados. Uma medição de dois passes no a reference capture "
             "deu correção média 0.072 com MÁXIMO DE 4.16 -- quatro vezes mais claro que o "
             "branco, numa imagem cuja média é 0.13. Isso não é algo que a rede viu; é um bloco "
             "onde ela extrapolou. Cada passe extra roda em cima desse bloco estourado, então "
             "acumula em vez de diluir.\n\n"
             "A correção inteira é escalada, não cortada canal por canal: cortar um canal de um "
             "trio é rotação de matiz, o que transformava bloco estourado em bloco estourado e "
             "colorido.\n\n"
             "0.25 por padrão, que é mais de três vezes a correção típica, então pixel normal "
             "nunca encosta nele. Baixe até os blocos sumirem; baixo demais achata o efeito "
             "inteiro, o que o Resíduo x8 mostra na hora.");
        Tag(kTraced);

        v = g.residualFade.load();
        if (ImGui::SliderFloat(T("Edge Fade", "Suavizar Bordas"), &v, 0.0f, 0.25f,
                               v <= 0.0f ? T("off", "desligado") : "%.3f", 0))
            g.residualFade.store(v);
        Help("Rolls the correction off to nothing over a band at the frame border, given as a "
             "fraction of the frame. 0.02 is a 2% band -- about 20 pixels at 1080p. 0 is off.\n\n"
             "The tiles at the border have no neighbour on one side, so what the network returns "
             "there is invented rather than seen, and bicubic upsampling rings on top of it. That "
             "is the glitching in the corners: a corner is inside two border bands at once, so it "
             "gets both rolloffs and is the first place to go wrong and the first place this "
             "fixes.\n\n"
             "Cheaper than turning the whole effect down, because it only touches the band.",

             "Vai apagando a correção até zero numa faixa na borda do quadro, dada como fração do "
             "quadro. 0.02 é uma faixa de 2% -- uns 20 pixels em 1080p. 0 é desligado.\n\n"
             "Os blocos da borda não têm vizinho de um lado, então o que a rede devolve ali é "
             "inventado, não visto, e o upsample bicúbico ainda toca sino em cima. É isso o "
             "glitch nos cantos: um canto está dentro de duas faixas de borda ao mesmo tempo, "
             "então leva as duas quedas -- é o primeiro lugar a estragar e o primeiro que isto "
             "conserta.\n\n"
             "Mais barato que baixar o efeito inteiro, porque só toca na faixa.");
        Tag(kTraced);

        v = g.structure.load();
        if (ImGui::SliderFloat(T("Structure Intensity", "Intensidade de Estrutura"), &v, 0.0f,
                               3.0f, "%.2f", 0))
            g.structure.store(v);
        Help("Written into the engine at a fixed offset, and the one control measured to matter.\n\n"
             "Measured on a reference capture: at 0 the residual collapses 25x, which is the proof that "
             "what reaches the screen comes from the network at all. From 1 to 3 the magnitude "
             "grows about 6% and the structure ratio goes 0.18 to 0.28. It saturates; 3 is the "
             "useful end of it.",

             "Escrito no motor num offset fixo, e o único controle medido como relevante.\n\n"
             "Medido no a reference capture: em 0 o resíduo despenca 25x, o que é a prova de que o que "
             "chega na tela vem da rede. De 1 para 3 a magnitude cresce uns 6% e a razão de "
             "estrutura vai de 0,18 para 0,28. Satura; 3 é o fim útil dele.");
        Tag(kMeasured);

        v = g.skin.load();
        if (ImGui::SliderFloat(T("Skin Structure Strength", "Força de Estrutura na Pele"), &v,
                               -1.0f, 3.0f, "%.2f", 0))
            g.skin.store(v);
        Help("Same kind of offset, aimed at skin. Measured worth about 1.5% of the residual, "
             "which is close to run-to-run noise.\n\n"
             "-1.00 is the sentinel the engine itself boots with, and it is the strongest "
             "evidence anything reads this field at all: it means automatic. Writing 1.00 over "
             "it, which this add-on used to do on startup, turns the automatic off before you "
             "ever touch the slider.",

             "Mesmo tipo de offset, mirado na pele. Medido valendo uns 1,5% do resíduo, o que "
             "está perto do ruído entre execuções.\n\n"
             "-1.00 é a sentinela com que o próprio motor liga, e é a evidência mais forte de "
             "que alguém lê esse campo: significa automático. Escrever 1.00 por cima, o que este "
             "add-on fazia no início, desliga o automático antes de você tocar no slider.");
        Tag(kMeasured);
    }

    if (ImGui::CollapsingHeader(T("Performance", "Desempenho"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Same-frame timing is what turns every other cost into a stall, so it is coloured
        // exactly when there is a cost big enough for that to matter.
        const float sc=g.scale.load();
        // Reads and writes the frontend's own flag rather than a shadow field, so it cannot drift
        // from what the bridge is actually doing and needs no round trip to the helper.
        int timing=::g.async?1:0;
        // Live. Switching is safe in both directions because CollectPending runs before
        // anything else each present, so neither mode ever starts with work outstanding.
        if(ImGui::Combo(T("Timing", "Momento"), &timing, "Same frame (inline)\0Async (previous frame)\0"))
            ::SetAsync(timing==1);
        Help("Async posts the frame and composes the answer in "
             "the next present, so the helper works while the game builds its next frame. It is "
             "faster and it costs one frame of lag. It does not smear: the whole picture is replaced, "
             "so you see the previous frame finished rather than a mix of two. Changing this takes "
             "effect at once and is saved to amd-nr.ini.",
             "No modo assíncrono o quadro é enviado e a "
             "resposta é composta no present seguinte, então o ajudante trabalha enquanto o jogo "
             "monta o próximo quadro. É mais rápido e custa um quadro de atraso. Não borra: a imagem "
             "inteira é substituída, então você vê o quadro anterior pronto, não uma mistura de dois. "
             "A troca vale na hora e fica salva no amd-nr.ini.");
        // Preview while dragging, but commit only once when the edit ends. Re-staging the
        // network raster on every mouse movement leaves retired runtime/driver allocations
        // resident and can quickly exhaust VRAM.
        static float editingScale = g.scale.load();
        static bool editingScaleActive = false;
        if (!editingScaleActive)
            editingScale = g.scale.load();
        float v = editingScale;
        if (Risk r(v > 1.0f && g.inlineMode.load() ? kDanger : kWarn, v > 0.50f);
            ImGui::SliderFloat(T("Resolution Scale", "Escala de Resolução"), &v, 0.25f, 2.0f,
                               "%.2f", 0))
            editingScale = v;
        if (ImGui::IsItemActive())
            editingScaleActive = true;
        if (ImGui::IsItemDeactivated())
        {
            if (editingScaleActive && editingScale != g.scale.load())
                g.scale.store(editingScale);
            editingScaleActive = false;
        }
        v = editingScale;
        Help("Network width and height relative to the game frame. 0.50 uses a quarter of the pixels; "
             "1.00 uses the full frame. Lower scales reduce fine detail and inference cost. "
             "Materials can still change below 1.00. Measure frame time at the chosen resolution.",
             "Largura e altura da rede em relação ao quadro do jogo. 0.50 usa um quarto dos pixels; "
             "1.00 usa o quadro inteiro. Escalas menores reduzem detalhe fino e custo da inferência. "
             "Materiais ainda podem mudar abaixo de 1.00. Meça o tempo de quadro na resolução escolhida.");
        Tag(kMeasured);
        if (g.outWidth != 0)
        {
            const UINT wantW = std::max<UINT>(64u, static_cast<UINT>(g.outWidth * v + 0.5f));
            const UINT wantH = std::max<UINT>(64u, static_cast<UINT>(g.outHeight * v + 0.5f));
            ImGui::Text(T("Network raster: %ux%u", "Raster da rede: %ux%u"), g.netWidth, g.netHeight);
            if (wantW != g.netWidth || wantH != g.netHeight)
            {
                ImGui::SameLine();
                ImGui::TextColored(kWarn, T("(asked for %ux%u -- not applied yet)",
                                            "(pediu %ux%u -- ainda não aplicado)"), wantW, wantH);
            }
        }
        if (v > 1.0f)
            Note(kWarn, T("Supersampling the neural input increases GPU time and memory use.",
                          "Supersampling da entrada neural aumenta o tempo de GPU e o uso de memória."));

        int passes = g.passes.load();
        const bool passDanger = g.inlineMode.load() && passes > 1 && sc > 1.0f;
        if (Risk r(passDanger ? kDanger : kWarn, passes > 1);
            ImGui::SliderInt(T("Pass Count", "Número de Passes"), &passes, 1,
                             static_cast<int>(3), "%d", 0))
        {
            g.passes.store(passes);
            Log("menu: pass count %d", passes);
        }
        Help("Runs the network over its own output one to three times. The final correction is "
             "measured against the original input. Later passes default to zero Local Tone; "
             "use Per pass or Taper to reduce Structure.\n\n"
             "More passes can strengthen material changes, grain and halos. GPU cost grows roughly "
             "with the pass count. Two and three inline passes were exercised on a saved a reference capture frame; "
             "quality in motion still needs validation.",
             "Roda a rede sobre a própria saída de uma a três vezes. A correção final é medida "
             "contra a entrada original. Os passes seguintes usam Tom Local zero por padrão; "
             "use Por passe ou Diminuir passes seguintes para reduzir Estrutura.\n\n"
             "Mais passes podem intensificar mudanças de material, granulado e halos. O custo de GPU "
             "cresce aproximadamente com a contagem. Dois e três passes inline foram executados "
             "num quadro salvo do a reference capture; a qualidade em movimento ainda precisa de validação.");
        Tag(kMeasured);
        if (passes > 1)
            Note(kWarn, T("Each extra pass costs another inference. Compare detail and frame time.",
                          "Cada passe extra custa outra inferência. Compare detalhe e tempo de quadro."));

        if (passes > 1)
        {
            bool taper = g.passTaper.load();
            if (ImGui::Checkbox(T("Taper later passes", "Diminuir passes seguintes"), &taper))
            {
                g.passTaper.store(taper);
                Log("menu: pass taper %s", taper ? "on" : "off");
            }
            Help("Halves Structure on each later pass: 1.0, 0.5, 0.25. Local Tone already defaults "
                 "to zero after pass 1. Skin keeps its selected mode. Explicit per-pass overrides "
                 "take precedence. This can reduce accumulated grain and outlines; compare in your scene.",
                 "Reduz Estrutura pela metade a cada passe: 1.0, 0.5, 0.25. Tom Local já usa zero "
                 "depois do primeiro passe. Pele mantém o modo escolhido. Ajustes explícitos por passe "
                 "têm prioridade. Pode reduzir granulado e contornos acumulados; compare na sua cena.");
            Tag(kTraced);
        }

        // Per-pass profiles, the reference fork's "Per pass" tree. A later pass is looking at a
        // picture an earlier one already edited, so the same numbers again ask it to sharpen its
        // own sharpening -- and that is the half of "3 passes looks deep fried" that the
        // composition cannot reach from outside, because it happens inside the network.
        if (passes > 1 && ImGui::TreeNode(T("Per pass", "Por passe")))
        {
            ImGui::TextUnformatted(T("What each run of the network is told, where it should "
                                     "differ from the values below.",
                                     "O que cada rodada da rede recebe, onde deve diferir dos "
                                     "valores abaixo."));
            for (int i = 0; i < passes; ++i)
            {
                char label[32];
                snprintf(label, sizeof(label), T("Pass %d", "Passe %d"), i + 1);
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
                if (ImGui::SliderFloat(T("Local Tone", "Tom Local"), &pv, 0.0f, 3.0f, "%.2f", 0))
                    g.passTone[i].store(pv);
                pv = g.passSkin[i].load();
                if (ImGui::SliderFloat(T("Skin", "Pele"), &pv, 0.0f, 3.0f, "%.2f", 0))
                    g.passSkin[i].store(pv);
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::TreePop();
            }
            Note(kWarn, T("A pass with its own settings off follows the Engine tab, exactly as "
                          "before. The useful shape is a taper -- full on pass 1, less on 2, "
                          "less again on 3 -- because each pass is editing the last one's work.",
                          "Um passe com os ajustes próprios desligados segue a aba Motor, "
                          "exatamente como antes. O formato útil é uma queda -- cheio no passe "
                          "1, menos no 2, menos ainda no 3 -- porque cada passe está editando o "
                          "trabalho do anterior."));
            ImGui::TreePop();
        }

        bool bic = g.bicubic.load();
        if (ImGui::Checkbox(T("Bicubic Residual Upsample", "Upsample Bicúbico do Resíduo"), &bic))
        {
            g.bicubic.store(bic);
            Log("menu: residual upsample %s", bic ? "bicubic" : "bilinear");
        }
        Help("Below Resolution Scale 1.00 only the correction comes back up to full resolution. "
             "Stretching it bilinearly is a blur, which throws away everything but the "
             "low-frequency part -- colour and brightness -- and that alone was enough to make "
             "the whole effect look like a colour filter. Catmull-Rom keeps the rest.\n\n"
             "Turn it off if hard edges ring. No effect at all at Resolution Scale 1.00.",

             "Abaixo de Escala de Resolução 1.00, só a correção volta para a resolução cheia. "
             "Esticar ela bilinearmente é um borrão, que joga fora tudo menos a parte de baixa "
             "frequência -- cor e brilho -- e isso sozinho já bastava para o efeito inteiro "
             "parecer um filtro de cor. Catmull-Rom mantém o resto.\n\n"
             "Desligue se arestas duras ficarem com halo. Nenhum efeito em Escala 1.00.");
        Tag(kMeasured);
    }

    if (ImGui::CollapsingHeader(T("Guides", "Guias"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        // What is actually feeding the engine inputs in the last reported frame. The most useful line in the
        // overlay: the network's ceiling is set here, not by any slider above it.
        ImGui::Text(T("Colour %s   Depth %s   Motion %s   Exposure not fed",
                      "Cor %s   Profundidade %s   Movimento %s   Exposição não alimentada"),
                    T("from the swapchain", "da swapchain"),
                    g.gameDepthActive    ? T("FROM THE GAME", "DO JOGO")
                    : T("none", "nenhuma"),
                    g.gameMotionActive ? T("FROM THE GAME", "DO JOGO") : T("estimated", "estimado"));
        Help("Game depth and motion are used only when captured and accepted by the engine. Without game motion the original optical flow estimator is used. Status is sampled, not inferred from resource existence.",
             "Profundidade e movimento do jogo só são usados quando capturados e aceitos pelo motor. Sem movimento do jogo é usado o estimador de fluxo original. O estado é amostrado, não inferido pela existência de recursos.");

        // A buffer can be crossed, bound and fed and still be a cleared constant, which looks
        // identical from the outside and is worth nothing to the network.
        if (g.probeStillPct.load() >= 0)
        {
            const float lo = g.probeDepthMin.load(), hi = g.probeDepthMax.load();
            const bool depthReal = (hi - lo) > 1e-6f;
            const int still = g.probeStillPct.load();
            ImGui::Text(T("Measured  depth %.4f..%.4f   motion %d%% still, mean %.2f px, max %.2f px",
                          "Medido  profundidade %.4f..%.4f   movimento %d%% parado, média %.2f px, "
                          "máx %.2f px"),
                        static_cast<double>(lo), static_cast<double>(hi), still,
                        static_cast<double>(g.probeMotionMean.load()),
                        static_cast<double>(g.probeMotionMax.load()));
            Help("What the guides actually contain, not just whether they were handed over.\n\n"
                 "Read it while playing, not on a menu: a 2D screen genuinely has no depth and "
                 "nothing on it moves. In a real scene depth should have a spread and motion a "
                 "non-zero mean. If it stays flat while you drive, the guide picked the wrong "
                 "buffer and the network is still working from colour alone.",

                 "O que os guias realmente contêm, não só se foram entregues.\n\n"
                 "Leia jogando, não no menu: uma tela 2D genuinamente não tem profundidade e "
                 "nada nela se move. Numa cena de verdade a profundidade tem que ter espalhamento "
                 "e o movimento uma média diferente de zero. Se ficar chapado enquanto você "
                 "dirige, o guia pegou o buffer errado e a rede continua trabalhando só com cor.");
            if (!depthReal || still >= 100)
                Note(kWarn, T("A guide is carrying nothing. Expected on a menu -- check it while "
                              "driving.",
                              "Um guia não está carregando nada. Esperado no menu -- confira "
                              "dirigindo."));
        }
        else if (g.gameDepthActive || g.gameMotionActive)
            ImGui::TextDisabled("%s",T("Measuring the guides...", "Medindo os guias..."));

        if (!g.gameDepthActive && !g.gameMotionActive)
            Note(kWarn, T("The network is running on colour alone.",
                          "A rede está rodando só com cor."));

        bool guides = g.useGameGuides.load();
        if (ImGui::Checkbox(T("Read Guides From The Game", "Ler Guias do Jogo"), &guides))
        {
            g.useGameGuides.store(guides);
            Log("menu: game guides %s", guides ? "on" : "off");
        }
        Help("D3D11 only. Watches which depth-stencil and which two-channel float target the game "
             "binds most often, copies each once a frame and carries them over the bridge to the "
             "network's device.\n\n"
             "Turn it off to fall back to estimated motion and no depth.",

             "Só D3D11. Observa qual depth-stencil e qual render target float de dois canais o "
             "jogo mais liga, copia cada um uma vez por quadro e leva pela ponte até o device da "
             "rede.\n\n"
             "Desligue para voltar a movimento estimado e nenhuma profundidade.");
        Tag(kMeasured);

        bool depth = g.useDepth.load();
        if (ImGui::Checkbox(T("Depth", "Profundidade"), &depth))
        {
            g.useDepth.store(depth);
            Log("menu: depth %s", depth ? "on" : "off");
        }
        Help("Hands the depth buffer to the engine and sets the flag that says it is valid.\n\n"
             "Depth is what stops the network guessing at geometry. It mainly buys stability and "
             "disocclusion, not sharper texture -- do not expect this one to transform the image.",

             "Entrega o buffer de profundidade ao motor e liga a flag que diz que ele é válido.\n\n"
             "Profundidade é o que faz a rede parar de chutar geometria. Compra principalmente "
             "estabilidade e desoclusão, não textura mais afiada -- não espere que este "
             "transforme a imagem.");
        Tag(kTraced);

        // Depth Inverted used to be a checkbox here. It is gone: 8d9b0 is pinned to the
        // engine's own default of 1 in the record path. No run on either target ever
        // produced a reading that told the two settings apart, so the only thing the switch
        // could do was be set wrong.
        bool hist = g.useHistory.load();
        if (Risk r(kWarn, hist); ImGui::Checkbox(T("History", "Histórico"), &hist))
        {
            g.useHistory.store(hist);
            // Host applies the original History side effect on SET_STATE.
            Log("menu: history %s", hist ? "on" : "off");
        }
        Help("Hands the engine last frame's output to carry forward.\n\n"
             "Motion vectors say where a pixel was. Without history there is nothing for them to "
             "point at, so this is the switch that turns motion from a number the engine reports "
             "into one it can use.\n\n"
             "Experimental: it writes a pointer into the runtime at a fixed offset, and a wrong "
             "one there hangs the game rather than failing. If the picture smears or the game "
             "stops responding, this is the first thing to turn off.",

             "Entrega ao motor a saída do quadro anterior para carregar adiante.\n\n"
             "Vetores de movimento dizem onde um pixel estava. Sem histórico não há para onde "
             "eles apontarem, então esta é a chave que transforma movimento de um número que o "
             "motor reporta em um que ele consegue usar.\n\n"
             "Experimental: escreve um ponteiro no runtime num offset fixo, e um ponteiro errado "
             "ali congela o jogo em vez de falhar. Se a imagem borrar ou o jogo parar de "
             "responder, esta é a primeira coisa a desligar.");
        Tag(kTraced);
        if (hist)
            Note(kWarn, T("Experimental -- first thing to turn off if the game hangs or smears.",
                          "Experimental -- primeira coisa a desligar se o jogo congelar ou "
                          "borrar."));

        bool mv = g.useMotion.load();
        if (ImGui::Checkbox(T("Motion Vectors", "Vetores de Movimento"), &mv))
        {
            g.useMotion.store(mv);
            Log("menu: motion %s", mv ? "on" : "off");
        }
        if (g.gameMotionActive)
            Help("Read from the game's own velocity buffer -- the real thing, per pixel.",
                 "Lidos do próprio buffer de velocidade do jogo -- a coisa real, por pixel.");
        else
            Help("Estimated by comparing consecutive frames, because this target has no velocity "
                 "buffer to read. It is wrong wherever pixels move without the geometry moving: "
                 "reflections, fire, moving shadows, and anything appearing from behind something "
                 "else. The NVIDIA route does the same thing here.",

                 "Estimados comparando quadros consecutivos, porque este alvo não tem buffer de "
                 "velocidade para ler. É errado onde pixels se movem sem a geometria se mover: "
                 "reflexos, fogo, sombras em movimento, e qualquer coisa que aparece de trás de "
                 "outra. A rota da NVIDIA faz o mesmo aqui.");
        Tag(kMeasured);

        if (mv)
        {
            float v = g.motionScale.load();
            if (ImGui::SliderFloat(T("Motion Scale", "Escala do Movimento"), &v, -2.0f, 2.0f,
                                   "%.2f", 0))
                g.motionScale.store(v);
            Help("How much motion the network is told there is. It multiplies the field on its "
                 "way into the engine, whichever field that is -- the game's own velocity buffer "
                 "or the estimated one. 0.00 says nothing moved, 1.00 is as measured, -1.00 "
                 "flips the direction, 0.50 suits a buffer stored in NDC.\n\n"
                 "It used to apply to the game's vectors only, and it was hidden on every target "
                 "without them -- so the estimated field, the one that is a guess and the one "
                 "most in need of turning down, had no control at all. Both go through it now.\n\n"
                 "Set it by eye: put Debug View on 'Motion vectors' and pan the camera. The field "
                 "should follow the camera steadily. Shimmer means it is too high -- turn it down "
                 "rather than turning motion off, which is the blunt version of the same thing.",

                 "Quanto movimento a rede é informada que existe. Multiplica o campo no caminho "
                 "para o motor, seja qual for o campo -- o buffer de velocidade do próprio jogo "
                 "ou o estimado. 0.00 diz que nada se moveu, 1.00 é como foi medido, -1.00 "
                 "inverte a direção, 0.50 serve para um buffer guardado em NDC.\n\n"
                 "Antes valia só para os vetores do jogo, e ficava escondido em todo alvo sem "
                 "eles -- então o campo estimado, que é um chute e é o que mais precisa ser "
                 "baixado, não tinha controle nenhum. Agora os dois passam por ele.\n\n"
                 "Ajuste no olho: ponha a Visão de Debug em 'Vetores de movimento' e gire a "
                 "câmera. O campo tem que acompanhar a câmera, firme. Cintilar quer dizer alto "
                 "demais -- baixe, em vez de desligar o movimento, que é a versão bruta da mesma "
                 "coisa.");
            Tag(kTraced);
        }
        if (mv && !g.gameMotionActive)
        {
            float v = g.flowGate.load();
            if (ImGui::SliderFloat(T("Flow Contrast Gate", "Portão de Contraste do Fluxo"), &v,
                                   0.002f, 0.10f, "%.3f", 0))
                g.flowGate.store(v);
            Help("The first of two filters that decide which pixels are allowed to move.\n\n"
                 "Before searching anything, the estimator measures the brightest and darkest "
                 "luma in the 3x3 block around the pixel. If the difference is below this number "
                 "the pixel is declared still and no search happens at all.\n\n"
                 "Why: a flat block -- clear sky, a painted wall -- matches equally well at every "
                 "offset, so the winner is simply whichever offset the loop tried first. That is "
                 "the aperture problem, and it is why widening the search made the field wilder "
                 "instead of better. There is nothing to track, so do not pretend to track it.\n\n"
                 "RAISE it and more of the screen is frozen: cleaner, but the network is told "
                 "nothing moved. 0.020 froze 99% of a dark a reference capture scene. LOWER it and "
                 "flatter blocks get tracked: more coverage, noisier vectors.\n\n"
                 "The number is a luma difference on a 0..1 scale, so 0.020 means 2% contrast.\n\n"
                 "Only used when motion is estimated. With a game that hands over real velocity "
                 "vectors this does nothing, which is why it is hidden then.",

                 "O primeiro de dois filtros que decidem quais pixels têm permissão de se mover.\n\n"
                 "Antes de procurar qualquer coisa, o estimador mede a luma mais clara e a mais "
                 "escura no bloco 3x3 em volta do pixel. Se a diferença for menor que este "
                 "número, o pixel é declarado parado e nenhuma busca acontece.\n\n"
                 "Por quê: um bloco chapado -- céu limpo, uma parede pintada -- casa igualmente "
                 "bem em todo deslocamento, então o vencedor é simplesmente o primeiro que o laço "
                 "testou. Isso é o problema da abertura, e é por isso que alargar a busca deixou "
                 "o campo mais doido em vez de melhor. Não há o que rastrear, então não finja que "
                 "há.\n\n"
                 "AUMENTE e mais da tela fica congelada: mais limpo, mas a rede é informada de "
                 "que nada se moveu. 0,020 congelou 99% de uma cena escura do a reference capture. "
                 "DIMINUA e blocos mais chapados passam a ser rastreados: mais cobertura, vetores "
                 "mais ruidosos.\n\n"
                 "O número é uma diferença de luma na escala 0..1, então 0,020 quer dizer 2% de "
                 "contraste.\n\n"
                 "Só é usado quando o movimento é estimado. Num jogo que entrega vetores de "
                 "velocidade de verdade isto não faz nada, por isso fica escondido.");
            Tag(kMeasured);
            v = g.flowRatio.load();
            if (ImGui::SliderFloat(T("Flow Accept Ratio", "Razão de Aceite do Fluxo"), &v, 0.50f,
                                   1.00f, "%.2f", 0))
                g.flowRatio.store(v);
            Help("The second filter, applied after the search instead of before it.\n\n"
                 "The estimator compares the 3x3 block against 81 candidate offsets in the "
                 "previous frame and keeps the one with the lowest error. It also records the "
                 "error of not moving at all. The winner is only believed if its error is below "
                 "the standing-still error times this number.\n\n"
                 "So the slider runs backwards from what you would guess: 1.00 is the most "
                 "PERMISSIVE -- any winner at least as good as standing still is accepted -- and "
                 "0.50 is the strictest, demanding a match that explains the block with half the "
                 "error of not moving. Lower means more of the screen is forced still.\n\n"
                 "Gate rejects a block before searching, on the grounds that there is nothing in "
                 "it. Ratio rejects a result after searching, on the grounds that the answer is "
                 "not convincing. Between them they are what stop the field being noise.\n\n"
                 "The flow probe at frame 300 prints what a pair actually did: look for the "
                 "percentage of blocks reported still.\n\n"
                 "Only used when motion is estimated.",

                 "O segundo filtro, aplicado depois da busca em vez de antes.\n\n"
                 "O estimador compara o bloco 3x3 contra 81 deslocamentos candidatos no quadro "
                 "anterior e fica com o de menor erro. Ele também guarda o erro de não se mover. "
                 "O vencedor só é aceito se o erro dele for menor que o erro de ficar parado "
                 "vezes este número.\n\n"
                 "Ou seja, o slider anda ao contrário do que se imagina: 1.00 é o mais "
                 "PERMISSIVO -- qualquer vencedor tão bom quanto ficar parado é aceito -- e 0.50 "
                 "é o mais rígido, exigindo um casamento que explique o bloco com metade do erro "
                 "de não se mover. Mais baixo significa mais tela forçada a parada.\n\n"
                 "O Portão rejeita um bloco antes de buscar, com o argumento de que não há nada "
                 "nele. A Razão rejeita um resultado depois de buscar, com o argumento de que a "
                 "resposta não convence. Juntos, são o que impede o campo de virar ruído.\n\n"
                 "A sonda de fluxo no quadro 300 imprime o que um par realmente fez: procure a "
                 "porcentagem de blocos reportados parados.\n\n"
                 "Só é usado quando o movimento é estimado.");
            Tag(kMeasured);
        }
    }

    if (ImGui::CollapsingHeader(T("Debug", "Depuração")))
    {
        int dbg = g.debugView.load();
        if (ImGui::Combo(T("Debug View", "Visão de Debug"), &dbg,
                         T("Off\0Network input\0Network output\0Residual x8\0Motion vectors\0Depth x500\0",
                           "Desligado\0Entrada da rede\0Saída da rede\0Resíduo x8\0Vetores de movimento\0Profundidade x500\0")))
        {
            g.debugView.store(dbg);
            Log("menu: debug view %d", dbg);
        }
        Help("Replaces the screen with one stage of the pipeline.\n\n"
             "Network input: what the network was handed, stretched back up. Black here means "
             "nothing downstream can work.\n\n"
             "Network output: what it gave back. Identical to the input means the network is "
             "returning what it was given.\n\n"
             "Residual x8: the correction alone against mid grey. This is the one that answers "
             "'is it doing anything'. Flat grey means it changed nothing; structure that follows "
             "edges and texture means it is working -- and how much of that survives is exactly "
             "what Resolution Scale decides.\n\n"
             "Motion vectors: red is horizontal, green vertical, mid grey is still. Flat grey "
             "while the camera moves means the flow is gated off -- lower Flow Contrast Gate.\n\n"
             "Depth x500: multiplied because PS2 depth peaks around 0.002 and is otherwise solid "
             "black. Overall Intensity scales this view.",

             "Substitui a tela por um estágio do pipeline.\n\n"
             "Entrada da rede: o que foi entregue à rede, esticado de volta. Preto aqui significa "
             "que nada depois disso pode funcionar.\n\n"
             "Saída da rede: o que ela devolveu. Idêntica à entrada significa que a rede está "
             "devolvendo o que recebeu.\n\n"
             "Resíduo x8: a correção sozinha contra cinza médio. Esta é a que responde 'está "
             "fazendo alguma coisa'. Cinza chapado significa que a rede não mudou nada; estrutura "
             "que segue arestas e textura significa que está funcionando -- e quanto disso "
             "sobrevive é exatamente o que a Escala de Resolução decide.\n\n"
             "Vetores de movimento: vermelho é horizontal, verde é vertical, cinza médio é "
             "parado. Cinza chapado com a câmera se movendo significa fluxo bloqueado -- baixe o "
             "Portão de Contraste do Fluxo.\n\n"
             "Profundidade x500: multiplicada porque a profundidade do PS2 chega a uns 0,002 e é "
             "preto puro fora isso. A Intensidade Geral escala esta visão.");
        Tag(kMeasured);

        if (ImGui::Button(T("Measure Residual Again", "Medir Resíduo de Novo")))
        {
            controls.measure=true;
            Log("menu: residual measurement re-armed");
        }
        Help("Writes a 'measure, residual' line to amd-nr.log: the size of the correction, "
             "and how much of it follows the image's own detail rather than being a flat shift.\n\n"
             "Change one control, press this, compare the two numbers. It is the only way to tell "
             "a control that does something from one that does not.",

             "Escreve uma linha 'measure, residual' no amd-nr.log: o tamanho da correção, e "
             "quanto dela segue o detalhe da própria imagem em vez de ser um deslocamento "
             "chapado.\n\n"
             "Mude um controle, aperte isto, compare os dois números. É o único jeito de separar "
             "um controle que faz algo de um que não faz.");
    }

    if (ImGui::CollapsingHeader(T("Engine", "Motor")))
    {
        ImGui::TextDisabled("%s",T("The engine's own option struct. Every offset below came from "
                              "decompiling the runtime's ini reader, not from guesswork -- but "
                              "an offset being real says nothing about what writing it does. "
                              "The tag after each control says how far that is actually known.",
                              "A struct de opções do próprio motor. Todo offset abaixo veio de "
                              "decompilar o leitor de ini do runtime, não de chute -- mas um "
                              "offset ser real não diz nada sobre o que escrever nele faz. A "
                              "etiqueta depois de cada controle diz até onde isso é sabido."));
        ImGui::TextDisabled("%s",T("The values here come from the host settings, so an untouched tab "
                              "changes nothing.",
                              "Os valores vêm dos ajustes do host, então esta aba intocada "
                              "não muda nada."));
        ImGui::Separator();

        bool mask = g.autoMask.load() != 0;
        if (Risk r(kDanger, !mask);
            ImGui::Checkbox(T("Character Mask", "Máscara de Personagem"), &mask))
        {
            g.autoMask.store(mask ? 1 : 0);
            Log("menu: automask %d", mask ? 1 : 0);
        }
        Help("UseAutoMask, at 8d9e0. The engine's semantic character mask: it is what makes Skin "
             "Structure Strength apply to characters rather than to the whole frame. This is the "
             "same control RenoDX exposes as Character Mask.\n\n"
             "It defaults to 1 and this add-on never wrote it, so it has always been on by "
             "default. Turning it off is a real test: if Skin stops doing even its measured 1.5%, "
             "the mask is what was carrying it.",

             "UseAutoMask, em 8d9e0. A máscara semântica de personagem do motor: é o que faz a "
             "Força de Estrutura na Pele se aplicar a personagens em vez do quadro inteiro. É o "
             "mesmo controle que o RenoDX expõe como Character Mask.\n\n"
             "O padrão é 1 e este add-on nunca escrevia esse campo, então sempre esteve ligado "
             "por omissão. Desligar é um teste de verdade: se a Pele parar de fazer até os 1,5% "
             "medidos, era a máscara que carregava aquilo.");
        Tag(kMeasured);
        if (!mask)
            Note(kDanger, T("Off removes the effect from the whole frame, not just from "
                            "characters -- seen in game. The engine derives its structure and "
                            "tone parameters through this mask, so with it off there is nothing "
                            "left to derive them from. Turn it back on unless you are measuring.",
                            "Desligado tira o efeito do quadro inteiro, não só dos personagens "
                            "-- visto no jogo. O motor deriva os parâmetros de estrutura e tom "
                            "através desta máscara, então com ela desligada não sobra de onde "
                            "derivar. Religue, a não ser que esteja medindo."));

        int tmode = g.temporalMode.load();
        if (ImGui::Combo(T("Temporal", "Temporal"), &tmode,
                         T("Auto (follow the guides)\0Off\0On\0",
                           "Automático (segue os guias)\0Desligado\0Ligado\0")))
        {
            g.temporalMode.store(tmode);
            Log("menu: temporal %d", tmode);
        }
        Help("Temporal accumulation, at 8d9bd. This add-on had the byte labelled 'motion is "
             "valid' -- a guess that turned out wrong. The engine's ini reader reads the key "
             "Temporal into it.\n\n"
             "That explains a measurement nobody could account for: Temporal=1 was the only run "
             "where the engine reported non-zero motion. Not a coincidence -- accumulating over "
             "time is what gives a motion vector something to point at.\n\n"
             "Auto turns it on whenever a motion field exists, which is the old behaviour. Off "
             "and On are explicit, for A/B.",

             "Acumulação temporal, em 8d9bd. Este add-on rotulava esse byte como 'movimento "
             "válido' -- um chute que estava errado. O leitor de ini do motor lê a chave "
             "Temporal para ele.\n\n"
             "Isso explica uma medição que ninguém conseguia justificar: Temporal=1 foi a única "
             "execução em que o motor reportou movimento diferente de zero. Não é coincidência "
             "-- acumular ao longo do tempo é o que dá a um vetor de movimento algo para onde "
             "apontar.\n\n"
             "Automático liga sempre que existe campo de movimento, que é o comportamento "
             "antigo. Desligado e Ligado são explícitos, para A/B.");
        Tag(kTraced);

        int tone = g.tonemap.load();
        if (ImGui::SliderInt(T("Tonemap", "Tonemap"), &tone, -1, 3, "%d", 0))
            g.tonemap.store(tone);
        Help("-1: automatic for the selected encoding. With sRGB, sends 0 (off); other "
             "encodings keep the runtime's automatic detection. 0..3: explicit runtime "
             "override. 1 reproduces the old automatic behaviour on an SDR FP16 input.\n\n"
             "Restart the game after changing this setting.",

             "-1: automático pela codificação escolhida. Em sRGB, envia 0 (desligado); "
             "as outras codificações mantêm a detecção do runtime. 0..3: valor explícito "
             "para o runtime. 1 reproduz o automático antigo na entrada SDR em FP16.\n\n"
             "Reinicie o jogo após alterar esta opção.");
        Tag(kMeasured);

        int ch = g.toneChannels.load();
        if (ImGui::SliderInt(T("Tone Channels", "Canais de Tom"), &ch, 0, 3, "%d", 0))
            g.toneChannels.store(ch);
        Help("ToneChannels, at 8d9e4. An ini key of the engine that nothing in this project knew "
             "existed until the reader was decompiled. Default 0. What it does is unknown -- it "
             "is here to be A/B'd against the residual, like everything else with an unknown "
             "effect.",

             "ToneChannels, em 8d9e4. Uma chave de ini do motor que ninguém neste projeto sabia "
             "que existia até o leitor ser decompilado. Padrão 0. O que ela faz é desconhecido "
             "-- está aqui para ser testada em A/B contra o resíduo, como tudo que tem efeito "
             "desconhecido.");
        Tag(kUnknown);

        float es = g.engineScale.load();
        if (ImGui::SliderFloat(T("Engine Scale", "Escala do Motor"), &es, 0.0f, 1.0f, "%.5f", 0))
            g.engineScale.store(es);
        Help("Scale, at 8d9dc, default 0.03125. This is the 0.031 that showed up in the startup "
             "float dump and was written down as 'a live value we never wrote' -- it is an ini "
             "key of the engine named Scale, and 0.03125 is exactly 1/32.\n\n"
             "Unrelated to Resolution Scale under Performance, which is ours. Nothing is known "
             "about what this one scales. Move it in small steps and watch the residual.",

             "Scale, em 8d9dc, padrão 0.03125. É o 0,031 que apareceu no despejo de floats da "
             "inicialização e foi anotado como 'valor vivo que nunca escrevemos' -- é uma chave "
             "de ini do motor chamada Scale, e 0,03125 é exatamente 1/32.\n\n"
             "Nada a ver com a Escala de Resolução em Desempenho, que é nossa. Nada se sabe "
             "sobre o que esta escala. Mexa em passos pequenos e olhe o resíduo.");
        Tag(kUnknown);
        ImGui::SameLine();
        // A 0..1 slider at five decimals cannot be dragged back onto exactly 1/32, and this is a
        // field nobody knows the effect of -- so leaving it a hair off its default is a way to
        // change the picture and never find out why.
        if (ImGui::SmallButton(T("Reset to 1/32", "Voltar para 1/32")))
        {
            g.engineScale.store(0.03125f);
            Log("menu: engine scale back to the default 0.03125");
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s",T("Model A / B / C: not portable to this runtime. Closed.",
                              "Model A / B / C: não é portável para este runtime. Encerrado."));
        Help("Model A/B/C is real and it does change the picture -- on NVIDIA. It is DLSSNR.Style, "
             "an int at options offset 236 in nvngx_dlssnr.dll. Style 0 is Model A and is the "
             "literal baseline: it has no table entry and overwrites nothing. Style 1 and 2 pick "
             "a row out of an 8x68-byte table and lerp fourteen floats into options+292..+344, "
             "scaled by LocalToneStrength. Three of those fourteen are non-zero across both rows. "
             "It is one weight set, not three networks.~~"
             "It cannot be ported here, and this was chased to the end rather than assumed. The "
             "three constants -0.10, -0.25 and -0.15 do not exist in the AMD binary. The AMD "
             "option block is mapped field by field and no slot is Style. Nothing in that binary "
             "writes a span of fourteen floats. And the HIP kernels on the appearance path are "
             "too small to take them: the style vector is 56 bytes, while k_final_head and "
             "k_post_block each receive a 32-byte struct by value.~~"
             "Whoever did the port compiled the network with the neutral style baked in. The "
             "kernels are precompiled GCN code objects inside the DLL with no source, so adding "
             "the fields means recompiling them. Model A is the only one that exists on this "
             "side. Not reopening it.",

             "Model A/B/C é real e muda a imagem sim -- na NVIDIA. É o DLSSNR.Style, um int no "
             "offset 236 da struct de opções do nvngx_dlssnr.dll. Style 0 é o Model A e é o "
             "baseline literal: não tem entrada na tabela e não sobrescreve nada. Style 1 e 2 "
             "pegam uma linha de uma tabela de 8x68 bytes e interpolam catorze floats para "
             "options+292..+344, escalados pelo LocalToneStrength. Três desses catorze são "
             "diferentes de zero nas duas linhas. É um conjunto de pesos só, não três redes.~~"
             "Não dá para portar para cá, e isso foi perseguido até o fim, não assumido. As três "
             "constantes -0,10, -0,25 e -0,15 não existem no binário AMD. O bloco de opções do "
             "AMD está mapeado campo a campo e nenhum slot é Style. Nada naquele binário escreve "
             "uma sequência de catorze floats. E os kernels HIP do caminho de aparência não têm "
             "espaço: o vetor de style são 56 bytes, e o k_final_head e o k_post_block recebem um "
             "struct de 32 bytes por valor.~~"
             "Quem portou compilou a rede com o style neutro embutido. Os kernels são code "
             "objects GCN pré-compilados dentro da DLL, sem fonte, então acrescentar os campos "
             "significa recompilar os kernels. Model A é o único que existe deste lado. Não "
             "vamos reabrir.");
    }

    if (ImGui::CollapsingHeader(T("Advanced", "Avançado")))
    {
        float v = g.tone.load();
        if (ImGui::SliderFloat(T("Local Tone Strength", "Força de Tom Local"), &v, 0.0f, 3.0f,
                               "%.2f", 0))
            g.tone.store(v);
        Help("Measured inert. A full sweep produced numbers byte-for-byte identical to the "
             "baseline, which matches the RenoDX note that Global Tone is not visible on the "
             "recovered NGX path. It is here to be re-checked on a different target, not to be "
             "used. Left at 1.",

             "Medido inerte. Uma varredura completa produziu números byte a byte idênticos ao "
             "baseline, o que bate com a nota do RenoDX de que Global Tone não é visível no "
             "caminho NGX recuperado. Está aqui para ser reconferido num alvo diferente, não "
             "para ser usado. Deixado em 1.");
        Tag(kInert);

        ImGui::Text("%s",T("Engine offsets written: 8d9d0 tone, 8d9d4 structure, 8d9d8 skin",
                      "Offsets escritos no motor: 8d9d0 tom, 8d9d4 estrutura, 8d9d8 pele"));
        Help("These three are hardcoded RVAs into one specific build of the runtime, found by "
             "matching strings in the binary. That the binary contains the words does not prove "
             "it reads these fields -- an offset written but never read looks identical from out "
             "here. The log dumps the float window around them at startup.",

             "Estes três são RVAs hardcoded numa build específica do runtime, achados casando "
             "strings no binário. Que o binário contenha as palavras não prova que ele lê estes "
             "campos -- um offset escrito e nunca lido é idêntico visto daqui. O log despeja a "
             "janela de floats em volta deles na inicialização.");

        ImGui::Separator();
        ImGui::Text(T("Restart-only: Stage=%d  Events=%d  NoBridge=%d  NoBackBuffer=%d",
                      "Só na reinicialização: Stage=%d  Events=%d  NoBridge=%d  NoBackBuffer=%d"),
                    g.stage.load(), g.events, g.noBridge.load() ? 1 : 0,
                    g.noBackBuffer.load() ? 1 : 0);
        Help("Diagnostics that decide what gets built at startup, so they cannot be changed live "
             "-- set them in amd-nr.ini.\n\n"
             "Stage=1 stops before D3D12 loads, 2 before the engine, 3 is everything. Events is a "
             "bitmask: 1 bind, 2 draw, 4 clear, 8 destroy_swapchain, 16 overlay. These are what "
             "found the DXGI resize failure.",

             "Diagnósticos que decidem o que é construído na inicialização, então não podem ser "
             "mudados ao vivo -- ajuste no amd-nr.ini.\n\n"
             "Stage=1 para antes da D3D12 carregar, 2 antes do motor, 3 é tudo. Events é uma "
             "máscara de bits: 1 bind, 2 draw, 4 clear, 8 destroy_swapchain, 16 overlay. Foram "
             "eles que acharam a falha de resize da DXGI.");
    }

    // Experimental. Things that work, were measured on one route, and are not the shipped
    // arrangement. New entries go above Network Output, which stays at the bottom.
    if (ImGui::CollapsingHeader(T("Experimental", "Experimental")))
    {
        ImGui::TextDisabled("%s",T("Proof of concept. Measured on one route each, and not the shipped "
                              "arrangement.",
                              "Prova de conceito. Medidos numa rota cada, e não são o arranjo "
                              "padrão."));
        ImGui::Separator();

        bool raw = g.networkOutput.load();
        if (ImGui::Checkbox(T("Network Output (bypass composition) -- proof of concept",
                              "Saída da Rede (ignora a composição) -- prova de conceito"), &raw))
        {
            g.networkOutput.store(raw);
            Log("menu: network output mode %d", raw ? 1 : 0);
        }
        Help("Show the current network answer without composition. Highlight Guard, Colour Strength and residual limits are bypassed. A skipped evaluation always presents the original frame on x86.",
             "Mostra a resposta atual da rede sem composição. Trava de Realce, Força da Cor e limites do resíduo são ignorados. Uma avaliação pulada sempre apresenta o quadro original no x86.");

    }

    ImGui::EndDisabled();
    if (ImGui::CollapsingHeader(T("Status", "Estado"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        StatusLine();
        ImGui::Text("Protocol v2 | Same-frame x86 | %s", controls.status.transportOnly ? "TRANSPORT_ONLY" : "Neural Rendering");
        ImGui::Text(T("Back buffer %ux%u -> network %ux%u", "Back buffer %ux%u -> rede %ux%u"),g.outWidth,g.outHeight,g.netWidth,g.netHeight);
        ImGui::Text("Depth active=%u | Motion active=%u | Passes loaded/active=%u/%u",controls.status.depthActive,controls.status.motionActive,controls.status.loadedPasses,controls.status.activePasses);
        CandidateStatus();
        ImGui::TextDisabled("Logs: amd-nr-x86.log / amd-nr-x86-host.log");
    }

    ImGui::Separator();
    ImGui::TextColored(kDanger,"%s", T("Red", "Vermelho"));
    ImGui::SameLine();
    ImGui::TextWrapped("%s",T("- this setting, at the value it is holding right now, can take the "
                         "display driver down with it. The game dies on DXGI_ERROR_DEVICE_REMOVED "
                         "and the desktop goes with it, with nothing in any log pointing back "
                         "here. Change it before you go further.",

                         "- este ajuste, no valor em que está agora, pode levar o driver de vídeo "
                         "junto. O jogo morre com DXGI_ERROR_DEVICE_REMOVED e o desktop vai "
                         "junto, sem nada em log nenhum apontando de volta para cá. Mude antes de "
                         "seguir."));
    ImGui::TextColored(kWarn,"%s", T("Amber", "Âmbar"));
    ImGui::SameLine();
    ImGui::TextWrapped("%s",T("- past what has actually been measured on this machine. Not known to "
                         "break, not known to work either. Change one thing at a time, save "
                         "first, and watch the skip rate under Status: if it climbs, back off.",

                         "- passou do que realmente foi medido nesta máquina. Não se sabe que "
                         "quebra, nem que funciona. Mude uma coisa por vez, salve antes, e olhe a "
                         "taxa de pulo em Estado: se subir, recue."));
    ImGui::TextDisabled("%s",T("A control is coloured only for the value it currently holds, so "
                          "turning it back down clears it. Hover any (?) for what a control does "
                          "and what was measured about it.",

                          "Um controle só fica colorido pelo valor que está segurando, então "
                          "baixar o valor limpa a cor. Passe o mouse em qualquer (?) para o que "
                          "um controle faz e o que foi medido sobre ele."));
    // The other legend. "Which of these actually does something in the game" was a question you
    // could only answer by reading the source, so the answer is printed next to each control and
    // spelled out once here.
    ImGui::TextDisabled("%s",T("Tags:", "Etiquetas:"));
    ImGui::SameLine();
    Tag(kMeasured);
    ImGui::SameLine();
    ImGui::TextDisabled("%s",T("a residual reading moved when it changed.", "uma leitura de resíduo "
                          "mudou quando ele mudou."));
    ImGui::TextDisabled(" ");
    ImGui::SameLine();
    Tag(kTraced);
    ImGui::SameLine();
    ImGui::TextDisabled("%s",T("reaches a consumer, never separated from its default here.",
                          "chega num consumidor, mas nunca foi separado do padrão dele aqui."));
    ImGui::TextDisabled(" ");
    ImGui::SameLine();
    Tag(kUnknown);
    ImGui::SameLine();
    ImGui::TextDisabled("%s",T("a real engine field whose effect nobody here has established.",
                          "um campo real do motor cujo efeito ninguém aqui estabeleceu."));
    ImGui::TextDisabled(" ");
    ImGui::SameLine();
    Tag(kInert);
    ImGui::SameLine();
    ImGui::TextDisabled("%s",T("swept and measured to change nothing.",
                          "varrido e medido como não mudando nada."));

    ImGui::Separator();
    if (ImGui::Button(T("Save Settings", "Salvar Ajustes")))
        controls.save=true;
    Help("Writes everything above to amd-nr.ini next to the exe, so it survives a restart.\n\n"
         "You do not have to press it: every control saves itself the moment you let go of it. This "
         "is here to write now, and to put a line in the log saying it happened.",

         "Escreve tudo acima no amd-nr.ini ao lado do exe, para sobreviver a um restart.\n\n"
         "Você não precisa apertar: cada controle se salva sozinho assim que você solta. Isto serve "
         "para escrever agora, e para deixar uma linha no log dizendo que aconteceu.");
    ImGui::SameLine();
    if (ImGui::Button(T("Reload Settings", "Recarregar Ajustes")))
    {
        controls.reload=true;
        Log("menu: settings reloaded from the ini");
    }
    Help("Re-reads amd-nr.ini, discarding anything changed here since the last save.",
         "Relê o amd-nr.ini, descartando qualquer coisa mudada aqui desde o último salvamento.");
    ImGui::SameLine();
    if(ImGui::Button(T("Factory Defaults", "Padrões de Fábrica")))controls.factory=true;
    Help("Restore factory tuning. Enabled, startup, hotkey and language are preserved. It lands in the ini with everything else, on its own.",
         "Restaura ajustes de fábrica. Ligado, inicialização, atalho e idioma são preservados. Vai para o ini junto com o resto, sozinho.");
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
