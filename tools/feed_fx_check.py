"""Compile the companion effect's pixel shaders, so a typo in it is not found in a game.

shaders/AMD_Neural_Feed.fx is ReShade FX, not HLSL: build.ps1 never looks at it and fxc
cannot read it, so until now the only thing that compiled it was ReShade, on the user's
machine, after an install. This rewrites the ReShade-specific syntax into plain Shader Model 5
-- samplers become a Texture2D plus a SamplerState, tex2Dlod becomes SampleLevel, uniforms
become constants at their declared defaults -- and runs fxc over each pixel shader.

What it proves: the maths compiles, every identifier resolves, every sampler is declared
before it is used, and each entry point writes the number of targets its pass binds. What it
does not prove: that ReShade's own parser accepts the parts thrown away here (annotations,
texture formats, the technique block).

    python tools/feed_fx_check.py
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FX = ROOT / "shaders" / "AMD_Neural_Feed.fx"

# Entry point -> how many render targets its pass binds. Checked against the signature, because
# a pass that binds three targets and a shader that writes two is a silent half-written texture.
ENTRIES = {"PS_Guides": 3, "PS_History": 4}

PRELUDE = """
#define __RENDERER__ 0xB000
#define BUFFER_WIDTH 1920
#define BUFFER_HEIGHT 1080
#define BUFFER_PIXEL_SIZE float2(1.0/1920.0, 1.0/1080.0)
#define BUFFER_SCREEN_SIZE float2(1920.0, 1080.0)
Texture2D __t_ReShade_BackBuffer; SamplerState __s_ReShade_BackBuffer;
Texture2D __t_ReShade_DepthBuffer; SamplerState __s_ReShade_DepthBuffer;
float ReShade_GetLinearizedDepth(float2 uv)
{ return __t_ReShade_DepthBuffer.SampleLevel(__s_ReShade_DepthBuffer, uv, 0).x; }
"""


def find_fxc():
    bases = [Path(r"C:\Program Files (x86)\Windows Kits\10\bin"),
             Path(r"D:\Windows Kits\10\bin")]
    found = []
    for base in bases:
        if base.is_dir():
            found += [p for p in base.rglob("fxc.exe") if "x64" in str(p)]
    if not found:
        sys.exit("fxc.exe not found. Install the Windows 10/11 SDK.")
    return sorted(found)[-1]


def rewrite_calls(text, name, build):
    """Replace every `name(sampler, coord)` with build(sampler, coord), brackets balanced."""
    out = []
    i = 0
    while True:
        at = text.find(name + "(", i)
        if at < 0:
            out.append(text[i:])
            return "".join(out)
        out.append(text[i:at])
        depth = 0
        j = at + len(name)
        for j in range(at + len(name), len(text)):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    break
        inner = text[at + len(name) + 1:j]
        sampler, _, coord = inner.partition(",")
        out.append(build(sampler.strip(), coord.strip()))
        i = j + 1


def translate(text):
    # ReShade's own header, replaced by the handful of things this file uses out of it.
    text = text.replace('#include "ReShade.fxh"', PRELUDE)
    text = text.replace("ReShade::GetLinearizedDepth", "ReShade_GetLinearizedDepth")
    text = text.replace("ReShade::BackBuffer", "ReShade_BackBuffer")

    # A namespace here only ever wraps the provider's texture declarations, which go away with
    # every other texture below. Nested ones (Deferred::IPC) need the inner braces gone first.
    for _ in range(3):
        text = re.sub(r"namespace\s+\w+\s*\{[^{}]*\}", "", text, flags=re.S)

    # uniform T NAME < ... > = VALUE;  ->  static const T NAME = VALUE;   (no default: dropped)
    text = re.sub(r"uniform\s+\w+\s+\w+\s*<[^>]*>\s*;", "", text, flags=re.S)
    text = re.sub(r"uniform\s+(\w+)\s+(\w+)\s*<[^>]*>\s*=\s*([^;]+);",
                  r"static const \1 \2 = \3;", text, flags=re.S)

    # sampler NAME { Texture = X; ... };  ->  the SM5 pair the rewritten tex2Dlod reads.
    text = re.sub(r"sampler\d?D?\s+(\w+)\s*\{[^}]*\}\s*;",
                  r"Texture2D __t_\1; SamplerState __s_\1;", text)
    # Textures themselves carry no information fxc needs once the samplers are pairs.
    text = re.sub(r"texture\d?D?\s+\w+\s*(<[^>]*>)?\s*\{[^}]*\}\s*;", "", text)

    # tex2Dlod(s, float4(uv, 0, 0))  ->  __t_s.SampleLevel(__s_s, (...).xy, 0)
    # Scanned rather than matched: the coordinate is itself a call, so a non-greedy regex stops
    # at the inner closing bracket and hands fxc half an expression.
    text = rewrite_calls(text, "tex2Dlod",
                         lambda s, c: f"__t_{s}.SampleLevel(__s_{s}, ({c}).xy, 0)")

    # The technique block and the vertex shader ReShade supplies are not part of the maths.
    text = re.sub(r"technique\s+\w+\s*(<.*?>)?\s*\{.*?\n\}", "", text, flags=re.S)
    text = re.sub(r"^\s*AMDNR_REQUEST_PASS\s*$", "", text, flags=re.M)
    return text


def main():
    fxc = find_fxc()
    src = translate(FX.read_text(encoding="utf-8"))

    bad = 0
    for entry, targets in ENTRIES.items():
        signature = re.search(rf"void\s+{entry}\s*\((.*?)\)\s*\{{", src, re.S)
        if signature is None:
            print(f"{entry:<12} FAIL  no such entry point")
            bad += 1
            continue
        written = len(re.findall(r"SV_Target\d", signature.group(1)))
        if written != targets:
            print(f"{entry:<12} FAIL  writes {written} targets, its pass binds {targets}")
            bad += 1
            continue
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / f"{entry}.hlsl"
            path.write_text(src, encoding="utf-8")
            done = subprocess.run(
                [str(fxc), "/nologo", "/T", "ps_5_0", "/E", entry, "/O3", "/Fo", "NUL",
                 str(path)], capture_output=True, text=True)
        if done.returncode == 0:
            print(f"{entry:<12} OK    {targets} targets")
        else:
            print(f"{entry:<12} FAIL")
            print((done.stdout + done.stderr).strip())
            bad += 1

    # The provider's texture is declared here exactly as the provider declares it, which is the
    # whole mechanism: a name that drifts binds a fresh empty texture instead, and the field
    # reads as all-zero motion with nothing to say it went wrong.
    raw = FX.read_text(encoding="utf-8")
    for want in ("Deferred::MotionVectorsTex", "MotVectTexVort", "Kernel::tFlow",
                 "QuantMotion::tFlow", "texMotionVectors"):
        leaf = want.split("::")[-1]
        if not re.search(rf"texture\d?D?\s+{leaf}\b", raw):
            print(f"provider     FAIL  {want} is not declared")
            bad += 1

    if bad:
        sys.exit(f"{bad} problem(s) in {FX.name}")
    print(f"\n{FX.name}: every entry point compiles and every provider texture is declared.")


if __name__ == "__main__":
    main()
