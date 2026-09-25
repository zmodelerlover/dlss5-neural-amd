"""The compose arithmetic, and the four things it has to be true about.

The composition lives in kComposeShader (core/shaders/compose.h) as HLSL and only ever runs
on a GPU inside a game, so a mistake in it is found by looking at a screen and disagreeing with
it. This is the same arithmetic
written out once more, in the smallest form that can be asserted against -- if the two ever
disagree, this file is the one that is wrong, and it is here to fail when a change to the shader
breaks a property the shader was written to have.

    python tools/compose_check.py

Mirrors the `guard > 0` branch of kComposeShader exactly: CubeScale, the luminance floor, the
two-sided guard, the chroma blend and the hue-preserving peak scale. Also the NaN/inf guards in
kResidualShader and kSmoothShader, the two places a value from the network is stored.
"""
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LUMA = (0.2126, 0.7152, 0.0722)
FLOOR = 1.0 / 512.0


def luma(c):
    return sum(a * b for a, b in zip(c, LUMA))


def cube_scale(p, t):
    """The largest fraction of (t - p) that leaves every channel inside [0,1]."""
    d = [b - a for a, b in zip(p, t)]
    a = 1.0
    for i in range(3):
        if d[i] > 1e-6:
            a = min(a, (1.0 - p[i]) / d[i])
        elif d[i] < -1e-6:
            a = min(a, (0.0 - p[i]) / d[i])
    a = max(0.0, min(1.0, a))
    return [p[i] + a * d[i] for i in range(3)]


def compose_ratio(P, E, colour, guard):
    M = cube_scale(P, [p + e for p, e in zip(P, E)])
    pl, ml = luma(P), luma(M)
    ratio = (ml + FLOOR) / (pl + FLOOR)
    bounded = max(1.0 / guard, min(guard, ratio))
    lit = ([m * (bounded / max(ratio, 1e-6)) for m in M] if ml > 1e-5
           else [p * bounded for p in P])
    v = [p * bounded + (l - p * bounded) * min(max(colour, 0.0), 1.0) for p, l in zip(P, lit)]
    peak = max(v)
    if peak > 1.0:
        v = [x / peak for x in v]
    return [max(0.0, x) for x in v]


def compose_additive(P, E):
    """What the add-on did before: per channel, then clipped per channel."""
    return [min(1.0, max(0.0, p + e)) for p, e in zip(P, E)]


def limit_residual(E, limit):
    """The whole triple scaled, never one channel clamped. Mirrors the `limit` block."""
    if limit <= 0.0:
        return list(E)
    mag = max(abs(x) for x in E)
    return [x * (limit / mag) for x in E] if mag > limit else list(E)


def edge_fade(E, uv, fade):
    """The `fade` block: roll the correction off over a band at the frame border, using the
    nearer of the two distances so a corner takes both rolloffs. uv is the output-space
    coordinate the shader computes as (p + 0.5) / (dw, dh)."""
    if fade <= 0.0:
        return list(E)
    e = [min(c, 1.0 - c) / fade for c in uv]
    return [x * min(1.0, max(0.0, min(e))) for x in E]


def non_finite(c):
    """The shaders' bit compare: an exponent of all ones is inf or NaN."""
    return any((struct.unpack("<I", struct.pack("<f", x))[0] & 0x7FFFFFFF) >= 0x7F800000 for x in c)


def residual_guard(E):
    """kResidualShader: a triple with a NaN or inf in it becomes no correction at all, and a finite
    one is clamped to the fp16 range it is stored in, so the store cannot turn it into inf."""
    return [0.0, 0.0, 0.0] if non_finite(E) else [min(65504.0, max(-65504.0, x)) for x in E]


def smooth(o, v, base, strength, threshold):
    """kSmoothShader, as fxc emits it (the lerp is o + w * (v - o), and saturate(NaN) is 0), then
    the store: the blend, else the network's pixel, else the warped one, else the input (netBase)."""
    d = max(abs(a - b) for a, b in zip(o, v))
    w = strength * min(1.0, max(0.0, 1.0 - d / threshold))
    r = [a + w * (b - a) for a, b in zip(o, v)]
    return next((list(c) for c in (r, o, v) if not non_finite(c)), list(base))


def chroma_direction(c):
    """Hue as a normalised direction away from grey. None for a pixel with no colour in it."""
    y = luma(c)
    d = [x - y for x in c]
    n = sum(x * x for x in d) ** 0.5
    return None if n < 1e-6 else [x / n for x in d]


def close(a, b, tol=1e-6):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


PIXELS = [
    [0.02, 0.02, 0.03],   # near-black, where a ratio is unbounded
    [0.20, 0.35, 0.60],   # ordinary blue-ish mid tone
    [0.85, 0.30, 0.10],   # saturated warm, the one a per-channel clip rotates
    [0.97, 0.95, 0.92],   # near-white, no headroom left
]
EDITS = [
    [0.0, 0.0, 0.0],
    [0.01, -0.01, 0.02],   # the size a single pass actually returns
    [0.06, -0.03, 0.09],   # about three passes' worth
    [0.60, -0.20, 0.45],   # absurd, to prove the bounds are bounds
]


