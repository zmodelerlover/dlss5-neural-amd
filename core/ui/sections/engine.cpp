#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../view_logic.h"
#include "../widgets/widgets.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

// The runtime's own default for its ini key 'Scale'.
constexpr float kRuntimeOutputScale = 0.03125f;

void Mask(PanelSettings &s)
{
    bool mask = s.autoMask != 0;
    if (Risk r(kDanger, !mask); ImGui::Checkbox(T("Character mask", "Máscara de personagem"), &mask))
        s.autoMask = mask ? 1 : 0;
    Help("UseAutoMask. The engine's semantic character mask, and what makes skin structure "
         "apply to characters rather than to the whole frame. Defaults to 1; this add-on never "
         "used to write it, so it has always been on.",
         "UseAutoMask. A máscara semântica de personagem do motor, e o que faz a estrutura da "
         "pele valer para personagens em vez do quadro inteiro. Padrão 1; este add-on nunca "
         "escrevia esse campo, então sempre esteve ligado.");
    if (!mask)
        Note(kDanger, T("Off removes the effect from the whole frame, not just from characters -- "
                        "seen in game. The engine derives its structure and tone parameters "
                        "through this mask, so with it off there is nothing left to derive them "
                        "from.",
                        "Desligada tira o efeito do quadro inteiro, não só dos personagens -- "
                        "visto no jogo. O motor deriva os parâmetros de estrutura e tom através "
                        "dela, então desligada não sobra de onde derivar."));
}

void Temporal(PanelSettings &s)
{
    int tmode = s.temporalMode;
    if (ImGui::Combo(T("Temporal", "Temporal"), &tmode,
                     T("Auto\0Off\0On\0", "Automático\0Desligado\0Ligado\0")))
        s.temporalMode = tmode;
    Help("Temporal accumulation. This add-on had the byte labelled 'motion is valid' -- a guess "
         "that turned out wrong; the engine's ini reader reads the key Temporal into it. That "
         "explains a measurement nobody could account for: Temporal=1 was the only run where the "
         "engine reported non-zero motion, which is what accumulating over time is for. Auto "
         "turns it on whenever a motion field exists.",

         "Acumulação temporal. Este add-on rotulava esse byte como 'movimento válido' -- um "
         "chute errado; o leitor de ini do motor lê a chave Temporal nele. Isso explica uma "
         "medição que ninguém justificava: Temporal=1 foi a única execução em que o motor "
         "reportou movimento diferente de zero, que é para isso que acumular no tempo serve. "
         "Automático liga sempre que existe campo de movimento.");
}

void Tonemap(PanelSettings &s)
{
    int tone = s.tonemap;
    if (ImGui::SliderInt(T("Tonemap", "Tonemap"), &tone, -1, 3, "%d", 0))
        s.tonemap = tone;
    Help("-1 is automatic for the selected Encoding: with sRGB it sends 0 (off), other "
         "encodings keep the runtime's own detection. 0..3 override it explicitly.\n\n"
         "Restart the game after changing this.",
         "-1 é automático pela Codificação escolhida: em sRGB manda 0 (desligado), as outras "
         "mantêm a detecção do próprio runtime. 0..3 forçam um valor.\n\n"
         "Reinicie o jogo depois de mudar.");
}

// The slider is 0..1 and not 0..3 because of what the write actually does: the record path writes
// `(value & ~2) | 4`, so four slider positions collapse to two -- 2 is byte-for-byte identical to
// 0, and 3 to 1. A control where half the range is a duplicate of the other half is a control that
// teaches you the wrong thing about the field. Bits 2 and 4 were read as the apply shader's
// timeout policy in v0.2.17; on v0.3.0 and v0.4.0 the runtime builds that word from its own state.
//
// Bit 4 has to stay set for another reason, read in the worker: when the whole word is 0 the
// runtime zeroes LocalStructure before it reaches the network (v0.3.0 zeroed LocalTone with it).
// The worker only ever tests the word against zero, so in the disassembly bit 0 reaches nothing
// on its own. Not measured.
void ToneChannels(PanelSettings &s)
{
    int ch = s.toneChannels & 1;
    if (ImGui::SliderInt(T("Tone channels", "Canais de tom"), &ch, 0, 1, "%d", 0))
        s.toneChannels = ch & 1;
    Help("ToneChannels bit 0. An ini key of the engine that nothing here knew existed until the "
         "reader was decompiled. On v0.3.0 and v0.4.0 the engine only tests the whole field "
         "against zero, so in the disassembly this bit reaches nothing on its own; not "
         "measured. Default 0, kept here to be A/B'd against the residual.\n\n"
         "The record path always sets bit 4 and clears bit 2, so the field is never 0: at 0 the "
         "runtime zeroes local structure before it reaches the network, whatever the sliders "
         "above say.",

         "ToneChannels bit 0. Uma chave de ini do motor que ninguém aqui sabia que existia até "
         "o leitor ser decompilado. No v0.3.0 e no v0.4.0 o motor só testa o campo inteiro "
         "contra zero, então na desmontagem este bit sozinho não chega a lugar nenhum; não "
         "medido. Padrão 0, mantido aqui para ser testado em A/B contra o resíduo.\n\n"
         "O caminho de gravação sempre liga o bit 4 e desliga o bit 2, então o campo nunca é 0: "
         "em 0 o runtime zera a estrutura local antes dela chegar na rede, digam o que disserem "
         "os sliders.");
}

