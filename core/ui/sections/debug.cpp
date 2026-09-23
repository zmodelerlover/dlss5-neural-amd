#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../view_logic.h"
#include "../widgets/widgets.h"

namespace ui {

namespace {

void View(PanelSettings &s)
{
    int dbg = s.debugView;
    if (ImGui::Combo(T("View", "Visão"), &dbg,
                     T("Off\0Network input\0Network output\0Residual x8\0Motion\0Depth x500\0",
                       "Desligado\0Entrada da rede\0Saída da rede\0Resíduo x8\0Movimento\0Profundidade x500\0")))
        s.debugView = dbg;
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
}

void Limits(PanelSettings &s)
{
    ImGui::SliderFloat(T("Limit", "Limite"), &s.residualLimit, 0.0f, 0.50f,
                       s.residualLimit <= 0.0f ? T("off", "desligado") : "%.3f", 0);
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

    ImGui::SliderFloat(T("Edges", "Bordas"), &s.residualFade, 0.0f, 0.25f,
                       s.residualFade <= 0.0f ? T("off", "desligado") : "%.3f", 0);
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
}

// A request, not a call: on the bridge the engine is in the other process, and on both routes the
// overlay callback is not where the engine is touched.
void Measure(PanelActions &actions)
{
    if (ImGui::Button(T("Measure residual", "Medir resíduo")))
        actions.Add(PanelAction::MeasureResidual);
    Help("Writes a 'measure, residual' line to the log: the size of the correction, and how "
         "much of it follows the image's own detail. Change one control, press this, compare "
         "the two numbers. It is the only way to tell a control that does something from one "
         "that does not.",

         "Escreve uma linha 'measure, residual' no log: o tamanho da correção, e quanto dela "
         "segue o detalhe da própria imagem. Mude um controle, aperte isto, compare os dois "
         "números. É o único jeito de separar um controle que faz algo de um que não faz.");
}

} // namespace

// Diagnostics, and the two controls that answer "my picture has artefacts". They live here rather
// than under Image because Residual x8 is how you see what either one did.
void DrawDebug(PanelSettings &s, const PanelStatus &status, PanelActions &actions)
{
    if (!SectionHeader(kHueDebug, "Debug"))
        return;
    View(s);
    Limits(s);
    if (Shown(s, kOptMeasure))
        Measure(actions);
    if (status.routeDiagnostics.empty())
        return;
    ImGui::Separator();
    for (const std::string &line : status.routeDiagnostics)
        ImGui::TextDisabled("%s", line.c_str());
}

} // namespace ui