def main():
    # 1. A network that returned its input changes nothing at all. This is what makes the
    #    add-on's own off-switch honest, and it has to hold at every setting.
    for P in PIXELS:
        for colour in (0.0, 0.5, 1.0):
            for guard in (1.0, 2.0, 4.0):
                out = compose_ratio(P, [0.0, 0.0, 0.0], colour, guard)
                assert close(out, P), f"zero edit moved {P} -> {out}"

    # 2. Colour Strength 0 keeps the game's hue exactly. This is the whole answer to "it changed
    #    the colours": at 0 it cannot, whatever the network returned.
    for P in PIXELS:
        want = chroma_direction(P)
        for E in EDITS:
            for guard in (1.0, 2.0, 4.0):
                out = compose_ratio(P, E, 0.0, guard)
                got = chroma_direction(out)
                if want is None or got is None:
                    continue
                assert close(want, got, 1e-4), f"hue moved at colour 0: {P} {E} -> {out}"

    # 3. The guard is a guard, and nothing leaves the cube.
    #
    #    Stated one-sided on purpose. Brightening is the failure being bounded -- an unbounded
    #    ratio against a dark pixel is the boiling and the blown highlights -- and the floor makes
    #    it exact: no pixel may end up brighter than guard times what it was, counting the floor.
    #    Darkening is bounded too at Colour Strength 0, where the answer is P * bounded, but the
    #    floor deliberately lets a pixel the network returned nearly black go darker than the
    #    reciprocal, because a pixel with no light in it should take no edit rather than a scaled
    #    one. That is the floor working, not the guard failing.
    for P in PIXELS:
        for E in EDITS:
            for guard in (1.0, 2.0, 4.0):
                for colour in (0.0, 0.5, 1.0):
                    out = compose_ratio(P, E, colour, guard)
                    ceiling = guard * (luma(P) + FLOOR)
                    assert luma(out) <= ceiling + 1e-6, \
                        f"{luma(out):.4f} is past the {guard}x ceiling {ceiling:.4f}"
                    assert all(-1e-6 <= x <= 1.0 + 1e-6 for x in out), f"left the cube: {out}"
                # At Colour Strength 0 the answer is exactly the frame times the bounded ratio,
                # so both sides of the bound are checkable.
                out = compose_ratio(P, E, 0.0, guard)
                moved = (luma(out) + FLOOR) / (luma(P) + FLOOR)
                assert 1.0 / guard - 1e-4 <= moved <= guard + 1e-4, \
                    f"colour 0 moved {moved:.3f}x outside {guard}x"

    # 4. The regression this replaced. A correction big enough to clip -- which is what a second
    #    and third pass produce, since additive composition multiplies the difference by the
    #    count -- rotates the hue of a saturated pixel when it is clipped per channel, and does
    #    not when the whole triple is scaled instead.
    P, E = [0.85, 0.30, 0.10], [0.60, -0.20, 0.45]
    before = chroma_direction(P)
    additive = chroma_direction(compose_additive(P, E))
    ratio = chroma_direction(compose_ratio(P, E, 0.0, 2.0))
    assert not close(before, additive, 0.05), "additive was expected to rotate this hue"
    assert close(before, ratio, 1e-4), "ratio at colour 0 must not rotate hue"

    # 5. The residual limit keeps the correction's direction and only gives up its size, and it
    #    is exactly nothing for a correction that was already inside it. The blown tile this
    #    exists for -- a measured maximum of 4.16 against a mean of 0.072 -- is what a per-channel
    #    clamp turned into a blown *coloured* tile.
    blown = [4.16, 0.9, 0.2]
    limited = limit_residual(blown, 0.25)
    assert abs(max(abs(x) for x in limited) - 0.25) < 1e-9, "the limit did not bind"
    for a, b in zip(blown, limited):
        assert abs(a * (0.25 / 4.16) - b) < 1e-9, "the limit changed the correction's direction"
    small = [0.01, -0.02, 0.005]
    assert close(limit_residual(small, 0.25), small), "an ordinary correction must be untouched"
    assert close(limit_residual(blown, 0.0), blown), "0 must mean off"

    # 6. Edge Fade. Nobody had ever checked this one, and "I cannot tell whether it is doing
    #    anything" is the reason: it removes a correction whose mean is 0.072 inside a band 2% of
    #    the frame wide, which is invisible on an ordinary frame and obvious on Residual x8.
    #    So the check is here instead of in somebody's eye.
    E = [0.072, -0.030, 0.011]
    band = 0.02
    assert close(edge_fade(E, (0.5, 0.5), band), E), "the middle of the frame must be untouched"
    assert close(edge_fade(E, (0.5, 0.5), 0.0), E), "0 must mean off"
    for uv in ((0.0, 0.5), (0.5, 0.0), (1.0, 0.5), (0.5, 1.0)):
        assert close(edge_fade(E, uv, band), [0.0, 0.0, 0.0]), f"the border at {uv} must go to zero"
    # A corner is inside two bands at once, so it takes the smaller of the two ramps and is the
    # first place the fade bites -- which is the artefact it was written for.
    corner = edge_fade(E, (band * 0.25, band * 0.75), band)
    edge = edge_fade(E, (band * 0.75, 0.5), band)
    assert max(abs(x) for x in corner) < max(abs(x) for x in edge), "a corner must fade harder"
    # Halfway into the band is half the correction, and the direction never moves: this is a
    # scale on the whole triple, like the limit above, so it cannot rotate hue either.
    half = edge_fade(E, (band * 0.5, 0.5), band)
    for a, b in zip(E, half):
        assert abs(a * 0.5 - b) < 1e-9, "the fade must scale the triple, not bend it"
    # The value the ini clamps to, and what it actually does: at 0.49 the bands very nearly meet
    # in the middle, so the one pixel at dead centre still passes at full strength (0.5 / 0.49 is
    # over 1) and everything else is dimmed by how far it is from there. A quarter of the way in
    # keeps about half. That is a vignette on the correction, not a border band -- worth knowing
    # before somebody drags the slider to the top wondering why the whole effect went quiet.
    assert close(edge_fade(E, (0.5, 0.5), 0.49), E), "dead centre survives even at the maximum"
    quarter = edge_fade(E, (0.25, 0.5), 0.49)
    for a, b in zip(E, quarter):
        assert abs(a * (0.25 / 0.49) - b) < 1e-9, "0.49 must dim a quarter-in pixel by 0.51"

    # 7. A NaN or inf from the network is no correction at all -- not the darkened pixel it made
    #    through the ratio path, nor the black one through the additive -- and never lands in the
    #    history the smooth writes, even where the blend weight is 0. Finite values pass as they were.
    for bad in (float("nan"), float("inf"), float("-inf")):
        E = residual_guard([0.01, bad, -0.02])
        for P in PIXELS:
            assert close(compose_ratio(P, E, 1.0, 2.0), P), f"{bad} moved {P} on the ratio path"
            assert close(compose_additive(P, E), P), f"{bad} moved {P} on the additive path"
        o, v, P = [0.2, 0.3, 0.4], [0.21, 0.3, 0.4], [0.25, 0.35, 0.45]
        assert smooth([0.2, bad, 0.4], v, P, 0.8, 10 / 255) == v, "a bad pixel takes the warped one"
        assert smooth(o, [bad, 0.3, 0.4], P, 0.8, 10 / 255) == o, "a bad history keeps the network's"
        assert smooth([bad] * 3, [0.3, bad, 0.3], P, 0.8, 10 / 255) == P, "else no correction, not 0"
    for E in EDITS:
        assert residual_guard(E) == E, "a finite correction must pass untouched"
    assert residual_guard([65504.0 - -65504.0, 0.0, 0.0]) == [65504.0, 0.0, 0.0], \
        "a finite difference past the fp16 range must not be stored as inf"
    o, v = [0.2, 0.3, 0.4], [0.21, 0.3, 0.4]
    assert close(smooth(o, v, o, 0.8, 10 / 255), [0.2 + 0.8 * (1 - 0.01 * 25.5) * 0.01, 0.3, 0.4]), \
        "a finite blend must be the plain smooth"
    # The guard statements themselves, in the order they run: a check that only looked for the
    # constant still passed with a guard deleted or aimed at the wrong variable.
    bad_fn = "bool bad(float3 c) { return any((asuint(c) & 0x7fffffff) >= 0x7f800000); }"
    for path, name, guard in (
            ("core/shaders/input.h", "kResidualShader",
             ["if (any((asuint(e) & 0x7fffffff) >= 0x7f800000)) e = 0;",
              "dst[p.xy] = float4(clamp(e, -65504.0, 65504.0), 0);"]),
            ("core/temporal/smooth.inc", "kSmoothShader",
             ["float3 r = lerp(o.rgb, v, strength * saturate(1.0 - d / threshold));",
              "if (bad(r)) r = !bad(o.rgb) ? o.rgb : !bad(v) ? v : input[p.xy].rgb;",
              "dst[p.xy] = float4(r, o.a);"])):
        body = re.search(name + r'\[\] = R"\((.*?)\)";', (ROOT / path).read_text(encoding="utf-8"), re.S)
        assert body and re.search(r"\s*".join(map(re.escape, guard)), body.group(1)), \
            f"{name} lost its NaN/inf guard, or it no longer guards what it stores"
        assert name != "kSmoothShader" or bad_fn in body.group(1), "kSmoothShader's bad() changed"

    print("compose: zero edit is a no-op, colour 0 holds hue, the guard bounds luminance,")
    print("         nothing leaves the cube, the additive hue rotation is gone, the residual")
    print("         limit scales the correction instead of clamping a channel, and edge fade")
    print("         reaches zero at the border, bites hardest in a corner, and holds hue. A NaN or")
    print("         inf from the network is no correction and never enters the smoothed history.")


if __name__ == "__main__":
    main()
