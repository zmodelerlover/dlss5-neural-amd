#pragma once
// The panel: one implementation, drawn by the 64-bit add-on and by the 32-bit bridge.
//
// The port is this one call. The adapter fills the settings and the status from what it owns,
// calls DrawPanel inside ReShade's overlay callback, applies back whatever changed in the settings,
// and carries out the actions -- the panel itself touches no engine state, no ini, no pipe.

#include "panel_model.h"

namespace ui {

PanelActions DrawPanel(PanelSettings &settings, const PanelStatus &status);

} // namespace ui
