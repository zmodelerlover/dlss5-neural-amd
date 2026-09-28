// The one settings table, X(type, field, ini key, default, low, high). The ini reader and writer,
// both panels, the engine's constructed values and the 32-bit bridge's wire, laid out in this
// order, are all generated from it, so a setting means the same on every route.
// - The default is what g is constructed with, what a key missing from amd-nr.ini reads as, and what
//   Factory Defaults restores.
// - The range is the widest the 64-bit route ever took from an ini: LoadSettings, the 64-bit panel
//   and the wire all clamp to it, so the bridge never clips a value the other route keeps.
//   +-1e9 stands for no bound: nothing on the 64-bit route ever bounded that key.
// - An empty key is not in the ini: Enabled starts as StartOn says.
X(uint32_t, enabled, "", 0, 0, 1)
X(uint32_t, startOn, "StartOn", 0, 0, 1)
X(uint32_t, disableOnAltTab, "DisableOnAltTab", 0, 0, 1)
X(uint32_t, serialPasses, "SerialPasses", 1, 0, 1)
// The engine runs same-frame on every route: an Inline=0 left in an ini reads as 1.
X(uint32_t, inlineMode, "Inline", 1, 1, 1)
X(uint32_t, passTaper, "PassTaper", 0, 0, 1)
X(uint32_t, guardTracksPasses, "GuardPerPass", 0, 0, 1)
X(uint32_t, bicubic, "Bicubic", 1, 0, 1)
X(uint32_t, networkOutput, "NetworkOutput", 0, 0, 1)
X(uint32_t, useMotion, "Motion", 1, 0, 1)
X(uint32_t, useHistory, "History", 1, 0, 1)
X(uint32_t, useDepth, "Depth", 1, 0, 1)
X(uint32_t, useGameGuides, "GameGuides", 1, 0, 1)
X(int32_t, toggleKey, "ToggleKey", 35, 0, 254)
X(int32_t, toggleMods, "ToggleMods", 1, 0, 7)
X(int32_t, language, "Language", 0, 0, 1)
X(int32_t, passes, "Passes", 1, 1, 3)
X(int32_t, encoding, "Encoding", 0, -1e9, 1e9)
X(int32_t, debugView, "DebugView", 0, 0, 5)
X(int32_t, autoMask, "AutoMask", 1, -1e9, 1e9)
// Temporal 2 (On), with Fixed seed and Output smoothing below: the steadiest set measured on recorded
// play. Temporal=0 (Auto), FixedSeed=0 and OutputSmooth=0 are the defaults before it.
X(int32_t, temporalMode, "Temporal", 2, 0, 2)
X(int32_t, tonemap, "Tonemap", -1, -1e9, 1e9)
X(int32_t, toneChannels, "ToneChannels", 0, -1e9, 1e9)
// 1.0, the Scale a fresh amd-nr.ini has always been written with.
X(float, scale, "Scale", 1, 0.25, 2)
X(float, diffuseWhite, "DiffuseWhite", 100, -1e9, 1e9)
X(float, intensity, "Intensity", 1, -1e9, 1e9)
X(float, residualLimit, "ResidualLimit", 0.25, 0, 1e9)
X(float, residualFade, "EdgeFade", 0, 0, 0.49)
X(float, ratioGuard, "Guard", 2, 0, 8)
X(float, colourStrength, "ColourStrength", 1, 0, 1)
X(float, structure, "Structure", 1, -1e9, 1e9)
X(float, skin, "Skin", -1, -1e9, 1e9)
X(float, tone, "Tone", 1, -1e9, 1e9)
X(float, motionScale, "MotionScale", 1, -1e9, 1e9)
X(float, flowGate, "FlowGate", 0.02, -1e9, 1e9)
X(float, flowRatio, "FlowRatio", 0.7, -1e9, 1e9)
// LoadSettings also refuses anything under 1e-4 for the default, with a warning.
X(float, engineScale, "EngineScale", 0.03125, 0, 1e9)
// Added with the rebuilt panel: the 32-bit route needs the same controls the 64-bit one has.
// FeedEffect is deliberately not here -- the companion effect is read through ReShade's effect
// runtime, which lives in the frontend process, so the helper has nothing to do with the flag.
X(uint32_t, depthInverted, "DepthInverted", 1, 0, 1)
X(uint32_t, depthNormalise, "DepthNormalise", 0, 0, 1)
// One bit per optional control, saved as HiddenShown. The high bound is kOptAll, 25 bits; this
// macro takes a literal, so it is spelled out. LoadSettings masks it rather than clamping it.
X(uint32_t, optional, "HiddenShown", 0, 0, 33554431)
X(int32_t, style, "Style", 0, 0, 2)
X(float, styleStrength, "StyleStrength", 1, 0, 1)
// The temporal-stability controls (core/temporal/seed.inc, smooth.inc), on the wire since v5.
X(int32_t, fixedSeed, "FixedSeed", 1, 0, 1)
X(float, outputSmooth, "OutputSmooth", 0.8, 0, 1)
X(float, outputSmoothLimit, "OutputSmoothLimit", 10, -1e9, 1e9)
// The runtime's Quality, on a danielblnc build that has one (runtime_offsets.h, kQuality): 1 fast,
// its own default, 0 reference, NVIDIA's exact arithmetic. Written over its dlssnr_on_amd.ini.
X(int32_t, quality, "Quality", 1, 0, 1)
