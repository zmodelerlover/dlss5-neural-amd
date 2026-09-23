#pragma once
// The panel's colour rule, in one place, because a hue that means two things means nothing.
//
// TWO FAMILIES, AND THEY NEVER MIX.
//
//   STATE is a report on what is true right now, and it is always earned by a measurement or by
//   a documented failure -- never by a threshold on a slider. The old panel painted Timing,
//   Scale and Passes amber the moment Scale went over 0.50 or Passes over 1, which is most of a
//   working configuration: amber that is on whenever somebody is using the add-on teaches people
//   to ignore amber, and then the one that matters goes unread. Scale 0.75 with every frame
//   finishing on time is not a warning about anything. So amber now comes from the skip rate,
//   from the card's own cap having fired, or from a switch whose off state was seen to break the
//   picture -- things the add-on observed, not things it assumed.
//
//   SECTION is an identity, not a judgement. Each header carries its own hue so a thin panel
//   reads as regions instead of as one long list, and nothing inside a section is tinted by it.
//   Blue is the picture, green is speed, violet is instrumentation, amber is provisional, teal
//   is what feeds the network, salmon is somebody else's struct.
//
// The same values on both routes on purpose: somebody who plays a 32-bit game and a 64-bit one
// should not have to learn the overlay twice.

#include "imgui_api.h"

namespace ui {

inline constexpr ImVec4 kWarn { 1.00f, 0.80f, 0.30f, 1.00f };
inline constexpr ImVec4 kDanger { 1.00f, 0.45f, 0.35f, 1.00f };
inline constexpr ImVec4 kOk { 0.38f, 0.86f, 0.48f, 1.00f };
// The frame count when nothing is wrong. Brighter than kOk because it sits beside the Enabled box
// and is the one line a person reads to know the add-on is running.
inline constexpr ImVec4 kRunning { 0.40f, 1.00f, 0.40f, 1.00f };
// The right column's own text: full weight, because it is the only place the run reports itself,
// but not white, so it reads as a report and not as a label.
inline constexpr ImVec4 kReport { 0.72f, 0.78f, 0.86f, 1.00f };

// The hues are held near full brightness and away from each other, and none of them is allowed
// to drop towards the header's own fill: a muted tint on a mid-grey header band is a hue you
// have to look for, and a label you have to look for is not doing the job of telling you which
// region you are in. Read at a glance from across the room, or it is decoration.
inline constexpr ImVec4 kHueImage { 0.40f, 0.78f, 1.00f, 1.00f };          // blue -- the picture
inline constexpr ImVec4 kHuePerf { 0.35f, 1.00f, 0.55f, 1.00f };           // green -- speed
inline constexpr ImVec4 kHueDebug { 0.78f, 0.60f, 1.00f, 1.00f };          // violet -- instrumentation
inline constexpr ImVec4 kHueExperimental { 1.00f, 0.72f, 0.22f, 1.00f };   // amber -- provisional
inline constexpr ImVec4 kHueGuides { 0.25f, 0.95f, 0.95f, 1.00f };         // cyan -- what feeds the network
inline constexpr ImVec4 kHueEngine { 1.00f, 0.55f, 0.42f, 1.00f };         // salmon -- somebody else's struct

} // namespace ui
