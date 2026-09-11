"""Convert Ctrl+PageDown diagnostic captures to PNG and a local A/B viewer.

    python tools/capture_pair.py ".../dlss5-captures/frame-123-456.txt"

The input and runtime textures are from the same recorded evaluation. The
runtime image already includes its internal residual application. It is not
the neural tensor, nor the add-on's final guarded composition. Display uses a
fixed [0, 1] range, without exposure normalization or sharpening.
"""
import argparse
import html
import json
from pathlib import Path

import numpy as np
from PIL import Image


def convert(metadata: Path):
    fields = dict(line.split("=", 1) for line in metadata.read_text().splitlines() if "=" in line)
    if fields.get("format") != "RGBA16F_LE":
        raise ValueError("Unsupported capture format")
    if fields.get("encoding") != "0":
        raise ValueError("This viewer expects SDR/sRGB captures (Encoding=0)")
    w, h = int(fields["width"]), int(fields["height"])
    if not (0 < w <= 16384 and 0 < h <= 16384):
        raise ValueError("Invalid dimensions")
    arrays = {}
    for kind in ("input", "runtime"):
        raw = metadata.with_name(metadata.stem + "-" + kind + ".raw")
        if raw.stat().st_size != w * h * 8:
            raise ValueError(f"Unexpected capture length: {raw}")
        a = np.fromfile(raw, dtype="<f2").reshape(h, w, 4)[..., :3].astype(np.float32)
        if not np.isfinite(a).all():
            raise ValueError(f"Non-finite pixels: {raw}")
        arrays[kind] = a
        Image.fromarray(np.rint(np.clip(a, 0, 1) * 255).astype(np.uint8)).save(raw.with_suffix(".png"))
    before, after = arrays["input"], arrays["runtime"]
    metrics = {
        "width": w, "height": h, "metadata": fields,
        "mean_absolute_change": float(np.abs(after - before).mean()),
        "max_absolute_change": float(np.abs(after - before).max()),
        "runtime_min": float(after.min()), "runtime_max": float(after.max()),
        "pixels_above_1_percent": float((after.max(axis=2) > 1).mean() * 100),
        "pixels_below_0_percent": float((after.min(axis=2) < 0).mean() * 100),
    }
    metadata.with_suffix(".json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    base = html.escape(metadata.stem, quote=True)
    page = """<!doctype html><html lang="pt-BR"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Comparação do runtime neural</title><style>
body{margin:24px;background:#17191d;color:#eef0f3;font:16px system-ui}p{max-width:1000px;line-height:1.5}
.viewer{position:relative;width:100%;max-width:WIDTHpx;overflow:hidden;border:1px solid #707780}
img{display:block;width:100%;height:auto}.after{position:absolute;inset:0;clip-path:inset(0 50% 0 0)}
input{width:min(100%,900px);margin:16px 0}label{display:block}button{padding:8px;margin:8px}
.native{width:WIDTHpx;max-width:none}.scroll{overflow:auto}a{color:#a5cdff}
</style><h1>Entrada / saída do runtime</h1>
<p>Mesma captura, WIDTH × HEIGHT, PASSES passe(s). Arraste: saída do runtime à esquerda, entrada à direita.
O runtime já aplicou sua própria correção. Esta imagem não inclui a composição final do add-on.</p>
<label for="split">Divisão da comparação</label><input id="split" type="range" min="0" max="100" value="50">
<button id="zoom">Alternar tamanho real (1:1)</button><div class="scroll"><div class="viewer" id="viewer">
<img src="BASE-input.png" alt="Entrada SDR"><img class="after" id="after" src="BASE-runtime.png" alt="Saída do runtime">
</div></div><p>Sem ajuste automático de exposição ou nitidez. Valores fora de 0–1 são limitados apenas
para exibição; os arquivos RAW preservam os valores originais. Mudança de pixels não prova melhora de textura.</p>
<p><a href="BASE-input.png">Entrada PNG</a> · <a href="BASE-runtime.png">Runtime PNG</a> ·
<a href="BASE.json">Métricas e parâmetros</a></p><script>
document.getElementById('split').oninput=e=>document.getElementById('after').style.clipPath=`inset(0 ${100-e.target.value}% 0 0)`;
document.getElementById('zoom').onclick=()=>document.getElementById('viewer').classList.toggle('native');
</script></html>"""
    page = page.replace("WIDTH", str(w)).replace("HEIGHT", str(h))
    page = page.replace("PASSES", html.escape(fields.get("passes", "?"))).replace("BASE", base)
    target = metadata.with_suffix(".html")
    target.write_text(page, encoding="utf-8")
    print(target)
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", type=Path, nargs="+")
    for path in parser.parse_args().metadata:
        convert(path)
