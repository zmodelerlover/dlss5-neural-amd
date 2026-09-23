#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../view_logic.h"
#include "../widgets/widgets.h"

#include <algorithm>

namespace ui {

namespace {

void Encoding(PanelSettings &s)
{
    int enc = s.encoding;
    if (ImGui::Combo(T("Encoding", "Codificação"), &enc, "sRGB\0Linear\0scRGB-nl\0"))
        s.encoding = enc;
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
    if (s.encoding == 0)
        return;
    ImGui::SliderFloat(T("Diffuse white", "Branco difuso"), &s.diffuseWhite, 80.0f, 1000.0f,
                       "%.0f nits", 0);
    Help("How many nits a value of 1.0 means, which sets the scale of the linear image the "
         "network is handed. 100 for linear BT.709, 203 for scRGB-nl, 250 for PQ. A wrong scale "
         "here looks like the network over- or under-reacting everywhere at once.",

         "Quantos nits um valor de 1.0 significa, o que define a escala da imagem linear "
         "entregue à rede. 100 para linear BT.709, 203 para scRGB-nl, 250 para PQ. Escala "
         "errada aqui parece a rede reagindo demais ou de menos em tudo ao mesmo tempo.");
}

void Strengths(PanelSettings &s)
{
    ImGui::SliderFloat(T("Intensity", "Intensidade"), &s.intensity, 0.0f, 2.0f, "%.2f", 0);
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

    ImGui::SliderFloat(T("Colour", "Cor"), &s.colourStrength, 0.0f, 1.0f, "%.2f", 0);
    Help("Whether the network's colour arrives with its light. This is the control for \"it "
         "changed the colours\": at 0 every pixel keeps the game's own hue and only its "
         "brightness carries what the network decided -- by construction, it cannot change "
         "colour there. 1 brings the network's colour with it.",

         "Se a cor da rede vem junto com a luz dela. É este o controle para \"mudou as "
         "cores\": em 0 cada pixel fica com a matiz do próprio jogo e só o brilho carrega o "
         "que a rede decidiu -- por construção, ali ele não consegue mudar cor. 1 traz a cor "
         "da rede junto.");

    ImGui::SliderFloat(T("Structure", "Estrutura"), &s.structure, 0.0f, 3.0f, "%.2f", 0);
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
}

// -1 is the value the engine boots with and it is a MODE -- "derive it from local structure" --
// not a strength, so it never belonged on the same axis as 0..3: every position between -1 and 0
// was a number with no meaning. Worse, both routes used to write 1.0 on a fresh install, which
// switched the automatic off before anybody had touched a control.
void Skin(PanelSettings &s)
{
    bool skinAuto = s.skin < 0.0f;
    if (ImGui::Checkbox(T("Auto skin", "Pele automática"), &skinAuto))
        s.skin = skinAuto ? -1.0f : 1.0f;
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
    if (skinAuto)
        return;
    float sk = s.skin;
    if (ImGui::SliderFloat(T("Skin", "Pele"), &sk, 0.0f, 3.0f, "%.2f", 0))
        s.skin = std::max(sk, 0.0f);
}

void Compose(PanelSettings &s)
{
    int comp = s.ratioGuard > 0.0f ? 1 : 0;
    const char *items[] = { T("Additive", "Aditiva"), T("Ratio", "Razão") };
    if (ImGui::Combo(T("Compose", "Composição"), &comp, items, 2))
        s.ratioGuard = comp == 1 ? 2.0f : 0.0f;
    Help("How the network's answer is put back onto the frame. Ratio is the default and the "
         "only one that survives more than one pass.\n\n"
         "Additive adds the correction channel by channel and clips what leaves the range, and a "
         "clipped channel is a hue rotation -- which is why two passes read as more saturation "
         "rather than more detail. Ratio compares luminance as a bounded ratio and blends two "
         "finished pictures, which cannot move hue. Kept switchable so both can be seen in one "
         "session.",

         "Como a resposta da rede volta para o quadro. Razão é o padrão e a única que sobrevive "
         "a mais de um passe.\n\n"
         "Aditiva soma a correção canal por canal e corta o que sai da faixa, e canal cortado é "
         "rotação de matiz -- por isso dois passes aparecem como mais saturação em vez de mais "
         "detalhe. Razão compara luminância como razão limitada e mistura duas imagens inteiras, "
         "o que não consegue mover matiz. Deixado trocável para dar para ver os dois na mesma "
         "sessão.");
}

void Guard(PanelSettings &s)
{
    // The additive composition is the guard at 0: no slider, and so no help beside it either.
    float gv = s.ratioGuard;
    if (gv <= 0.0f)
        return;
    if (ImGui::SliderFloat(T("Guard", "Trava"), &gv, 1.0f, 8.0f, "%.1fx", 0))
        s.ratioGuard = gv;
    Help("The most compose may move a pixel, as a multiple of what it already was, in both "
         "directions. One scalar taken from luminance and applied to the whole triple, so it "
         "bounds brightness without touching hue.\n\n"
         "2.0x is the reference fork's default and leaves detail intact -- 254 composition lines "
         "across seven games and nine machines all read 2.0x. Raise it only if bright areas look "
         "clipped.",

         "O máximo que a composição pode mover um pixel, como múltiplo do que ele já era, nos "
         "dois sentidos. Um escalar só, tirado da luminância e aplicado no trio inteiro, então "
         "limita brilho sem tocar em matiz.\n\n"
         "2.0x é o padrão do fork de referência e não come detalhe -- 254 linhas de composição "
         "em sete jogos e nove máquinas todas dizem 2.0x. Só aumente se áreas claras parecerem "
         "estouradas.");
}

void GuardPerPass(PanelSettings &s)
{
    bool track = s.guardTracksPasses != 0;
    if (ImGui::Checkbox(T("Guard per pass", "Trava por passe"), &track))
        s.guardTracksPasses = track ? 1u : 0u;
    Help("Adds one multiple of headroom per extra pass, so 2.0x becomes 3.0x at two passes. Off, "
         "because the reference fork does not actually do it: their ini says MaxRatio defaults "
         "to 2.0 and never mentions the count. A fixed bound is also the honest way to see what "
         "an extra pass is contributing.",

         "Acrescenta um múltiplo de folga por passe extra, então 2.0x vira 3.0x em dois passes. "
         "Desligado, porque o fork de referência não faz isso: a ini deles diz que MaxRatio tem "
         "padrão 2.0 e nunca cita a contagem. Um limite fixo também é o jeito honesto de ver o "
         "que um passe extra está somando.");
}

void LocalTone(PanelSettings &s)
{
    ImGui::SliderFloat(T("Local tone", "Tom local"), &s.tone, 0.0f, 3.0f, "%.2f", 0);
    Help("LocalToneStrength, the first control slot of the network, and -- clamped to 0..1 -- "
         "the scale of the Model's grade. RenoDX ships it at 1.\n\n"
         "An earlier sweep measured it inert on the network side, byte for byte. That reading "
         "was taken with the fifth control slot at an off-menu value and has not been repeated "
         "since.",

         "LocalToneStrength, o primeiro slot de controle da rede e -- limitado a 0..1 -- a "
         "escala do grade do Modelo. O RenoDX manda 1.\n\n"
         "Uma varredura anterior mediu isto inerte do lado da rede, byte a byte. Essa leitura "
         "foi feita com o quinto slot de controle num valor fora do menu e não foi repetida "
         "desde então.");
}

} // namespace

void DrawImage(PanelSettings &s)
{
    if (!SectionHeader(kHueImage, T("Image", "Imagem"), true))
        return;
    Encoding(s);
    Strengths(s);
    Skin(s);
    // The composition's own bounds. Real controls with real effects, but their defaults are what
    // seven games' worth of reference logs agree on, so the ordinary reason to touch one is to
    // answer a question rather than to tune a picture -- which is why each waits for somebody to
    // ask for it in the cascade at the bottom of the panel.
    if (Shown(s, kOptCompose))
        Compose(s);
    // Both inside the same BeginDisabled, because neither means anything on the additive
    // composition -- which is what the guard being 0 is.
    ImGui::BeginDisabled(s.ratioGuard <= 0.0f);
    if (Shown(s, kOptGuard))
        Guard(s);
    if (Shown(s, kOptGuardPerPass))
        GuardPerPass(s);
    ImGui::EndDisabled();
    if (Shown(s, kOptLocalTone))
        LocalTone(s);
}

} // namespace ui