void OutputScale(PanelSettings &s)
{
    float es = s.engineScale;
    if (Risk r(kDanger, std::fabs(es - kRuntimeOutputScale) > 1e-6f);
        ImGui::SliderFloat(T("Output scale", "Escala de saída"), &es, 0.0001f, 1.0f, "%.5f", 0))
        s.engineScale = std::max(es, 0.0001f);
    ImGui::SameLine();
    // A 0..1 slider at five decimals cannot be dragged back onto exactly 1/32.
    if (ImGui::SmallButton("1/32"))
        s.engineScale = kRuntimeOutputScale;
    Help("The runtime's ini key 'Scale', default 1/32. Not a control of the network: it sits "
         "after the four control floats and goes to the post kernel that writes the output.\n\n"
         "Near 0 the network's answer never reaches the frame -- measured with a residual mean "
         "of 0.00024 against an input of 0.45, with enabled and disabled identical and every "
         "frame reported as processed. The slider will not go to 0 and the ini refuses it too; "
         "Intensity 0 is the way to see the game's own frame.",

         "A chave 'Scale' do ini do runtime, padrão 1/32. Não é controle da rede: fica depois "
         "dos quatro floats de controle e vai para o kernel de pós que escreve a saída.\n\n"
         "Perto de 0 a resposta da rede nunca chega no quadro -- medido com resíduo médio de "
         "0,00024 contra entrada de 0,45, com ligado e desligado idênticos e todo quadro dado "
         "como processado. O slider não vai a 0, e o ini também recusa; Intensidade 0 é o jeito "
         "de ver o quadro do próprio jogo.");
}

void RestartOnly(const PanelStatus &status)
{
    ImGui::TextDisabled(T("Restart-only: Stage=%d Events=%d NoBridge=%d NoBackBuffer=%d",
                          "Só na reinicialização: Stage=%d Events=%d NoBridge=%d NoBackBuffer=%d"),
                        status.stage, status.events, status.noBridge ? 1 : 0,
                        status.noBackBuffer ? 1 : 0);
    if (status.helperProcess)
        Help("Diagnostics that decide what the helper builds at startup, so they cannot be "
             "changed live -- set them in amd-nr.ini. Stage=1 stops before D3D12 loads, 2 before "
             "the engine, 3 is everything. Events is a bitmask: 1 bind, 2 draw, 4 clear, 8 "
             "destroy_swapchain, 16 overlay.",

             "Diagnósticos que decidem o que o ajudante constrói na inicialização, então não "
             "mudam ao vivo -- ajuste no amd-nr.ini. Stage=1 para antes da D3D12 carregar, 2 "
             "antes do motor, 3 é tudo. Events é máscara de bits: 1 bind, 2 draw, 4 clear, 8 "
             "destroy_swapchain, 16 overlay.");
    else
        Help("Diagnostics that decide what gets built at startup, so they cannot be changed live "
             "-- set them in amd-nr.ini. Stage=1 stops before D3D12 loads, 2 before the engine, 3 "
             "is everything. Events is a bitmask: 1 bind, 2 draw, 4 clear, 8 destroy_swapchain, "
             "16 overlay.",

             "Diagnósticos que decidem o que é construído na inicialização, então não mudam ao "
             "vivo -- ajuste no amd-nr.ini. Stage=1 para antes da D3D12 carregar, 2 antes do "
             "motor, 3 é tudo. Events é máscara de bits: 1 bind, 2 draw, 4 clear, 8 "
             "destroy_swapchain, 16 overlay.");
}

} // namespace

void DrawEngine(PanelSettings &s, const PanelStatus &status)
{
    if (!GroupShown(s, status, kGrpEngine) || !SectionHeader(kHueEngine, T("Engine", "Motor")))
        return;
    ImGui::TextWrapped("%s", T("The engine's own option struct. Every offset came from decompiling "
                               "the runtime's ini reader, not from guesswork -- but an offset being "
                               "real says nothing about what writing it does. Every default here is "
                               "the engine's own, so an untouched section changes nothing.",

                               "A struct de opções do próprio motor. Todo offset veio de decompilar "
                               "o leitor de ini do runtime, não de chute -- mas um offset ser real "
                               "não diz nada sobre o que escrever nele faz. Todo padrão aqui é o do "
                               "próprio motor, então esta seção intocada não muda nada."));
    if (Shown(s, kOptMask))
        Mask(s);
    if (Shown(s, kOptTemporal))
        Temporal(s);
    if (Shown(s, kOptTonemap))
        Tonemap(s);
    if (Shown(s, kOptToneChannels))
        ToneChannels(s);
    if (Shown(s, kOptOutputScale))
        OutputScale(s);
    RestartOnly(status);
}

} // namespace ui
