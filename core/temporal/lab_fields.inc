// The temporal-stability controls, as X(type, field, low, high) like settings_fields.inc and read
// through the same X definitions right beside it. 64-bit route only: they are not in that list
// because it is also the bridge's wire, and WireSettings is frozen at 224 bytes (bridge_ipc.h), so
// the bridge offers none of them (view_logic.cpp, kOptHostOnly) until its protocol moves.
X(int32_t, fixedSeed, 0, 1)
X(float, outputSmooth, 0, 0.95)
X(float, outputSmoothLimit, 1, 32)
