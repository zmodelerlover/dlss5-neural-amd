"""The Model B / Model C arithmetic, mirrored from the compose shader and checked.

Two things can go wrong here and neither shows up as a broken picture -- they show up as a picture
that is subtly not what NVIDIA draws, which is worse, because it looks fine.

  1. The coefficients. They are transcribed from the descriptor table in nvngx_dlssnr.dll and a
     typo in a sign or a digit is invisible.
  2. The saturation step. The kernel does a full RGB->HSL->RGB round trip; the shader uses the
     closed form of it. That is a derivation, not a transcription, so it is checked here against
     an actual round trip rather than asserted.

     HSL, not HSV. This check used to run against colorsys.rgb_to_hsv and passed, because the
     shader had been written from the same wrong reading: it proved the closed form matched its
     own premise and said nothing about the kernel. The SASS settles it -- L = (max+min)*0.5 at
     3ca0/3cb0, S over (max+min) or (2-max-min) under a test on L at 3cd0-3db0, and the rebuild
     through q = L<0.5 ? L(1+S) : L+S-L*S with p = 2L-q at 3fa0-4080. The two differ on bright
     saturated colours, which are the ones Models B and C exist to touch.

    python tools/style_check.py
"""

import colorsys


def coefficients(style, strength=1.0):
    """Mirror of StyleCoefficients in neural.cpp. Neutral for all three slots is 0."""
    t = min(max(strength, 0.0), 1.0)
    if style == 1:
        return (-0.10 * t, -0.25 * t, -0.10 * t)
    if style == 2:
        return (0.0, 0.0, -0.15 * t)
    return (0.0, 0.0, 0.0)


def sat(x):
    return min(max(x, 0.0), 1.0)


def style_rgb(c, gexp, gcon, gsat):
    """Mirror of NeuralStyle() in kComposeShader."""
    c = [sat(v * (2.0 ** gexp)) for v in c]
    c = [sat(v + gcon * (v * v * (3.0 - 2.0 * v) - v)) for v in c]
    hi, lo = max(c), min(c)
    d = hi - lo
    if d > 1e-6:
        light = (hi + lo) * 0.5
        s = d / ((2.0 - hi - lo) if light > 0.5 else (hi + lo))
        f = sat(s * (1.0 + gsat)) / s
        c = [light + f * (v - light) for v in c]
    return [sat(v) for v in c]


def saturation_roundtrip(c, gsat):
    """What the kernel literally does: to HSL, scale S, back to RGB.

    colorsys spells it HLS; the middle component is the lightness.
    """
    h, light, s = colorsys.rgb_to_hls(*c)
    return list(colorsys.hls_to_rgb(h, light, sat(s * (1.0 + gsat))))


