#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../view_logic.h"
#include "../widgets/widgets.h"

namespace ui {

namespace {

void Feed(PanelSettings &s, const PanelStatus &status)
{
    bool feed = s.useFeedEffect != 0;
    if (ImGui::Checkbox(T("Use Feed.fx", "Usar o Feed.fx"), &feed))
        s.useFeedEffect = feed ? 1u : 0u;
    Help("The companion effect in shaders/. It hands over a real optical-flow field from a "
         "motion-vector shader -- iMMERSE Launchpad, VORT, LumeniteFX -- and ReShade's own "
         "depth buffer. That beats this add-on's own estimator, which is two levels of block "
         "matching with a search radius of four because it shares the frame with the network; "
         "Launchpad runs eight levels. A game that renders its own velocity buffer still beats "
         "both.",

         "O effect companheiro, em shaders/. Ele entrega um campo de fluxo óptico de verdade, "
         "vindo de um shader de vetores de movimento -- iMMERSE Launchpad, VORT, LumeniteFX -- "
         "e o depth buffer do próprio ReShade. Isso ganha do estimador deste add-on, que são "
         "dois níveis de block matching com raio quatro porque divide o frame com a rede; o "
         "Launchpad roda oito. Um jogo que desenha o próprio velocity buffer ainda ganha dos "
         "dois.");
    if (!status.feedStatus.empty())
        ImGui::TextWrapped("%s", status.feedStatus.c_str());
}

void GameGuides(PanelSettings &s, const PanelStatus &status)
{
    bool guides = s.useGameGuides != 0;
    if (ImGui::Checkbox(T("Read from the game", "Ler do jogo"), &guides))
        s.useGameGuides = guides ? 1u : 0u;
    if (status.helperProcess)
        Help("D3D11 only. Watches which depth-stencil and which two-channel float target the game "
             "binds most often, copies each once a frame and carries them across the bridge. Turn "
             "it off to fall back to estimated motion and no depth -- which is what you want when "
             "the detector picked the wrong buffer.",

             "Só D3D11. Observa qual depth-stencil e qual render target float de dois canais o "
             "jogo mais liga, copia cada um uma vez por quadro e leva pela ponte. Desligue para "
             "voltar a movimento estimado e nenhuma profundidade -- que é o que você quer quando o "
             "detector pegou o buffer errado.");
    else
        Help("D3D11 only. Watches which depth-stencil and which two-channel float target the game "
             "binds most often, copies each once a frame and carries them to the network's device. "
             "Turn it off to fall back to estimated motion and no depth -- which is what you want "
             "when the detector picked the wrong buffer.",

             "Só D3D11. Observa qual depth-stencil e qual render target float de dois canais o "
             "jogo mais liga, copia cada um uma vez por quadro e leva até o device da rede. "
             "Desligue para voltar a movimento estimado e nenhuma profundidade -- que é o que você "
             "quer quando o detector pegou o buffer errado.");
}

void Depth(PanelSettings &s)
{
    bool depth = s.useDepth != 0;
    if (ImGui::Checkbox(T("Depth", "Profundidade"), &depth))
        s.useDepth = depth ? 1u : 0u;
    Help("Hands the depth buffer to the engine and sets the flag that says it is valid. Depth "
         "mainly buys stability and disocclusion, not sharper texture.",
         "Entrega o buffer de profundidade ao motor e liga a flag que diz que ele é válido. "
         "Profundidade compra estabilidade e desoclusão, não textura mais afiada.");
}

void DepthInverted(PanelSettings &s)
{
    bool inv = s.depthInverted != 0;
    if (ImGui::Checkbox(T("Depth inverted", "Profundidade invertida"), &inv))
        s.depthInverted = inv ? 1u : 0u;
    Help("Which way round the buffer grows. 1 is the runtime's own default. RenoDX sends 0, "
         "measured with a dummy depth, so that 0 says nothing about any real game. Nothing here "
         "has told the two apart on a real buffer yet -- try both on a scene with depth and "
         "watch the residual.",

         "De que lado o buffer cresce. 1 é o padrão do próprio runtime. O RenoDX manda 0, "
         "medido com profundidade falsa, então esse 0 não diz nada sobre jogo nenhum. Nada aqui "
         "ainda separou os dois num buffer real -- teste os dois numa cena com profundidade e "
         "olhe o resíduo.");
}

void DepthStretch(PanelSettings &s)
{
    bool dn = s.depthNormalise != 0;
    if (Risk r(kWarn, dn); ImGui::Checkbox(T("Stretch depth", "Esticar profundidade"), &dn))
        s.depthNormalise = dn ? 1u : 0u;
    Help("Multiplies the depth guide by 1/max so a buffer occupying a fraction of 0..1 fills "
         "the range instead. It shipped ON and it is off now, because on this bench it reads as "
         "the wrong operation: the emulator's depth already has its bulk at the top of its own "
         "tiny range (probe: mean 0.00197 against max 0.00200), so scaling by 1/max lands nearly "
         "every pixel at 0.99 rather than spreading anything out -- which under the engine's "
         "inverted convention reads as \"the whole scene is against the camera\".\n\n"
         "Compare the 'measure, residual' line with it on and off before trusting it.",

         "Multiplica a guia de profundidade por 1/max, para que um buffer que ocupa uma fração "
         "de 0..1 passe a preencher a faixa. Vinha LIGADA e agora vem desligada, porque nesta "
         "bancada parece ser a operação errada: a profundidade do emulador já tem o grosso dos "
         "pixels no topo da própria faixa minúscula (probe: média 0,00197 contra máximo "
         "0,00200), então escalar por 1/max joga quase todo pixel em 0,99 em vez de espalhar -- "
         "o que sob a convenção invertida do motor lê como \"a cena inteira está colada na "
         "câmera\".\n\n"
         "Compare a linha 'measure, residual' com ela ligada e desligada antes de confiar.");
}

void History(PanelSettings &s)
{
    bool hist = s.useHistory != 0;
    if (Risk r(kWarn, hist); ImGui::Checkbox(T("History", "Histórico"), &hist))
        s.useHistory = hist ? 1u : 0u;
    Help("Hands the engine last frame's output to carry forward. Motion vectors say where a "
         "pixel was; without history there is nothing for them to point at.\n\n"
         "Experimental: it writes a pointer into the runtime at a fixed offset, and a wrong one "
         "there hangs the game rather than failing. If the picture smears or the game stops "
         "responding, this is the first thing to turn off.",

         "Entrega ao motor a saída do quadro anterior. Vetores de movimento dizem onde um pixel "
         "estava; sem histórico não há para onde eles apontarem.\n\n"
         "Experimental: escreve um ponteiro no runtime num offset fixo, e um ponteiro errado ali "
         "congela o jogo em vez de falhar. Se a imagem borrar ou o jogo parar de responder, esta "
         "é a primeira coisa a desligar.");
}

void Motion(PanelSettings &s, const PanelStatus &status)
{
    bool mv = s.useMotion != 0;
    if (ImGui::Checkbox(T("Motion", "Movimento"), &mv))
        s.useMotion = mv ? 1u : 0u;
    if (status.gameMotionActive)
        Help("Read from the game's own velocity buffer -- the real thing, per pixel.",
             "Lidos do próprio buffer de velocidade do jogo -- a coisa real, por pixel.");
    else
        Help("Estimated by comparing consecutive frames, because this target has no velocity "
             "buffer. It is wrong wherever pixels move without the geometry moving: reflections, "
             "fire, moving shadows, anything appearing from behind something else. The NVIDIA "
             "route does the same thing here.",

             "Estimados comparando quadros consecutivos, porque este alvo não tem buffer de "
             "velocidade. É errado onde pixels se movem sem a geometria se mover: reflexos, fogo, "
             "sombras em movimento, e qualquer coisa que aparece de trás de outra. A rota da "
             "NVIDIA faz o mesmo aqui.");
}

void MotionScale(PanelSettings &s)
{
    ImGui::SliderFloat(T("Motion scale", "Escala do mov."), &s.motionScale, -2.0f, 2.0f, "%.2f", 0);
    Help("Multiplies the motion field on its way into the engine, whichever field that is. 0 "
         "says nothing moved, 1 is as measured, -1 flips the direction, 0.5 suits a buffer "
         "stored in NDC.\n\n"
         "Set it by eye: put the Debug view on Motion and pan the camera. The field should "
         "follow steadily. Shimmer means it is too high -- turn it down rather than turning "
         "motion off, which is the blunt version of the same thing.",

         "Multiplica o campo de movimento no caminho para o motor, seja qual for o campo. 0 diz "
         "que nada se moveu, 1 é como foi medido, -1 inverte a direção, 0,5 serve para um buffer "
         "em NDC.\n\n"
         "Ajuste no olho: ponha a Visão de debug em Movimento e gire a câmera. O campo tem que "
         "acompanhar, firme. Cintilar quer dizer alto demais -- baixe, em vez de desligar o "
         "movimento, que é a versão bruta da mesma coisa.");
}

void FlowGate(PanelSettings &s)
{
    ImGui::SliderFloat(T("Flow gate", "Portão do fluxo"), &s.flowGate, 0.002f, 0.10f, "%.3f", 0);
    Help("The first of two filters deciding which pixels may move, applied BEFORE the search. If "
         "the brightest and darkest luma in the 3x3 block differ by less than this, the pixel is "
         "declared still and no search happens.\n\n"
         "A flat block -- clear sky, a painted wall -- matches equally well at every offset, so "
         "the winner is whichever the loop tried first. That is the aperture problem, and it is "
         "why widening the search made the field wilder instead of better.\n\n"
         "Raise it and more of the screen is frozen: cleaner, but the network is told nothing "
         "moved. 0.020 froze 99% of a dark scene on the bench. The number is a luma difference "
         "on 0..1, so 0.020 is 2% contrast. Estimated motion only.",

         "O primeiro de dois filtros que decidem quais pixels podem se mover, aplicado ANTES da "
         "busca. Se a luma mais clara e a mais escura do bloco 3x3 diferem menos que isto, o "
         "pixel é declarado parado e nenhuma busca acontece.\n\n"
         "Um bloco chapado -- céu limpo, parede pintada -- casa igualmente bem em todo "
         "deslocamento, então o vencedor é o primeiro que o laço testou. É o problema da "
         "abertura, e é por isso que alargar a busca deixou o campo mais doido em vez de "
         "melhor.\n\n"
         "Aumente e mais da tela fica congelada: mais limpo, mas a rede é informada de que nada "
         "se moveu. 0,020 congelou 99% de uma cena escura na bancada. O número é diferença de "
         "luma em 0..1, então 0,020 é 2% de contraste. Só vale para movimento estimado.");
}

void FlowAccept(PanelSettings &s)
{
    ImGui::SliderFloat(T("Flow accept", "Aceite do fluxo"), &s.flowRatio, 0.50f, 1.00f, "%.2f", 0);
    Help("The second filter, applied AFTER the search. The estimator keeps the best of 81 "
         "candidate offsets and also records the error of not moving at all; the winner is "
         "believed only if its error is below the standing-still error times this.\n\n"
         "So it runs backwards from what you would guess: 1.00 is the most PERMISSIVE and 0.50 "
         "is the strictest. Lower means more of the screen is forced still.\n\n"
         "Gate rejects a block before searching, on the grounds there is nothing in it. Accept "
         "rejects a result after searching, on the grounds the answer is not convincing. "
         "Estimated motion only.",

         "O segundo filtro, aplicado DEPOIS da busca. O estimador fica com o melhor de 81 "
         "deslocamentos candidatos e também guarda o erro de não se mover; o vencedor só é "
         "aceito se o erro dele for menor que o erro de ficar parado vezes isto.\n\n"
         "Ou seja, anda ao contrário do que se imagina: 1.00 é o mais PERMISSIVO e 0,50 é o mais "
         "rígido. Mais baixo significa mais tela forçada a parada.\n\n"
         "O Portão rejeita um bloco antes de buscar, com o argumento de que não há nada nele. O "
         "Aceite rejeita um resultado depois de buscar, com o argumento de que a resposta não "
         "convence. Só vale para movimento estimado.");
}

} // namespace

// Only under the cascade. Everything here is automatic, or ships at the engine's own default, or
// is a question waiting for a measurement -- which is exactly why it is not on screen by default,
// and exactly why it is still here. The right column shows what the guides ended up being; this is
// where they can be overruled, one control at a time: each row waits for its own tick.
void DrawGuides(PanelSettings &s, const PanelStatus &status)
{
    if (!GroupShown(s, status, kGrpGuides) || !SectionHeader(kHueGuides, T("Guides", "Guias")))
        return;
    if (status.hasFeedEffect && Shown(s, kOptFeed))
        Feed(s, status);
    if (Shown(s, kOptGameGuides))
        GameGuides(s, status);
    if (Shown(s, kOptDepth))
        Depth(s);
    if (s.useDepth != 0 && Shown(s, kOptDepthInv))
        DepthInverted(s);
    if (s.useDepth != 0 && Shown(s, kOptDepthStretch))
        DepthStretch(s);
    if (Shown(s, kOptHistory))
        History(s);
    if (Shown(s, kOptMotion))
        Motion(s, status);
    if (s.useMotion != 0 && Shown(s, kOptMotionScale))
        MotionScale(s);
    // Both filters belong to the estimator, which a game's own velocity buffer replaces.
    if (s.useMotion != 0 && !status.gameMotionActive && Shown(s, kOptFlowGate))
        FlowGate(s);
    if (s.useMotion != 0 && !status.gameMotionActive && Shown(s, kOptFlowAccept))
        FlowAccept(s);
}

} // namespace ui
