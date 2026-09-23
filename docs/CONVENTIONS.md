# Conventions

- **Layout.** `core/<layer>/`: `addon`, `transport/<api>`, `shaders`, `shared`, `ui`, `x86bridge`, `diagnostics/<tool>`. Tests live in the module's own `tests/`. Vendored code is in `3rdparty/` and is never edited.
- **Graphics APIs.** Anything that differs per API goes through `core/transport/FrameTransport.hpp`. `TransportFor` is the only place that reads `device_api`; `tools/transport_check.py` fails the build otherwise.
- **Failure.** Code that runs inside the game never throws. It sets `g.status.reason`, stands down and leaves the game's frame as the game drew it. `throw` is for tests and tools.
- **Comments.** Only the why that the code cannot say: a measurement, a bug already paid for, a constraint from outside. Never what a line does.
- **Style.** `.clang-format`, for new code and for code moved into a new home. Older code converges when it is touched, and a formatting change is always its own commit.
- **Size.** 500 lines per file. The few above it are listed in `tools/line_limit_allow.json` and may only shrink.
- **Commits.** One subject line in English, no body.
