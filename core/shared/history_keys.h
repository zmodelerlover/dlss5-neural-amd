#pragma once
// The settings whose change leaves the temporal history describing another picture than the next
// frame will: the raster (Scale), which way depth reads, whether history is kept at all, the
// temporal mode, everything that decides the motion handed with it, and the network's arithmetic
// (Quality, on which 0.5.0 drops its own history as well). One list for the 64-bit panel
// (ApplyPanelSettings) and the 32-bit helper (host64 ApplySettings, Factory Defaults included).
// OpticalFlow is in neither struct: only an ini read moves it, and LoadSettings drops the history.
template <class A, class B> bool SettingsInvalidateHistory(const A &a, const B &b)
{
    return a.scale != b.scale || a.depthInverted != b.depthInverted ||
           a.useHistory != b.useHistory || a.temporalMode != b.temporalMode ||
           a.useMotion != b.useMotion || a.useDepth != b.useDepth ||
           a.useGameGuides != b.useGameGuides || a.motionScale != b.motionScale ||
           a.flowGate != b.flowGate || a.flowRatio != b.flowRatio || a.quality != b.quality;
}
