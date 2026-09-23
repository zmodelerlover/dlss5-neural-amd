#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../view_logic.h"
#include "../widgets/widgets.h"

#include <algorithm>
#include <cstdio>

namespace ui {

namespace {

// Same frame versus async. The one control whose meaning depends on the route, because the two
// pipeline in different places: the add-on chooses when the engine's answer is composed, the
// bridge chooses whether this process waits for the helper at all -- inside the helper the network
// always runs same-frame. Same setting, same values, two honest descriptions.
void Timing(PanelSettings &s)
{
    int timing = s.inlineMode != 0 ? 0 : 1;
    // Red is the documented device-removal path -- same frame, waiting on the GPU, above full
    // resolution -- and nothing else here earns a colour. The old amber lit whenever Scale passed
    // 0.50 or Passes passed 1, which is most of a working configuration: a warning that is on
    // while everything is fine is a warning nobody reads.
    if (Risk r(kDanger, s.inlineMode != 0 && s.scale > 1.0f);
        ImGui::Combo(T("Timing", "Momento"), &timing,
                     T("Same frame\0Async\0", "Mesmo quadro\0Assíncrono\0")))
        s.inlineMode = timing == 0 ? 1u : 0u;
}

void TimingHelp(const PanelStatus &status)
{
    if (status.helperProcess)
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
    else
        Help("Same frame waits for this frame's own result: correct, and every millisecond the "
             "network costs is a millisecond of frame time.\n\n"
             "Async pastes an older correction instead, which is cheaper and can show the "
             "correction of a picture that has already moved -- that is what a trail behind "
             "moving objects is.",

             "Mesmo quadro espera o resultado deste quadro: correto, e cada milissegundo que a "
             "rede custa é milissegundo de tempo de quadro.\n\n"
             "Assíncrono cola uma correção anterior, o que é mais barato e pode mostrar a "
             "correção de uma imagem que já andou -- é isso o rastro atrás de coisa em "
             "movimento.");
}

// Keep the value being dragged separate from the value the engine consumes. SliderFloat changes
// on every mouse movement; publishing each intermediate float made the engine build a complete
// network raster every frame, while the runtime and driver kept the retired allocations resident.
// Commit once, when the edit ends.
void Scale(PanelSettings &s, PanelActions &actions)
{
    // ImGui-style widget state: it lives exactly as long as a drag, and there is one Scale slider.
    static float editing = 0.0f;
    static bool active = false;
    if (!active)
        editing = s.scale;
    float v = editing;
    if (Risk r(kDanger, v > 1.0f && s.inlineMode != 0);
        ImGui::SliderFloat(T("Scale", "Escala"), &v, 0.25f, 2.0f, "%.2f", 0))
        editing = v;
    if (ImGui::IsItemActive())
        active = true;
    if (ImGui::IsItemDeactivated())
    {
        if (active)
        {
            // Letting go of the slider is the person overruling a cap the card's own limit put
            // on, even when they let go on the number they started from -- which is exactly what
            // somebody capped at 0.75 does when their Scale already says 1.00, and the panel tells
            // them to let go of the slider to ask again. If the card still cannot carry it, three
            // long jobs put the cap back.
            s.scale = editing;
            actions.Add(PanelAction::LiftScaleCap);
        }
        active = false;
    }
    Help("Width and height the network runs at, relative to the game frame. 0.50 uses a "
         "quarter of the pixels. Lower is faster and the network answers differently, not "
         "just softer; above 1.00 it costs GPU time and memory for a frame that is already "
         "at full resolution.",

         "Largura e altura em que a rede roda, em relação ao quadro do jogo. 0.50 usa um "
         "quarto dos pixels. Menor é mais rápido e a rede responde diferente, não só mais "
         "suave; acima de 1.00 custa tempo de GPU e memória para um quadro que já está em "
         "resolução cheia.");
}

// Against what is actually being run at, not what the slider says: under a cap those two
// disagree by design, and comparing with the slider reported the raster as "not applied yet" for
// ever while it was working exactly as intended.
void Raster(const PanelSettings &s, const PanelStatus &status)
{
    if (status.outWidth != 0)
    {
        const float running = EffectiveScale(s.scale, status.scaleCap);
        const uint32_t wantW = std::max<uint32_t>(64u, static_cast<uint32_t>(status.outWidth * running + 0.5f));
        const uint32_t wantH = std::max<uint32_t>(64u, static_cast<uint32_t>(status.outHeight * running + 0.5f));
        if (wantW != status.netWidth || wantH != status.netHeight)
            ImGui::TextColored(kWarn, T("%ux%u, asked %ux%u", "%ux%u, pediu %ux%u"),
                               status.netWidth, status.netHeight, wantW, wantH);
        else
            ImGui::TextDisabled("%ux%u", status.netWidth, status.netHeight);
    }
    if (status.scaleCap > 0.0f && status.scaleCap < s.scale)
        Note(kWarn,
             T("Held below the slider by this card's own limit: one network run took long "
               "enough to reset the display driver and take the game with it. Let go of the "
               "slider to ask for the full scale again.",
               "Segurado abaixo do slider pelo limite desta placa: uma passada da rede levou "
               "tempo bastante para resetar o driver de vídeo e levar o jogo junto. Solte o "
               "slider para pedir a escala cheia de novo."));
}

void Passes(PanelSettings &s)
{
    int passes = s.passes;
    if (Risk r(kDanger, s.inlineMode != 0 && passes > 1 && s.scale > 1.0f);
        ImGui::SliderInt(T("Passes", "Passes"), &passes, 1, kMaxPasses, "%d", 0))
        s.passes = passes;
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
}

void Taper(PanelSettings &s)
{
    bool taper = s.passTaper != 0;
    if (ImGui::Checkbox(T("Taper passes", "Diminuir passes"), &taper))
        s.passTaper = taper ? 1u : 0u;
    Help("Halves Structure on each later pass: 1.0, 0.5, 0.25. Local tone already drops to zero "
         "after pass 1. Per-pass overrides win over this. Can reduce accumulated grain and "
         "outlines; compare in your scene.",
         "Reduz Estrutura pela metade a cada passe: 1.0, 0.5, 0.25. Tom local já cai a zero "
         "depois do passe 1. Ajustes por passe têm prioridade sobre isto. Pode reduzir "
         "granulado e contorno acumulados; compare na sua cena.");
}

// Per-pass profiles, the reference fork's "Per pass" tree. A later pass is looking at a picture
// an earlier one already edited, so the same numbers again ask it to sharpen its own sharpening --
// and that is the half of "3 passes looks deep fried" that the composition cannot reach from
// outside, because it happens inside the network.
void PerPass(PanelSettings &s)
{
    if (!ImGui::TreeNode(T("Per pass", "Por passe")))
        return;
    for (int i = 0; i < s.passes && i < kMaxPasses; ++i)
    {
        char label[32];
        std::snprintf(label, sizeof(label), T("Pass %d", "Passe %d"), i + 1);
        if (!ImGui::TreeNode(label))
            continue;
        ImGui::PushID(i);
        bool own = s.passOverride[i] != 0;
        if (ImGui::Checkbox(T("Own settings", "Ajustes próprios"), &own))
            s.passOverride[i] = own ? 1u : 0u;
        ImGui::BeginDisabled(!own);
        ImGui::SliderFloat(T("Structure", "Estrutura"), &s.passStructure[i], 0.0f, 3.0f, "%.2f", 0);
        ImGui::SliderFloat(T("Tone", "Tom"), &s.passTone[i], 0.0f, 3.0f, "%.2f", 0);
        ImGui::SliderFloat(T("Skin", "Pele"), &s.passSkin[i], -1.0f, 3.0f, "%.2f", 0);
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::TreePop();
    }
    Note(kWarn, T("A pass without its own settings follows the values above. The useful shape is a "
                  "taper, because each pass edits the last one's work. -1 Skin is the engine's "
                  "automatic.",
                  "Um passe sem ajustes próprios segue os valores acima. O formato útil é uma "
                  "queda, porque cada passe edita o trabalho do anterior. Pele -1 é o automático "
                  "do motor."));
    ImGui::TreePop();
}

void Bicubic(PanelSettings &s)
{
    bool bic = s.bicubic != 0;
    if (ImGui::Checkbox(T("Bicubic upsample", "Upsample bicúbico"), &bic))
        s.bicubic = bic ? 1u : 0u;
    Help("Below Scale 1.00 only the correction comes back up to full resolution. Stretching it "
         "bilinearly is a blur that throws away everything but colour and brightness, which alone "
         "made the whole effect look like a colour filter. Catmull-Rom keeps the rest. Turn it off "
         "if hard edges ring. Nothing at all at Scale 1.00.",

         "Abaixo de Escala 1.00 só a correção volta para a resolução cheia. Esticar ela "
         "bilinearmente é um borrão que joga fora tudo menos cor e brilho, e isso sozinho já fazia "
         "o efeito inteiro parecer um filtro de cor. Catmull-Rom mantém o resto. Desligue se "
         "arestas duras ficarem com halo. Nada em Escala 1.00.");
}

} // namespace

void DrawPerformance(PanelSettings &s, const PanelStatus &status, PanelActions &actions)
{
    if (!SectionHeader(kHuePerf, T("Performance", "Desempenho"), true))
        return;
    Timing(s);
    TimingHelp(status);
    Scale(s, actions);
    Raster(s, status);
    Passes(s);
    if (Shown(s, kOptTaper) && s.passes > 1)
        Taper(s);
    if (Shown(s, kOptPerPass) && s.passes > 1)
        PerPass(s);
    if (Shown(s, kOptBicubic))
        Bicubic(s);
}

} // namespace ui
