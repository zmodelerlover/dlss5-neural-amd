#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../widgets/widgets.h"

#include <algorithm>

namespace ui {

namespace {

// Live, and deliberately so: judging three looks by editing an ini and restarting is three
// restarts, and on a host whose swapchain is in a child window Ctrl+Home cannot reload the file at
// all. Here it is one click and the frame changes under you.
//
// Both namings, because both are in circulation for the same three values of the same NGX field:
// RenoDX calls them Model A/B/C, Deep Fried Chicken calls them Default, Natural and Cinematic. A
// guide written against one and an overlay showing the other is a person changing the wrong
// control.
void Model(PanelSettings &s)
{
    int style = s.style;
    if (ImGui::Combo(T("Model", "Modelo"), &style,
                     T("A - Default\0B - Natural\0C - Cinematic\0",
                       "A - Default\0B - Natural\0C - Cinematic\0")))
        s.style = std::clamp(style, 0, 2);
    Help("The Neural Rendering Model -- the same three DLSSNR.Style selects on NVIDIA, but "
         "not yet doing the same thing here, which is why it sits under Experimental.\n\n"
         "On NVIDIA a model is two things: an input of the network, which is what moves "
         "lighting and detail, and a grade on the finished frame. This add-on does not feed "
         "the first to the network, so here a model is its grade only: B darkens "
         "slightly, flattens contrast and removes a tenth of the saturation; C removes 15 "
         "percent of the saturation. A colour change rather than a detail change, and a "
         "smaller difference than on NVIDIA.\n\n"
         "Two names for each: RenoDX writes Model A, B and C; Deep Fried Chicken writes "
         "Default, Natural and Cinematic, in that order.",

         "O Modelo de Renderização Neural -- os mesmos três que o DLSSNR.Style seleciona na "
         "NVIDIA, mas ainda não fazendo a mesma coisa aqui, e é por isso que está em "
         "Experimental.\n\n"
         "Na NVIDIA um modelo é duas coisas: uma entrada da rede, que é o que move iluminação "
         "e detalhe, e um grade no quadro pronto. Este add-on não leva a primeira até a "
         "rede, então aqui um modelo é só o grade dele: B escurece de "
         "leve, achata o contraste e tira um décimo da saturação; C tira 15 por cento da "
         "saturação. Mudança de cor em vez de mudança de detalhe, e diferença menor que na "
         "NVIDIA.\n\n"
         "Dois nomes para cada um: o RenoDX escreve Model A, B e C; o Deep Fried Chicken "
         "escreve Default, Natural e Cinematic, nessa ordem.");

    if (s.style == 0)
        return;
    float ss = s.styleStrength;
    if (ImGui::SliderFloat(T("Strength", "Força"), &ss, 0.0f, 1.0f, "%.2f"))
        s.styleStrength = std::clamp(ss, 0.0f, 1.0f);
    Help("Scales the model's grade towards neutral. 1 is the full grade, 0 removes the colour "
         "change entirely. Ours -- NVIDIA has no separate knob for this.",
         "Escalona o grade do modelo em direção ao neutro. 1 é o grade inteiro, 0 tira a "
         "mudança de cor por completo. É nosso -- a NVIDIA não tem controle separado para "
         "isto.");
}

void NetworkOutput(PanelSettings &s, const PanelStatus &status)
{
    bool raw = s.networkOutput != 0;
    if (ImGui::Checkbox(T("Network output", "Saída da rede"), &raw))
        s.networkOutput = raw ? 1u : 0u;
    if (status.helperProcess)
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
    else
        Help("Shows the network's answer directly instead of composing it onto the game's frame. "
             "Preferred by eye in one game, where the composition trailed behind fast movement, "
             "and never measured against the composition anywhere else.\n\n"
             "It turns off every bound on the correction: nothing limits how far a pixel may "
             "move, and hue is whatever the network returned. That is the trade.",

             "Mostra a resposta da rede direto, em vez de compô-la sobre o quadro do jogo. "
             "Preferido a olho em um jogo, onde a composição deixava rastro atrás de movimento "
             "rápido, e nunca medido contra a composição em nenhum outro lugar.\n\n"
             "Desliga todo limite sobre a correção: nada limita o quanto um pixel pode andar, e o "
             "matiz é o que a rede devolveu. Essa é a troca.");
}

} // namespace

void DrawExperimental(PanelSettings &s, const PanelStatus &status)
{
    if (!SectionHeader(kHueExperimental, "Experimental"))
        return;
    Note(kWarn, T("Work in progress -- not yet behaving the way they do on NVIDIA.",
                  "Em desenvolvimento -- ainda não se comportam como na NVIDIA."));
    Model(s);
    NetworkOutput(s, status);
}

} // namespace ui
