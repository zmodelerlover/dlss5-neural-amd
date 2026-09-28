"""The raster follows a back buffer size that has lasted, and never one that flaps.

EnsureResources (neural.cpp) used to keep the raster at its first size for good once the engine
was up: NFS windowed from 1080 to 1017 lines ran the network on a stretched frame for the rest of
the session. It now asks RasterPin (core/shared/raster_pin.h), which is compiled here with g++ and
fed the sizes the games were logged presenting, so the rule is run, not read:
  - Xenosaga 2 walking 1920x1080, 1918x1014, 1918x994 and 1918x1008, 3 to 5 presents each, for
    2000 presents never re-rasters (the rule must be AND: every step is over 2%);
  - PCSX2 flapping between 1920x974 and 1920x971, and then holding 971, never re-rasters (0.3%);
  - NFS going from 1080 to 1017 lines keeps the old raster for exactly 120 presents and re-rasters
    on the 121st, once; a raster already at the wanted size is never kept;
  - the edges of the rule, each held 300 presents: 1920 to 1880 columns (width only) and 1080 to
    1058 lines (22 * 50 > 1080, just over 2%) re-raster once after 120 presents, and 1080 to 1059
    lines (21 * 50 <= 1080, just under) never does.
And read from the text: EnsureResources asks g.rasterPin.Keep in its pin, and nothing else, once
the engine is up, which both runtimes say: danielblnc's InitEngine and mochizuki's MzInit (with
mochizuki selected the pin never held, and PCSX2's flaps re-rastered and dropped history).

    python tools/raster_pin_check.py

It needs a C++20 g++: the one CXX names, else g++ on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
neural = (ROOT / "core/addon/neural.cpp").read_text(encoding="utf-8")

HARNESS = r"""
#include "raster_pin.h"
#include <cstdio>
// EnsureResources' pin as the add-on runs it: a present whose size is not kept re-rasters to it.
struct Route {
    RasterPin pin;
    unsigned netW, netH, presents = 0, kept = 0, rasters = 0, firstRaster = 0;
    void Present(unsigned w, unsigned h) {
        ++presents;
        if (pin.Keep(w, h, netW, netH)) { ++kept; return; }
        if ((w != netW || h != netH) && !rasters++) firstRaster = presents;
        netW = w;
        netH = h;
    }
};
int fails = 0;
// 100 presents at the raster's own 1920x1080, then 300 at w x h.
Route Held(unsigned w, unsigned h) {
    Route r { {}, 1920, 1080 };
    for (unsigned i = 0; i < 400; ++i) r.Present(i < 100 ? 1920 : w, i < 100 ? 1080 : h);
    return r;
}
void Expect(bool ok, const char *what, const Route &r) {
    if (!ok) { ++fails; std::printf("FAIL %s: kept %u, re-rastered %u (first at %u)\n", what, r.kept, r.rasters, r.firstRaster); }
}
int main() {
    const unsigned xeno[4][2] = { { 1920, 1080 }, { 1918, 1014 }, { 1918, 994 }, { 1918, 1008 } };
    Route x { {}, 1920, 1080 };
    for (unsigned i = 0; x.presents < 2000; ++i)
        for (unsigned n = 3 + i % 3; n--;) x.Present(xeno[i % 4][0], xeno[i % 4][1]);
    Expect(x.rasters == 0 && x.kept > 1000, "Xenosaga 2's walk", x);
    Route p { {}, 1920, 974 };
    for (unsigned i = 0; i < 4000; ++i) p.Present(1920, i < 2000 && i % 2 == 0 ? 974 : 971);
    Expect(p.rasters == 0 && p.kept == 3000, "PCSX2's 974/971", p);
    Route f { {}, 1920, 1080 };
    for (unsigned i = 0; i < 1100; ++i) f.Present(1920, i < 100 ? 1080 : 1017);
    Expect(f.kept == RasterPin::kHold && f.kept == 120 && f.rasters == 1 && f.firstRaster == 221 && f.netH == 1017, "NFS 1080 to 1017", f);
    Route e { {}, 1920, 1080 };
    for (unsigned i = 0; i < 500; ++i) e.Present(1920, 1080);
    Expect(e.kept == 0 && e.rasters == 0, "a raster already at the wanted size", e);
    Route wo = Held(1880, 1080), over = Held(1920, 1058), under = Held(1920, 1059);
    Expect(wo.kept == 120 && wo.rasters == 1 && wo.firstRaster == 221 && wo.netW == 1880, "a width-only 1920 to 1880", wo);
    Expect(over.kept == 120 && over.rasters == 1 && over.firstRaster == 221 && over.netH == 1058, "1080 to 1058, just over 2%", over);
    Expect(under.kept == 300 && under.rasters == 0, "1080 to 1059, just under 2%", under);
    if (!fails) std::printf("PASS the raster pin holds Xenosaga 2 and PCSX2 and follows NFS's 1017 after 120 presents\n");
    return fails;
}
"""

bad = []
m = re.search(r"\n[^\n;]*\bEnsureResources\([^;{]*\)\s*{", neural)
ensure = neural[m.start():neural.index("\n}\n", m.start())] if m else ""
# The control line: a check that found nothing to read passes on nothing.
if not ensure:
    bad.append("could not find EnsureResources in neural.cpp")
elif ('#include "../shared/raster_pin.h"' not in neural
        or not re.search(r"!scaleChanged &&\s+g\.rasterPin\.Keep\(nw, nh, g\.netWidth, g\.netHeight\)\)", ensure)
        or "nw != g.netWidth || nh != g.netHeight" in ensure):
    bad.append("EnsureResources: the pin does not ask g.rasterPin.Keep, or still compares sizes on its own")
elif not re.search(r"if \(g\.engineReady && g\.netWidth != 0 && !scaleChanged &&\s+g\.rasterPin\.Keep\(", ensure):
    bad.append("EnsureResources: the pin is not keyed on the engine being up")
mz = (ROOT / "core/addon/mochizuki.inc").read_text(encoding="utf-8")
for name, text in (("InitEngine", neural), ("MzInit", mz)):
    at = text.find(f"\nbool {name}()")
    if at < 0 or "g.engineReady = true;" not in text[at:text.index("\n}\n", at)]:
        bad.append(f"{name}: brings its runtime up without saying so, and the raster pin never holds")

with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp) / "raster_pin.cpp", Path(tmp) / "raster_pin.exe"
    src.write_text(HARNESS, encoding="utf-8")
    cxx = os.environ.get("CXX", "g++")
    try:
        built = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", "-static",
                                "-I" + str(ROOT / "core/shared"), str(src), "-o", str(exe)],
                               capture_output=True, text=True)
    except FileNotFoundError:
        built = subprocess.CompletedProcess(cxx, 1, "", f"no compiler at {cxx!r}; set CXX to a g++")
    if built.returncode != 0:
        bad.append("raster_pin.h did not compile on its own:\n" + built.stderr[-2000:])
    else:
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        out = ran.stdout.strip()
        if ran.returncode != 0 or "PASS" not in out:
            bad.append(out or ran.stderr.strip())

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print(out)
