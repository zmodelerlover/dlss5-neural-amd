"""The compose arithmetic, and the four things it has to be true about.

The composition lives in kComposeShader as HLSL and only ever runs on a GPU inside a game, so a
mistake in it is found by looking at a screen and disagreeing with it. This is the same arithmetic
written out once more, in the smallest form that can be asserted against -- if the two ever
disagree, this file is the one that is wrong, and it is here to fail when a change to the shader
breaks a property the shader was written to have.

    python tools/compose_check.py

Mirrors the `guard > 0` branch of kComposeShader exactly: CubeScale, the luminance floor, the
two-sided guard, the chroma blend and the hue-preserving peak scale.
"""

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

    print("compose: zero edit is a no-op, colour 0 holds hue, the guard bounds luminance,")
    print("         nothing leaves the cube, and the additive hue rotation is gone.")


if __name__ == "__main__":
    main()
