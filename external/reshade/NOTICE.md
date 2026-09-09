# Third-party headers, vendored

These are not my files. They are checked in unmodified so that `git clone` + `build.ps1` works
with nothing else to download, which is the whole point of them being here. Both are permissively
licensed and allow exactly this.

## ReShade add-on SDK — `reshade*.hpp`

* Upstream: <https://github.com/crosire/reshade>, `include/` directory
* Version: **ReShade API 20** (`RESHADE_API_VERSION 20` in `reshade.hpp`), matching ReShade 6.8.x
* Copyright (C) 2021 Patrick Mours
* License: **BSD-3-Clause OR MIT** (SPDX header in every file)

## Dear ImGui — `imgui.h`, `imconfig.h`

* Upstream: <https://github.com/ocornut/imgui>, tag `v1.92.5-docking`
* Version: **1.92.5**, `IMGUI_VERSION_NUM 19250`, **docking branch**
* Copyright (C) 2014-2026 Omar Cornut
* License: **MIT**

The docking branch specifically: `reshade_overlay.hpp` builds a function table for
`ImGuiDockNodeFlags` / `ImGuiWindowClass`, which only exist there. The version number is not a
suggestion either — `reshade_overlay.hpp` has

```cpp
#if IMGUI_VERSION_NUM != 19250
#error Unexpected ImGui version, please update the "imgui.h" header to version 19250!
#endif
```

so a mismatch is a compile error, not a silent breakage. Only `imgui.h` and `imconfig.h` are
needed; the add-on never links imgui, ReShade passes its own function table in at load time.

## Verifying these are upstream

They are stored with `-text` in `.gitattributes`, so git does not touch line endings and the
bytes survive a clone intact. Compare against upstream if you want:

```
cf3cd150920ea310b4b1e2e4f11ab8d7221a9fc18755f4891a026563311b524c  reshade.hpp
8d83ec8a620c48da7a20d975bd27b3345b24d3f0d5b78ae4c4fe633d61b804eb  reshade_api.hpp
a716c44b6fbb064dbbb0826687728f21aa71ed45e13564e4dc1cdf36da5bdfa0  reshade_api_device.hpp
68caa17fef712043e810ab88770c15af009aa3c7693e5c115ddc324f2d0809a7  reshade_api_format.hpp
5bda8e07260c2c60f0fd18a78b11782aed8f69e5d24b5757874614caafdf416f  reshade_api_pipeline.hpp
32cdafe6d783813f3ac72ad43e426638d8d0fef56bcb558fad218a14cfb75ee3  reshade_api_resource.hpp
2717f56e2bd89af6d708c9c316353d6581c5a41d4526629fadba51d13be57a89  reshade_events.hpp
ed914b12fbc41ed038287b0163c0bc934772afcff2c22392044c6008adc5ae79  reshade_overlay.hpp
6e5687893594ebfaf8569280cfea83d025904a8a4e23bec073086f6c5bc8f7fb  imconfig.h
07562842049eaca4e47c3fcdcc9d8eafd442695b9856d6f6639e92e1bb6c737e  imgui.h
```

`sha256sum -c` against this block, or `Get-FileHash` one at a time on Windows.
