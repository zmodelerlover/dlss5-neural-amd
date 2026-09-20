# Handoffs

Start with [PROJECT_HANDOFF_2026-09-20.md](PROJECT_HANDOFF_2026-09-20.md). It is the current
state: the OpenGL route, what it cost to learn, the two settings it added, the diagnostics that
now exist, the published artefacts and what is still missing.

Older ones are kept because they record how things got here, and they describe branches and
dirty trees that no longer exist. Each one carries a dated note at the top saying what has
since changed, so read that before trusting the body:

- [PROJECT_HANDOFF_2026-09-14.md](PROJECT_HANDOFF_2026-09-14.md) — architecture, the x86 bridge,
  the rejected experiments and the full test matrix as of v0.5.0. Still the best account of the
  bridge and of why the routes are shaped the way they are, and the best account anywhere of how
  a measurement was read wrongly three times. Its §0 errata lists the four things it says that are
  no longer true, the pinned runtime and the x86 presentation mode among them.
- [HANDOFF_x86_hotkey_2026-09-15.md](HANDOFF_x86_hotkey_2026-09-15.md) — the hotkey capture and the
  32-bit overlay work, in Portuguese. All of it shipped in v0.5.1; what survives is the method and
  the list of traps at the end, `GetAsyncKeyState` inside a ReShade overlay first among them.
