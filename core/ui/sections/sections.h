#pragma once
// One function per region of the panel, drawn in this order by DrawPanel. Each takes the settings
// it edits in place, the status it reports and the actions it can ask for -- nothing else, and no
// state of its own beyond what an ImGui widget keeps while it is being dragged.
//
// The whole PanelSettings rather than a slice of it: it is one flat struct generated from the
// bridge's field list, and a per-section struct would be a second layout of the same fields to
// keep in step with the wire.

#include "../panel_model.h"

namespace ui {

void DrawGeneral(PanelSettings &s, const PanelStatus &status, PanelActions &actions);
void DrawPerformance(PanelSettings &s, const PanelStatus &status, PanelActions &actions);
void DrawImage(PanelSettings &s);
void DrawDebug(PanelSettings &s, const PanelStatus &status, PanelActions &actions);
void DrawExperimental(PanelSettings &s, const PanelStatus &status);
void DrawGuides(PanelSettings &s, const PanelStatus &status);
void DrawEngine(PanelSettings &s, const PanelStatus &status);
void DrawMoreSettings(PanelSettings &s, const PanelStatus &status);
void DrawFooter(const PanelStatus &status, PanelActions &actions);

} // namespace ui