def demo():
    # --- the coefficients -------------------------------------------------------------------
    # Model A is the neutral vector: selecting it must change nothing at all.
    assert coefficients(0) == (0.0, 0.0, 0.0)
    # Model B, from record+108, mask 0x34 -> slots 75, 77, 78.
    assert coefficients(1) == (-0.10, -0.25, -0.10)
    # Model C, from record+176, mask 0x20 -> slot 78 only. Exposure and contrast stay neutral,
    # and getting this wrong would make C a second B rather than its own look.
    assert coefficients(2) == (0.0, 0.0, -0.15)
    # LocalToneStrength scales towards neutral, and neutral here is zero.
    assert coefficients(1, 0.5) == (-0.05, -0.125, -0.05)
    assert coefficients(1, 0.0) == (0.0, 0.0, 0.0)
    # Out-of-range strength is clamped, not extrapolated.
    assert coefficients(2, 4.0) == coefficients(2, 1.0)
    assert coefficients(2, -1.0) == (0.0, 0.0, 0.0)

    # --- identity ---------------------------------------------------------------------------
    # Every operation must be its own identity at a zero coefficient, or Style=0 would not be
    # the picture every release so far has drawn.
    for c in ([0.0, 0.0, 0.0], [1.0, 1.0, 1.0], [0.2, 0.5, 0.9], [0.5, 0.5, 0.5]):
        out = style_rgb(c, 0.0, 0.0, 0.0)
        assert max(abs(a - b) for a, b in zip(out, c)) < 1e-9, (c, out)

    # --- the derivation -----------------------------------------------------------------------
    # The closed form has to agree with a real HSL round trip, on colours of every shape: grey
    # (d = 0, the degenerate case the shader guards), fully saturated, ordinary, and on both
    # sides of the L = 0.5 branch, since the two halves use different denominators.
    for c in ([0.2, 0.5, 0.9], [0.9, 0.1, 0.4], [0.5, 0.5, 0.5], [1.0, 0.0, 0.0],
              [0.0, 0.0, 0.0], [0.3, 0.3, 0.7], [0.05, 0.9, 0.35],
              # L well above the branch, L well below it, and L exactly on it.
              [1.0, 0.9, 0.8], [0.12, 0.04, 0.02], [0.9, 0.5, 0.1]):
        for k in (-0.15, -0.10, -0.5, 0.0, 0.25):
            mine = style_rgb(c, 0.0, 0.0, k)
            theirs = [sat(v) for v in saturation_roundtrip(c, k)]
            assert max(abs(a - b) for a, b in zip(mine, theirs)) < 1e-6, (c, k, mine, theirs)

    # A coefficient above zero can push S past 1, and there it has to stop rather than carry on
    # pulling the channels apart until they leave the cube. S = 1 puts the ends exactly on 0 and
    # 1, so this is the boundary case and not a clamp hiding an overshoot.
    out = style_rgb([0.3, 0.5, 0.7], 0.0, 0.0, 10.0)
    assert -1e-9 <= min(out) < 1e-6 and 1.0 - 1e-6 < max(out) <= 1.0 + 1e-9, out
    assert max(abs(a - b) for a, b in zip(out, saturation_roundtrip([0.3, 0.5, 0.7], 10.0))) < 1e-6
    # A colour already at S = 1 in HSL has nowhere to go, and must come back untouched. Under
    # HSV the same colour still had room, which is one of the places the two readings part.
    already = [0.4, 0.5, 1.0]
    assert max(abs(a - b) for a, b in zip(style_rgb(already, 0.0, 0.0, 10.0), already)) < 1e-6

    # --- direction of each knob ---------------------------------------------------------------
    gexp, gcon, gsat = coefficients(1)
    # B darkens: exp2(-0.10) is about 0.933, so a mid grey comes back lower.
    assert abs(2.0 ** gexp - 0.93303) < 1e-4, 2.0 ** gexp
    assert style_rgb([0.5, 0.5, 0.5], gexp, 0.0, 0.0)[0] < 0.5
    # B's contrast coefficient is negative, which blends *away* from the S-curve. Below the
    # midpoint smoothstep pulls down, so blending away must push that sample up.
    assert style_rgb([0.25, 0.25, 0.25], 0.0, gcon, 0.0)[0] > 0.25
    assert style_rgb([0.75, 0.75, 0.75], 0.0, gcon, 0.0)[0] < 0.75
    # ...and it is a real flattening: the gap between a dark and a light sample narrows.
    lo, hi = style_rgb([0.25] * 3, 0.0, gcon, 0.0)[0], style_rgb([0.75] * 3, 0.0, gcon, 0.0)[0]
    assert (hi - lo) < 0.5, (lo, hi)
    # B and C both desaturate, C more than B.
    base = [0.8, 0.2, 0.3]
    sb = style_rgb(base, 0.0, 0.0, coefficients(1)[2])
    sc = style_rgb(base, 0.0, 0.0, coefficients(2)[2])
    spread = lambda v: max(v) - min(v)
    assert spread(sc) < spread(sb) < spread(base), (spread(base), spread(sb), spread(sc))

    # Grey has no saturation to remove, so neither model may tint it.
    for style in (1, 2):
        _, _, k = coefficients(style)
        out = style_rgb([0.5, 0.5, 0.5], 0.0, 0.0, k)
        assert max(out) - min(out) < 1e-9, (style, out)

    print("style_check: the B/C coefficients match the descriptor table, every knob is the "
          "identity at neutral,")
    print("             and the closed-form saturation agrees with a real HSL round trip.")


if __name__ == "__main__":
    demo()
