#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../widgets/widgets.h"

#include <algorithm>

namespace ui {

namespace {

// Live, and deliberately so: judging three looks by editing an ini and restarting is three
// restarts. Both namings, because both are in circulation for the same three values: RenoDX calls
// them Model A/B/C, danielblnc and Deep Fried Chicken call them Default, Natural and Cinematic.
void Model(PanelSettings &s)
{
    int style = s.style;
    if (ImGui::Combo(T("Style", "Estilo"), &style,
                     T("Default (A)\0Natural (B)\0Cinematic (C)\0",
                       "Default (A)\0Natural (B)\0Cinematic (C)\0")))
        s.style = std::clamp(style, 0, 2);
    Help("danielblnc's Style: the look the network is asked for, the same three DLSSNR.Style "
         "selects on NVIDIA. It goes to the network itself, as in his own overlay, so it moves "
         "lighting and skin as well as colour. Changing it restarts the temporal history.",

         "O Style do danielblnc: o visual pedido à rede, os mesmos três que o DLSSNR.Style "
         "seleciona na NVIDIA. Vai para a própria rede, como no overlay dele, então muda "
         "iluminação e pele além da cor. Trocar reinicia o histórico temporal.");
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
