#pragma once
// The panel's decisions that do not draw anything: which rows exist, which are on screen, what a
// number means for the status column. Kept apart from ImGui so it reads as rules, not as layout.

#include "panel_model.h"

namespace ui {

// Which header a row belongs under, in the cascade and in the panel alike.
enum OptGroup { kGrpPerf, kGrpImage, kGrpDebug, kGrpGuides, kGrpEngine, kGrpCount };

struct OptRow
{
    uint32_t bit;
    int group;
    const char *en, *pt;
};
extern const OptRow kOpts[];
extern const int kOptCount;

const char *OptGroupName(int grp);

// The rows this route can draw. Feed.fx is the one that depends on the route: see
// PanelSettings::useFeedEffect.
uint32_t AvailableOpts(const PanelStatus &status);

inline bool Shown(const PanelSettings &s, uint32_t bit)
{
    return (s.optional & bit) != 0;
}

// Whether a whole group has anything turned on, which is what decides if its header is drawn. A
// bit for a row this route does not have does not count: the 64-bit route may have written it.
bool GroupShown(const PanelSettings &s, const PanelStatus &status, int grp);

// What the network is really running at. The cap is the card's own limit: it lowers the scale when
// one evaluation takes long enough to risk the display driver. Comparing the raster against the
// slider instead would report a working configuration as "not applied yet" for as long as the cap
// held.
float EffectiveScale(float scale, float cap);

double SkippedPercent(const PanelStatus &status);

// A skipped frame reuses whatever the network textures hold, and a job still running is writing
// them while compose reads them. A few percent is invisible; a third of the frames is a correction
// that changes every frame, which reads as flicker.
bool Skipping(const PanelStatus &status);

const char *GuideSourceName(GuideSource source);

// Factory defaults keep what is a preference rather than tuning: Enabled, startup, hotkey,
// alt-tab, language, and which optional controls have a widget -- a panel somebody arranged is
// not something a reset should empty.
PanelSettings KeepPreferences(PanelSettings factory, const PanelSettings &current);

} // namespace ui
