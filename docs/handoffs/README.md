# Handoffs

**Start here:** [HANDOFF-rocm-spike-knobs-e-paridade-de-pesos-20260922.md](HANDOFF-rocm-spike-knobs-e-paridade-de-pesos-20260922.md)
(Portuguese): the second session of 22/09, which changed no code and changed the map. The port runs
NVIDIA's own weights byte for byte, so the network is not where the two machines differ; the port's
HIP kernels are named and callable after all, which retires "precompiled, no source" as a reason to
close a question; the network has a fifth trained conditioning input the port ties to zero, on a
kernel that only runs behind an environment variable; the runtime has six undocumented environment
switches, two of them measured, showing it spends a third to two thirds of the effect's strength
buying temporal stability. Its section 8 is the list of this session's own mistakes and how to avoid
them, its section 9 is what is now measured-and-closed, and its section 10 is the measurement
protocol that actually produced trustworthy numbers. Where it contradicts the one below, it wins.

Before it, [HANDOFF-style-preset-fechado-e-roadmap-20260922.md](HANDOFF-style-preset-fechado-e-roadmap-20260922.md)
(Portuguese): the consolidated state after 21–22/09. NR Style and NR Preset closed (a Model on this
runtime is its grade only; there is no style input in the port's network), the "fifth control slot"
mistake that made the network return its input for a day and the watchdog that now reports that
class of failure, what the NVIDIA measurement settled, composition parity, the roadmap and the commit
order. Where it contradicts an older handoff, it wins.

Before it, [HANDOFF-style-control-e-proximos-passos-20260921.md](HANDOFF-style-control-e-proximos-passos-20260921.md)
(Portuguese) is the 21/09 evening session as it happened: everything it did, the
claim that Style is a network control the AMD runtime accepts (the field the add-on called
EngineScale) -- **retracted in its section 10**: that field is the post kernel's output scale, writing
style/128 there made the network return its input, and a Model on AMD is its grade only -- and, in
its section 8, what the NVIDIA logs settled and what was
changed because of them: history reset on every control the NVIDIA DLL resets on, the grade
scaled by Local Tone, NR Preset closed on both sides, overlay and docs rewritten. Its section 9
and `docs/nvidia-parity.md` answer "is it the same as NVIDIA now": what is the same expression,
what differs on purpose, and the ini recipe that reproduces the NVIDIA ETS2 configuration.

The NVIDIA-side measurement itself is
[RESULTADO-nvidia-preset-style-ets2-20260921.md](RESULTADO-nvidia-preset-style-ets2-20260921.md)
(Portuguese, the operator's notes): the `DLSSNR.*` table per Model, RenoDX's defaults, the NGX
log, the DLL hash and the static analysis of `Evaluate`, the reset function and the style table.
The script the operator ran is
[HANDOFF-nvidia-preset-style-ets2-20260921.md](HANDOFF-nvidia-preset-style-ets2-20260921.md).

Start with [HANDOFF-launchpad-multipass-20260921.md](HANDOFF-launchpad-multipass-20260921.md)
(Portuguese): the companion effect running in the right order, one runtime module per pass, the
validation latch that capped motion at 2.8 px, and NR Preset closed as impossible. All of it
measured in PCSX2. It follows [HANDOFF-feed-launchpad-20260921.md](HANDOFF-feed-launchpad-20260921.md),
which built the effect and the per-pass history without a bench, and whose section 7 is a list of
bench mistakes worth not repeating.

Before those, the current state of the routes is in [PROJECT_HANDOFF_2026-09-20.md](PROJECT_HANDOFF_2026-09-20.md):
the OpenGL route, what it cost to learn, the two settings it added, the diagnostics that
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
