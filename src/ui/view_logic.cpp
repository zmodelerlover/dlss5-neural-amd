#include "view_logic.h"

#include "i18n.h"

namespace ui {

const OptRow kOpts[] {
    { kOptCompose,      kGrpImage,  "Composition",       "Composição" },
    { kOptGuard,        kGrpImage,  "Highlight guard",   "Trava de realce" },
    { kOptGuardPerPass, kGrpImage,  "Guard per pass",    "Trava por passe" },
    { kOptLocalTone,    kGrpImage,  "Local tone",        "Tom local" },
    { kOptTaper,        kGrpPerf,   "Taper passes",      "Diminuir passes" },
    { kOptPerPass,      kGrpPerf,   "Per-pass settings", "Ajustes por passe" },
    { kOptBicubic,      kGrpPerf,   "Bicubic upsample",  "Upsample bicúbico" },
    { kOptMeasure,      kGrpDebug,  "Measure residual",  "Medir resíduo" },
    { kOptFeed,         kGrpGuides, "Use Feed.fx",       "Usar o Feed.fx" },
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
const int kOptCount = static_cast<int>(sizeof(kOpts) / sizeof(kOpts[0]));

const char *OptGroupName(int grp)
{
    static const char *en[] { "Performance", "Image", "Debug", "Guides", "Engine" };
    static const char *pt[] { "Desempenho", "Imagem", "Debug", "Guias", "Motor" };
    return T(en[grp], pt[grp]);
}

uint32_t AvailableOpts(const PanelStatus &status)
{
    // Built from the table rather than spelled out, so a row this route does not have is not
    // claimed by it -- which is also what the All button writes.
    uint32_t bits = 0;
    for (int i = 0; i < kOptCount; ++i)
        if (kOpts[i].bit != kOptFeed || status.hasFeedEffect)
            bits |= kOpts[i].bit;
    return bits;
}

bool GroupShown(const PanelSettings &s, const PanelStatus &status, int grp)
{
    const uint32_t on = s.optional & AvailableOpts(status);
    for (int i = 0; i < kOptCount; ++i)
        if (kOpts[i].group == grp && (on & kOpts[i].bit) != 0)
            return true;
    return false;
}

float EffectiveScale(float scale, float cap)
{
    return cap > 0.0f && cap < scale ? cap : scale;
}

double SkippedPercent(const PanelStatus &status)
{
    const uint64_t seen = status.processed + status.skipped;
    return seen != 0 ? 100.0 * static_cast<double>(status.skipped) / static_cast<double>(seen) : 0.0;
}

bool Skipping(const PanelStatus &status)
{
    return status.processed + status.skipped > 300 && SkippedPercent(status) >= 10.0;
}

const char *GuideSourceName(GuideSource source)
{
    switch (source)
    {
    case GuideSource::Game:      return T("game", "jogo");
    case GuideSource::Effect:    return T("effect", "effect");
    case GuideSource::Snapshot:  return T("snapshot", "snapshot");
    case GuideSource::Estimated: return T("estimated", "estimado");
    case GuideSource::None:      break;
    }
    return T("none", "nenhuma");
}

PanelSettings KeepPreferences(PanelSettings factory, const PanelSettings &current)
{
    factory.enabled = current.enabled;
    factory.startOn = current.startOn;
    factory.toggleKey = current.toggleKey;
    factory.toggleMods = current.toggleMods;
    factory.disableOnAltTab = current.disableOnAltTab;
    factory.language = current.language;
    factory.optional = current.optional;
    return factory;
}

} // namespace ui
