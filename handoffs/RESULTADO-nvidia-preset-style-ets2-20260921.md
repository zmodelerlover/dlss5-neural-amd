# Resultado — parâmetros que o RenoDX entrega à `nvngx_dlssnr.dll` (ETS2, NVIDIA)

Rodada de 21/09/2026, 22:41–23:37. Operador na máquina NVIDIA; execução do protocolo da §4 do
`HANDOFF-nvidia-preset-style-ets2-20260921.md`, mais análise estática da DLL.

---

## 0. Ambiente

| | |
|---|---|
| GPU / driver | NVIDIA GeForce RTX 3050 6GB Laptop, **610.62** (`32.0.16.1062`) |
| Jogo | Euro Truck Simulator 2 (Steam), `C:\Program Files (x86)\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64`, 1920×1080 |
| Rota | Jogo em **D3D11**; o RenoDX cria um **proxy D3D12** após o primeiro present e chama `NVSDK_NGX_D3D12_*`. Hook Method = **Present**, "presentation backbuffer with dummy temporal inputs" |
| ReShade | 6.8.0.2155 (`dxgi.dll`, 5 592 064 B) |
| Addon | `renodx-dlss.addon64`, 2 624 512 B, "RenoDX DLSS" v0.0.0.0, ReShade API 18 |
| NGX core | `_nvngx.dll` do DriverStore `nvlti.inf_amd64_7566d6b2a7331e4e`, force-loaded pelo addon |

Resíduos de outro pacote na mesma pasta (`nvngx.dll`, `nvngx.dll_dlssnr.dll`, `sl.dlss_nr.dll`,
`READ ME - DLSS Neural Rendering.txt`) pertencem a uma distribuição OptiScaler e **não** participaram
desta rota — o `ReShade.log` mostra só o `renodx-dlss.addon64` carregado.

---

## 1. Tabela de parâmetros `DLSSNR.*` por Model

Todos os valores abaixo saíram dos snapshots do `ngx_params.log`, tirados nos marcos `S1`, `S2`, `S3`.

| parâmetro | tipo | Model A | Model B | Model C |
|---|---|---|---|---|
| `DLSSNR.Style` | `unsigned int` | **0** | **1** | **2** |
| `DLSSNR.Intensity` | float | 1.000000 | 1.000000 | 1.000000 |
| `DLSSNR.LocalToneStrength` | float | 1.000000 | 1.000000 | 1.000000 |
| `DLSSNR.LocalStructureStrength` | float | 1.000000 | 1.000000 | 1.000000 |
| `DLSSNR.SkinStructureStrength` | float | 1.000000 | 1.000000 | 1.000000 |
| `DLSSNR.GlobalToneStrength` | float | 1.000000 | 1.000000 | 1.000000 |
| `DLSSNR.UseAutoMask` | int | 1 | 1 | 1 |
| `DLSSNR.DepthInverted` | int | 0 | 0 | 0 |
| `DLSSNR.Enabled` | int | 1 | 1 | 1 |
| `DLSSNR.UICorrection` | int | 1 | 1 | 1 |
| `DLSSNR.Reset` | int | 0 | 0 | 0 |
| `DLSSNR.MVecScaleX` / `Y` | float | 1.000000 / 1.000000 | idem | idem |
| `DLSSNR.Hint.Render.Preset` | uint | 1 | 1 | 1 |
| `DLSSNR.ScalingRatio` / `.Scale` | float | 1.000000 | idem | idem |
| `DLSSNR.Upscaling` | int | 0 | 0 | 0 |
| `DLSSNR.Width` / `.Height` | uint | 1920 / 1080 | idem | idem |
| `DLSSNR.InputWidth/Height`, `.OutputWidth/Height`, `.Output.Width/Height` | uint | 1920 / 1080 | idem | idem |
| subrects `Color/MVec/Depth/Output` Base X,Y / W,H | uint | 0,0 / 1920,1080 | idem | idem |
| `DLSSNR.JitterOffsetX` / `Y` | float | 0 / 0 | idem | idem |
| `CreationNodeMask` / `VisibilityNodeMask` | uint | 1 / 1 | idem | idem |
| `DLSS.Indicator.Invert.X/Y.Axis` | int | 0 / 0 | idem | idem |
| `DLSSNR.ControlMask`, `.Backbuffer`, `.BidirectionalDistortionField` | — | **nunca setados** | — | — |
| `DLSSNR.UI`, `.UIAlpha` | recurso | `0x0` (nulo) | idem | idem |

### A linha que interessa

> **Ao trocar o Model, o RenoDX escreve exatamente dois parâmetros: `DLSSNR.Style` e um pulso de
> `DLSSNR.Reset` (1 no frame da troca, 0 no frame seguinte). Nada mais muda.** Não há
> `CreateFeature`: a feature não é reconstruída, só o history é zerado.

Sequência-tipo, idêntica nas **três** repetições de A→B→C→A (22:51:50–22:52:58, 22:55:18–22:57:13,
23:31:08–23:31:27):

```
22:55:18.465  I     DLSSNR.Reset = 1
22:55:18.465  UI    DLSSNR.Style = 1
22:55:18.467  DLSSNR: reset temporal history for CC_Control_History_...  after control change
22:55:18.566  I     DLSSNR.Reset = 0
```

Detalhe: ao entrar em `Style = 1` pela primeira vez a DLL cria a textura
`dlssnr_original_color` (RGBA16F 1920×1080); ao voltar a `Style = 0` ela a libera. Essa textura é
o que o passe de grading usa — style 0 não tem grading.

---

## 2. Defaults do RenoDX

Medidos com a seção `[RENODX-DLSS*]` do `ReShade.ini` removida, de modo que o P0 capturou o que o
addon escolhe sozinho. Confirmados duas vezes: pelo trace e pelo ini que o addon reescreveu ao sair
(`ReShade.ini.AFTER`).

| controle no painel | chave no ini | parâmetro NGX | default |
|---|---|---|---|
| *(Neural Rendering ligado?)* | — | — | **desligado** |
| Model | `DirectNeuralRenderingStyle` | `DLSSNR.Style` | **0** (Model A) |
| Overall Intensity | `DirectNeuralRenderingIntensity` | `DLSSNR.Intensity` | **1.0** |
| Local Tone Intensity | `DirectNeuralRenderingLocalToneStrength` | `DLSSNR.LocalToneStrength` | **1.0** |
| Structure Intensity | `DirectNeuralRenderingLocalStructureStrength` | `DLSSNR.LocalStructureStrength` | **1.0** |
| Skin Structure Strength | `DirectNeuralRenderingSkinStructureStrength` | `DLSSNR.SkinStructureStrength` | **1.0** |
| Global Tone Intensity | `DirectNeuralRenderingGlobalToneStrength` | `DLSSNR.GlobalToneStrength` | **1.0** |
| Character Mask | `DirectNeuralRenderingAutoMask` | `DLSSNR.UseAutoMask` | **1** (On) |
| Pass Count | `DirectNeuralRenderingPassCount` | *(nenhum)* | **1** |
| Hook Method | `DirectNeuralRenderingHookPoint` | — | **4** (Present) |
| UI Correction | `DirectNeuralRenderingUiCorrectionMode` | `DLSSNR.UICorrection` | **0** (Auto) → envia **1** |
| Require DLSS | `DirectNeuralRenderingRequireDlss` | — | **0** (Off) |
| Encoding | `DirectNeuralRenderingEncoding` | — | **0** (Auto) |
| Diffuse White (nits) | `DirectNeuralRenderingDiffuseWhiteNits` | — | **100** |

E os que o RenoDX calcula, não expõe:

| parâmetro | default enviado |
|---|---|
| `DLSSNR.MVecScaleX` / `Y` | **1.0 / 1.0** |
| `DLSSNR.DepthInverted` | **0** — o default *interno da DLL* é 1; o RenoDX manda 0 explicitamente |
| `DLSSNR.Reset` | 0, pulsado a 1 em **qualquer** mudança de opção |
| `DLSSNR.Hint.Render.Preset` | **1** |
| `DLSSNR.ScalingRatio` / `.Scale` | 1.0 (a DLL força 1.0 de qualquer jeito) |

**Parâmetros que o RenoDX envia e esta DLL nunca lê:** `GlobalToneStrength`, `InputWidth/Height`,
`OutputWidth/Height`, `Output.Width/Height`, `Scale`, `Upscaling`, `JitterOffsetX/Y`. A lista de
nomes que a DLL realmente consulta está em `sub_180019F30` (60 strings `DLSSNR.*` em `.rdata`).

---

## 3. Log do NGX

O log **não** aparece pelo caminho da §3.4 sozinho. Dois arquivos:

- `<jogo>\bin\win_x64\nvngx_dlssnr_310_8_0.log` — o log do *snippet*, com todas as linhas `DLSSNR:`;
- `%LOCALAPPDATA%\Temp\RenoDX\dlssnr\nvngx.log` — o log do *core*, que só recebeu os erros do loader.

As linhas ricas também saem por `OutputDebugString`, e é assim que o `ngx_param_trace2.js` as
intercala com o trace de parâmetros no mesmo arquivo, já em ordem cronológica.

| item | valor |
|---|---|
| `config(s) available` | **2 ocorrências, ambas "`DLSSNR: 1 config(s) available:`"** — uma por `CreateFeature` |
| entrada `[0]` | `CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights` (backbone: **crazy-cuckoo**) |
| nome após `->` em `Created feature` | `CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights`, em ambas: `Created feature 1 (output 1920x1080, network 1920x1080, preset=1 -> …)` e `Created feature 2 (…)` |
| linha de fallback de preset | **nunca apareceu** (`preset=1` é o único disponível) |
| `reset temporal history` | **27 ocorrências, todas com motivo `control change`** (o outro motivo possível na format string é `config/control-state init`) |

### Em quais trocas apareceu `reset temporal history`

| mudança | reset? |
|---|---|
| `Style` 0→1, 1→2, 2→0 (3 ciclos completos) | **sim**, sempre |
| `LocalToneStrength` | **sim**, a cada valor intermediário do slider |
| `LocalStructureStrength` | **sim** |
| `SkinStructureStrength` | **sim** |
| `UseAutoMask` (Character Mask On/Off) | **sim** |
| `Intensity` | **não** |
| `GlobalToneStrength` | **não** |
| Pass Count | não — em vez disso **reconstrói a feature** (`CreateFeature` → `Created feature 2`) |

O `DLSSNR.Reset = 1` é pulsado pelo RenoDX em **todas** essas mudanças, inclusive nas que não
geram reset de history. As duas coisas são independentes.

---

## 4. A DLL

| | |
|---|---|
| Caminho | `<jogo>\bin\win_x64\nvngx_dlssnr.dll` |
| Tamanho | **165 840 496 bytes** |
| SHA256 | **`E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A`** |
| FileVersion / Product | 310.8.0.0 — "NVIDIA DLSSNR - DVS PRODUCTION" |
| Build interna | `v310.8.0 CL 38718415`, path `.../snippets/rel_310_8/source/features/dlssnr/...` |
| Assinatura | **falha** — `nvLoadSignedLibraryW() failed on snippet ... missing or corrupted` |

O hash difere do `e16bcf15…` do handoff, com **tamanho idêntico**. Explicação do operador: é uma
versão modificada para rodar em RTX anteriores à série 50 (esta máquina é Ampere). Não é corrupção
— é o patch de checagem de arquitetura, e é por isso que a verificação de assinatura falha e o
RenoDX carrega o snippet pelo caminho próprio.

**A tabela de styles é a mesma do handoff.** A entrada de 648 bytes em `0x1800B0D80` contém, ao
mesmo tempo, `+0x00 = "CC_Control_History_..."` (o nome que o log imprime), `+0x08 = 1` (o id),
`+0x10 = "WEIGHTS_HT"` e `+0x18 = "CC_SILVER_AARDWOLD"` (as tags que o handoff leu no IDA). Não são
DLLs diferentes; são campos diferentes da mesma entrada. A §5.3 do handoff fica resolvida.

---

## 5. Análise estática — o que a DLL faz com os parâmetros

Feita com `pehelp.py` (PE + `.pdata` + capstone) sobre a DLL instalada. Endereços em RVA.

### 5.1 `sub_180019F30` — leitura dos parâmetros e o layout de `opts`

Todos os offsets do §2.1 do handoff **confirmados**, mais dois que faltavam:

| offset | parâmetro | default se ausente |
|---|---|---|
| `+0xD8` / `+0xDC` | `MVecScaleX` / `MVecScaleY` | 1.0 |
| `+0xE0` | `Intensity` | 1.0 |
| `+0xE4` | `LocalToneStrength` | 1.0 |
| `+0xE8` | `LocalStructureStrength` | 1.0 |
| `+0xEC` | `Style` (int) | 0 |
| `+0xF0` | `UseAutoMask` (int) | 0 |
| `+0xF4` | `SkinStructureStrength` | **−1.0** |
| **`+0xF8`** | **`skinEff`** — derivado | — |
| **`+0xFC`** | **`structEff`** — derivado | — |
| `+0x100` | `Reset` (int) | 0 |
| `+0x104` | `DepthInverted` (int) | **1** |
| `+0x108` | `Enabled` (int) | 1 |
| `+0x10C` | `UICorrection` (int) | 0 |
| `+0x110` / `+0x114` | `DLSS.Indicator.Invert.X/Y.Axis` | 0 |
| `+0x120` | `ScalingRatio` | **forçado a 1.0 sempre**, ignorando o valor lido |

### 5.2 Derivação de `skinEff` / `structEff` — correção ao §2.1 do handoff

`0x1AA4B`–`0x1AAA1`:

```c
if (ControlMask presente)  UseAutoMask = 0;
if (UseAutoMask != 0) {
    skinEff   = (Skin >= 0.0f) ? Skin : LocalStructure;
    structEff = LocalStructure;
} else {
    skinEff = structEff = -1.0f;
}
```

O handoff dizia "se `UseAutoMask` e `Skin < 0`, `Skin = LocalStructure`". Não é isso: são **dois**
valores efetivos separados, e com AutoMask desligado **ambos** viram −1.0 — o caminho de estrutura
inteiro é desativado, não só o de pele.

### 5.3 `CG2R_ResetTemporalHistoryOnControlChange` (`0x179D0`)

Compara o estado anterior com `opts` e, se diferir, escreve `opts[+0x100] = 1` (o `Reset`) e loga.
Campos comparados:

- inteiros, igualdade exata: `+0xEC` (Style), `+0xF0` (UseAutoMask)
- floats, tolerância **`|Δ| > 1e-5`**: `+0xE4` (LocalTone), `+0xE8` (LocalStructure), `+0xF4` (Skin),
  `+0xF8` (skinEff), `+0xFC` (structEff)

`Intensity` (`+0xE0`) e `MVecScale` **não** entram — daí o S5 não gerar reset. Os dois derivados
não acrescentam gatilho novo (mudam junto com suas entradas), mas fazem parte da comparação.

### 5.4 `CG2RNetworkManager::Evaluate` (`0x21BB0`) — o style entra na rede

`0x224A9`–`0x22505`:

```
descritores: vetor [rdi+0x28] .. [rdi+0x30], elementos de 0xF0 = 240 bytes
n         = int em descritor[selecionado] + 0x64
style     = opts[+0xEC]
se n <= 0            -> style = 0
senão se style < n   -> style inalterado      (comparação UNSIGNED: jb)
senão                -> style = n - 1         (clamp, SEM log)
controle  = (float)style * 0.0078125          (= style / 128)
```

**`n = 3`, medido em runtime** (1 descritor, índice 0, contagem 3). Ou seja: a rede aceita styles
**0, 1, 2** — exatamente Model A/B/C. Não há style escondido. `style ≥ 3` vira 2 em silêncio, e
style negativo também vira 2 (a comparação é unsigned).

Valores de controle: **0 / 0.0078125 / 0.015625**. Isso valida a correção feita no addon AMD.

Logo em seguida o `forward` recebe, nesta ordem de registradores: `[+0xFC]` structEff,
`[+0xF8]` skinEff, `[+0xE8]` LocalStructure, `[+0xE4]` LocalTone, `[+0x104]` DepthInverted,
`[+0xD8]`/`[+0xDC]` MVecScale.

### 5.5 Os vetores de grading — em `.rdata`, **não** no kernel CUDA

Confirmado dos dois lados: estão no host e **não** estão no fatbin.

**Não estão no kernel.** `cg2r_post_process_kernel` vive em `carved_0112fd90.fatbin`, com quatro
arquiteturas (`sm_75`, `sm_86`, `sm_89`, `sm_120`). Desmontado com `cuobjdump -sass`, nas quatro:

| constante | encoding | ocorrências |
|---|---|---|
| −0.10 | `0xBDCCCCCD` | **0** |
| −0.15 | `0xBE19999A` | **0** |
| −0.25 | `0xBE800000` | 6 — mas em `FADD.FTZ R, R, -0.25`, três por bloco (um por canal), cercado de `0.333333`, `0.666667`, `0.166667`, `ln 2`, `log2 e`, `MUFU.EX2`/`MUFU.LG2`: é aproximação de raiz cúbica / `pow`, conversão de espaço de cor, não o knob de contraste |

O kernel tem **só** `.nv.constant0` (banco de parâmetros, `EIATTR_CBANK_PARAM_SIZE = 0x178` = 376 B)
e nenhum `.nv.constant2/3` — não há tabela de constantes compilada. O `cg2r_copy_kernel`
(`carved_0113d3c0.fatbin`) não tem nenhuma das três.

**Estão no host.** Tabela em RVA `0xB0D80` (VA `0x1800B0D80`), de `0xB0D80` a `0xB1008` =
`0x288` = **648 bytes: uma única entrada**, exatamente como o handoff descreve. Acessada por
`CG2RFindWeightByPreset` (`0x23A40`, o nome está na própria string de log) e por um lookup por
nome (`0x239A0`).

O caminhador dos styles é `0x1D7C0`:

```
lea rdx, [rdi + 0x28]        ; rdi = entrada
lea rax, [rdx + 0x3c]        ; primeiro bloco = entrada + 0x64
lea rcx, [rdx + 0x25c]       ; fim           = entrada + 0x284   -> 8 slots de bloco
cmp byte ptr [rax], 0        ; +0x00 = flag de valido
cmp dword ptr [rax + 4], ebp ; +0x04 = indice do style
add rax, 0x44                ; stride 68
lea rdx, [rax + 8]           ; payload
call 0x1d5f0
```

Bloco = 68 B: flag (4) + índice do style (4) + payload (60). Payload = máscara (4) + **14 floats**.
Dois blocos preenchidos, styles **1** e **2**; style 0 não tem bloco — é identidade.

`0x1D5F0` aplica o payload, e é aqui que o `LocalToneStrength` entra:

```c
float t = clamp(opts[+0xE4], 0.0f, 1.0f);        // LocalToneStrength
uint32_t mask = payload[0];
for (int i = 0; i < 14; i++)
    if (mask & (1u << i)) {
        float neutro = (i == 1) ? 1.0f : 0.0f;
        opts[0x124 + 4*i] = (payload_float[i] - neutro) * t + neutro;   // lerp
    }
```

A máscara é o que o dump mostrava como "52" e "32":

| style | máscara | bits | payload | valor | destino |
|---|---|---|---|---|---|
| 1 | 52 = `0b110100` | 2, 4, 5 | `+0x0C`, `+0x14`, `+0x18` | **−0.10**, **−0.25**, **−0.10** | `opts+0x12C`, `+0x134`, `+0x138` |
| 2 | 32 = `0b100000` | 5 | `+0x18` | **−0.15** | `opts+0x138` |

Ou seja: bit 2 = exposição, bit 4 = contraste, bit 5 = saturação. Style 2 **não aplica**
exposição nem contraste (bits desligados) em vez de aplicá-los com valor 0 — numericamente dá
no mesmo, porque o lerp parte de 0.

Isso reproduz **exatamente** a tabela do `style_compare.py`
(`STYLES = {1: (-0.10, -0.25, -0.10), 2: (0.0, 0.0, -0.15)}`), e confirma o "escalado por
`LocalToneStrength`" do handoff — com a precisão a mais de que o escalonamento é um **lerp a partir
do neutro**, não uma multiplicação. Para esses três coeficientes o neutro é 0, então `k*t`
(o que o `style_compare.py` faz com `--strength`) está correto.

A DLL suporta **14 coeficientes de grading**; esta build usa três. Os outros 11 slots existem e
estão zerados — espaço para styles que não foram enviados. O cabeçalho da entrada tem `+0x024 = 3`,
coerente com o `n = 3` medido em runtime.

### 5.6 Do host até o parâmetro do kernel

`0x1C9C5` monta a struct de lançamento (base `rbp`):

| origem | destino na struct | conteúdo |
|---|---|---|
| `opts+0xE0` | `+0xF0` | `Intensity` |
| `opts+0x124` (16 B) | `+0x10C` | coeficientes 0–3 |
| `opts+0x134` (16 B) | `+0x11C` | coeficientes 4–7 |
| `opts+0x144` (16 B) | `+0x12C` | coeficientes 8–11 |
| `opts+0x154` | `+0x13C` | coeficiente 12 |
| `opts+0x158` | `+0x140` | coeficiente 13 |

Os 14 coeficientes ficam contíguos em `stage+0x10C`…`+0x140`, então **coef *i* → `stage + 0x10C + 4i`**:
exposição (i=2) → `+0x114`, contraste (i=4) → `+0x11C`, saturação (i=5) → `+0x120`.

No kernel, os parâmetros começam em `c[0x0][0x160]` no sm_86 (`0x160 + 0x178 = 0x2D8`, que é
exatamente o tamanho da `.nv.constant0`). As operações de float consomem um bloco denso no fim —
`0x29C`, `0x2A4`–`0x2AC`, `0x2B4`–`0x2D0`, cada um 6 a 8 vezes — que é onde o grading é aplicado.
O SASS de sm_86 vai no zip como `cg2r_post_process_kernel.sm_86.sass`.

### 5.7 O operador de grading, revertido do SASS

**Feito.** Desmontagem do `cg2r_post_process_kernel` (sm_86). O veredito antecipado: o modelo do
`style_compare.py` está **correto**, verificado instrução por instrução.

#### Mapeamento coeficiente → parâmetro do kernel

Os 14 coeficientes ocupam `c[0x0][0x29C]`…`c[0x0][0x2D0]`, um dword cada (o bloco de parâmetros vai
de `0x160` a `0x2D8` no sm_86). Ancorado por duas evidências independentes que batem: o coeficiente
de índice 1 é o único com neutro **1.0** no host, e no kernel `c[0x2A0]` é exatamente o *hi* de uma
normalização; e o índice 2 é consumido por **`MUFU.EX2`**, que só faz sentido para exposição.
Depois disso, os papéis de 4 e 5 se confirmam sozinhos pela forma das contas.

| coef | param | papel | usado pelos styles? |
|---|---|---|---|
| 0 | `c[0x29C]` | nível de preto (*lo*), neutro 0 | não |
| 1 | `c[0x2A0]` | nível de branco (*hi*), **neutro 1.0** | não |
| **2** | `c[0x2A4]` | **exposição** | style 1 |
| 3 | `c[0x2A8]` | — | não |
| **4** | `c[0x2AC]` | **contraste** | style 1 |
| **5** | `c[0x2B0]` | **saturação** | styles 1 e 2 |
| 6–13 | `c[0x2B4]`…`c[0x2D0]` | outros operadores | não |

#### A cadeia

**0. Normalização** (`0x2830`), com coef 0 e 1:

```
inv = 1 / (hi - lo + 1e-10)          ; MUFU.RCP
c   = saturate((c - lo) * inv)
```

Com `lo = 0` e `hi = 1` (os neutros, que é o caso nos dois styles) isto é `saturate(c)`, identidade.

**1. Exposição** (`0x2DE0`), coef 2:

```
002DE0  MUFU.EX2  R3, c[0x0][0x2a4]     ; g = 2^k
002E00  FFMA.SAT  R5, R3, R5, RZ        ; c = saturate(c * g)
```

**2. Contraste** (`0x2E30`–`0x2ED0`), coef 4, por canal:

```
002E30  FADD  R4, R5, R5                ; 2c
002E40  FMUL  R0, R5, R5                ; c²
002E70  FADD  R4, -R4, 3                ; 3 - 2c
002EA0  FFMA  R0, R0, R4, -R5           ; c²(3-2c) - c
002ED0  FFMA  R0, R0, c[0x0][0x2ac], R5 ; c + k*(smoothstep(c) - c)
002F00  FADD.SAT R0, RZ, R0             ; saturate
```

`smoothstep(c) = c²(3 − 2c)`, exatamente como no `style_compare.py`.

**3. Saturação** (`0x3B30`–`0x3E90`), coef 5. RGB→HSL completo, mexe só em S, volta:

```
003B10  R13 = MAX(r,g,b)
003B20  R6  = MIN(r,g,b)
003B40  P0  = MAX > MIN                 ; sem croma -> cinza, pula
003B70  R0  = k + 1                     ; (1 + k)
003B80  R2  = (MAX + MIN) * 0.5         ; L
003BA0  P1  = L > 0.5
003BB0  R15 = MAX - MIN                 ; C
003C00  @P1  denom = 2 - MAX - MIN
003C40  @!P1 denom = MAX + MIN
003C70  S   = C / denom
003D30  FFMA.SAT R11, R0, R3, RZ        ; S' = saturate(S * (1 + k))
003D90  ...                             ; hue2rgb: H+1/3, H, H-1/3, wrap, piecewise em 1/6
```

O `denom = (L > 0.5) ? 2 - M - m : M + m` é idêntico ao do `style_compare.py`, inclusive o ramo.

#### O atalho do `style_compare.py` é exato, não aproximado

O script não reconverte por HSL: ele escala `(c − L)` pela razão `S'/S`. Isso é **matematicamente
idêntico** ao round-trip do kernel. Para `L < 0.5`, `q = L(1+S)` e `p = 2L − q = L(1−S)`, logo
`canal = p + (q−p)·w(H) = L + L·S·(2w−1)`; para `L ≥ 0.5`, `canal = L + S(1−L)(2w−1)`. Nos dois
casos `canal − L` é **linear em S** com `H` e `L` fixos, então multiplicar `S` por `r` multiplica
`(canal − L)` por `r`. O atalho não introduz erro.

#### Conclusão

```c
// operador de grading da DLSSNR 310.8, por canal, na ordem do kernel
c = saturate((c - lo) * (1.0f / (hi - lo + 1e-10f)));   // coef 0, 1  (identidade aqui)
c = saturate(c * exp2f(k_exposicao));                   // coef 2
c = saturate(c + k_contraste * (c*c*(3.0f - 2.0f*c) - c)); // coef 4
// coef 5: RGB->HSL, S' = saturate(S * (1 + k_saturacao)), H e L mantidos, HSL->RGB
```

com `k = (valor_da_tabela - neutro) * clamp(LocalToneStrength, 0, 1) + neutro` (§5.5).

**Nada disso precisa ser adivinhado do lado AMD.** O `style_compare.py` já implementa exatamente
esta cadeia; o que faltava era a prova, e ela está aqui.

#### Dois avisos

- Entre a normalização e a exposição o kernel roda mais dois operadores, keyed em `c[0x2B8]` e
  `c[0x2BC]` (coef 7 e 8), que calculam `L = (MAX+MIN)/2` e misturam com constantes fixas
  (`0.061110`, `0.105557`). Depois do contraste há um banco de **quatro bandas** de luminância com
  smoothstep (`saturate(c·4)`, `saturate((c−0.25)·±4)`, `saturate((c−0.5)·±4)`, `c−0.75`) — daí vêm
  os `−0.25/−0.5/−0.75` do SASS. **Todos são pulados** nesta build (`if |k| < 1e-6 → BRA`), porque
  os coeficientes correspondentes são 0. Se uma build futura os ligar, a cadeia acima fica
  incompleta.
- Quando o style não tem bloco (style 0), `0x1D5F0` é chamado com máscara 0 e não escreve nada nos
  14 campos. Isso **não** causa valor velho: a struct é reinicializada aos neutros a cada evaluate,
  antes de qualquer leitura de parâmetro — ver §5.8. Style 0 é identidade de verdade.

### 5.8 A ordem de execução, e a inicialização aos neutros

`func 0x00018620` é a função-mestra de evaluate. Ela chama, nesta ordem:

| endereço | chama | o quê |
|---|---|---|
| `0x186E4` | `0x19F30` | zera a struct aos neutros e lê todos os `DLSSNR.*` |
| `0x18E64` | `0x1D7C0` | acha o bloco do style e aplica o grading (via `0x1D5F0`) |
| `0x19811` | `0x21BB0` | `CG2RNetworkManager::Evaluate` — o forward da rede |

O prólogo de `0x19F30` (`0x19FBD`–`0x1A076`) escreve **toda** a struct antes de consultar o NGX,
usando escritas de 64 bits que cobrem dois campos de uma vez:

```
mov dword [rdi+0xD8], 0x3F800000   ; MVecScaleX = 1.0
mov dword [rdi+0xDC], 0x3F800000   ; MVecScaleY = 1.0
mov dword [rdi+0xE0], 0x3F800000   ; Intensity = 1.0
mov dword [rdi+0xE4], 0x3F800000   ; LocalToneStrength = 1.0
mov qword [rdi+0xE8], 0x3F800000   ; LocalStructure = 1.0   |  Style (+0xEC) = 0
mov dword [rdi+0xF0], esi          ; UseAutoMask = 0            (esi = 0)
mov dword [rdi+0xF4], 0xBF800000   ; SkinStructureStrength = -1.0
mov dword [rdi+0xF8], 0xBF800000   ; skinEff   = -1.0
mov dword [rdi+0xFC], 0xBF800000   ; structEff = -1.0
mov dword [rdi+0x100], esi         ; Reset = 0
mov dword [rdi+0x104], 1           ; DepthInverted = 1
mov qword [rdi+0x108], 1           ; Enabled = 1            |  UICorrection (+0x10C) = 0
mov qword [rdi+0x110], rsi         ; Indicator.Invert.X / .Y = 0
mov qword [rdi+0x118], rsi         ; +0x118, +0x11C = 0
mov qword [rdi+0x120], 0x3F800000  ; ScalingRatio = 1.0     |  coef 0  (+0x124) = 0
mov qword [rdi+0x128], 0x3F800000  ; coef 1 = 1.0           |  coef 2  (+0x12C) = 0
mov qword [rdi+0x130], rsi         ; coef 3, 4  = 0
mov qword [rdi+0x138], rsi         ; coef 5, 6  = 0
mov qword [rdi+0x140], rsi         ; coef 7, 8  = 0
mov qword [rdi+0x148], rsi         ; coef 9, 10 = 0
mov qword [rdi+0x150], rsi         ; coef 11, 12 = 0
mov dword [rdi+0x158], esi         ; coef 13 = 0
```

Os 14 coeficientes saem daí exatamente nos neutros que `0x1D5F0` usa como base do lerp: **0 para
todos, 1.0 só para o índice 1**. Isso vale a cada evaluate, então style 0 (sem bloco, máscara 0) é
identidade de verdade — não herda nada do style anterior.

De quebra, este prólogo é uma confirmação independente de toda a tabela de defaults da §5.1,
incluindo os dois que divergem do que o RenoDX manda: `DepthInverted = 1` e
`SkinStructureStrength = -1.0`.

### 5.9 O "exposure" do `forward` não existe — é o ponteiro do ControlMask

O §2.3 do handoff lista os argumentos como
`CCNetwork::forward(..., LocalTone, LocalStructure, style * 0.0078125, exposure, skinEff, structEff, ...)`.
A chamada real é `call 0x3F490` em `0x22636`, e os argumentos de pilha são montados em
`0x225A7`–`0x225CB`:

| slot | conteúdo real | o handoff chamava |
|---|---|---|
| `[rsp+0x60]` | `opts+0xE4` — LocalToneStrength | LocalTone ✓ |
| `[rsp+0x68]` | `opts+0xE8` — LocalStructureStrength | LocalStructure ✓ |
| `[rsp+0x70]` | `style/128` | style·0.0078125 ✓ |
| `[rsp+0x78]` | **`r9` — 64 bits, não float** | *exposure* ✗ |
| `[rsp+0x80]` | `opts+0xF8` — skinEff | skinEff ✓ |
| `[rsp+0x88]` | `opts+0xFC` — structEff | structEff ✓ |

Cinco dos seis conferem, e as posições batem, então o quarto slot é sem dúvida o que o handoff
rotulou de "exposure". Mas ele é um `mov qword`, não um `movss`. Rastreando `r9` para trás: a última
escrita antes da chamada é `0x224A1  mov r9, qword ptr [rsp+0xb8]`, sem nenhuma `call` entre as duas
(as chamadas da função pulam de `0x223DD` direto para `0x22636`), então o valor chega intacto. E
`[rsp+0xb8]` é preenchido em `0x22328`–`0x22384`:

```
mov  r9, rsi                  ; rsi = 0
mov  qword [rsp+0xb8], rsi    ; default: 0
mov  rdx, qword [r14+0x60]    ; o recurso DLSSNR.ControlMask
test rdx, rdx
je   0x224a9                  ; ausente -> o slot permanece 0
mov  r9d, 2                   ; modo de acesso
lea  r8,  [rsp+0xb8]          ; out: ponteiro de device
call [vtbl+0xa8]              ; API CUBIN de mapeamento de recurso
                              ; falha -> "CUBIN API failed %d", cg2r_network_manager.cpp:1017
```

`opts+0x60` é onde `sub_19F30` guarda `DLSSNR.ControlMask` (`0x1A3F0: lea r8, [rdi+0x60]`) — o mesmo
campo que a §5.2 testa para forçar `UseAutoMask = 0`.

**Conclusão: não há auto-exposure em lugar nenhum.** O quarto argumento é o ponteiro de device da
máscara de controle, ou `0` quando não há máscara. Não existe nenhuma string com "exposure" na DLL
inteira, e nenhum `DLSSNR.*` desse tipo.

**Para a AMD isto evita um trabalho inútil:** não é preciso calcular métrica de exposição nenhuma.
E como o RenoDX **nunca** escreve `DLSSNR.ControlMask` (confirmado no trace — o parâmetro está
ausente de todos os snapshots), neste caminho de referência o argumento é sempre nulo. ControlMask e
AutoMask são alternativas: ou a aplicação fornece a máscara explícita, ou a rede deriva a dela.

---

## 6. O que a rota AMD precisa mudar

1. **Ao trocar de Model, mexer só no style.** Nada mais. O RenoDX não toca em tone/structure/skin/
   intensity/preset junto.
2. **Zerar o history em sete condições, não uma.** Hoje a AMD zera só no Style. A DLL zera em
   `Style`, `LocalTone`, `LocalStructure`, `Skin`, `UseAutoMask` e nos derivados `skinEff`/`structEff`
   — e **não** em `Intensity`. Usar tolerância `1e-5` nos floats.
3. **Corrigir a derivação de skin/structure** conforme §5.2: com AutoMask desligado, `structEff`
   também vai a −1.0, não só `skinEff`.
4. **Defaults**: tone 1.0, structure 1.0, skin 1.0, AutoMask 1, intensity 1.0, MVecScale 1.0/1.0,
   DepthInverted 0. A AMD hoje usa tone 0 e structure 1 — o tone está errado.
   (Atenção: o default *interno* da DLL para Skin é −1.0 e para DepthInverted é 1; quem manda
   1.0 e 0 é o RenoDX.)
5. **`style/128` com clamp a `n−1 = 2`**, unsigned. Já corrigido no addon; confirmado aqui.
6. **Grading**: style 1 = (−0.10 EV, −0.25 contraste, −0.10 saturação), style 2 = (0, 0, −0.15).
   Confirmado no host (`.rdata`) e confirmado **ausente** dos kernels CUDA nas quatro arquiteturas.
   O escalonamento por `LocalToneStrength` é um **lerp a partir do neutro**, com `t` clampeado em
   [0,1]: `coef = (valor - neutro)*t + neutro`. Para exposição/contraste/saturação o neutro é 0, de
   modo que `k*t` é equivalente — mas se a AMD for usar algum dos outros 11 coeficientes, o de
   índice 1 tem neutro **1.0**, não 0.
7. **`GlobalToneStrength` não existe nesta DLL.** Se o addon AMD expuser o controle, ele não tem
   efeito nenhum nesta rota — é campo do ABI Streamline.

### 6.1 Mapeamento para os campos do runtime AMD

O §2.4 do handoff descreve o vetor de controle do `dlssnr_amd_pass1.dll` (`sub_180018670`):
`0x97B30` tone, `0x97B34` structure, `0x97B38` skin, e o quinto valor em `0x96FA8` vindo de
`0x97B3C` (o antigo `EngineScale`, hoje `style/128`).

Do lado NVIDIA, `CG2RNetworkManager::Evaluate` carrega, em `0x22548`–`0x22563`, **quatro** valores
para o `forward`, nesta ordem de registrador:

| registrador | offset | valor |
|---|---|---|
| `xmm5` | `opts+0xFC` | **structEff** (derivado) |
| `xmm7` | `opts+0xF8` | **skinEff** (derivado) |
| `xmm8` | `opts+0xE8` | LocalStructureStrength (cru) |
| `xmm9` | `opts+0xE4` | LocalToneStrength |

Mais `opts+0x104` (DepthInverted) e `opts+0xD8/+0xDC` (MVecScale).

Ou seja: a NVIDIA entrega à rede **LocalStructure cru *e* structEff derivado, além de skinEff**. O
vetor AMD tem três slots para isso (tone, structure, skin). Correspondência direta:

| campo AMD | deve receber | hoje recebe |
|---|---|---|
| `0x97B30` tone | `LocalToneStrength`, default **1.0** | tone 0 — **errado** |
| `0x97B34` structure | `LocalStructureStrength` cru, default 1.0 | 1 — ok |
| `0x97B38` skin | **`skinEff`** (derivado, §5.2), não o Skin cru | skin −1 — ok só quando AutoMask off |
| `0x97B3C` | `style/128` | corrigido em 22:24 — ok |

**Não há slot AMD para o `structEff`.** Ele é um quarto valor que a rede NVIDIA recebe e que o
runtime AMD, pela leitura do handoff, não tem onde colocar. Isso precisa ser verificado do lado
AMD antes de assumir paridade: ou o `structEff` está implícito noutro campo, ou está faltando.

### 6.2 Multi-pass

`Pass Count` de 1 a 10. Com N > 1 a DLL **reconstrói a feature** e passam a sair N pares
`Color`/`Output` alternados por frame: o passe *k* consome a saída do passe *k−1* como `Color`, com
os buffers privados alternando. Observado com N=2: `Color=0x…4a8070 Output=0x…dc734c0` seguido de
`Color=0x…4ec8e0 Output=0x…4a8070`. Preparação da fonte e composição do resultado rodam **uma vez
só**, não por passe.

---

## 7. Ressalvas

- **Sem screenshots.** O passo F era opcional e ficou por último; nada de `style_compare.py` nesta
  rodada. As constantes de grading foram confirmadas direto no binário, que é evidência mais forte
  que a foto teria sido.
- **A rota é "Present" com temporal inputs falsos.** ETS2 não tem DLSS, então o RenoDX alimenta o
  modelo com o backbuffer de apresentação e MVec/Depth artificiais (`dummy temporal inputs` no
  próprio painel). Os *parâmetros* medidos valem; o comportamento temporal do modelo neste jogo
  não é representativo de um título com DLSS real.
- **A DLL é a patcheada para RTX pré-50.** O patch é de checagem de arquitetura; a tabela de styles
  e o código de `Evaluate` conferem com a leitura do handoff, mas nada garante que o patch não
  tenha tocado em outra coisa.
- **`ngx_param_trace.js` original não funciona** e não foi usado: `_nvngx.dll` não exporta
  `NVSDK_NGX_Parameter_Set*` (são helpers inline do SDK sobre métodos virtuais), e o caminho de
  float lia `this.context.xmm2`, que o Frida não expõe em x64. O substituto está em
  `ngx_param_trace2.js`.
- **Layout da vtable**: esta build usa a ordem de overloads **invertida** do MSVC —
  Set: `void*`, `ID3D12Resource*`, `ID3D11Resource*`(stub), `int`, `uint`, `double`, `float`, `u64`;
  Get idem, com `Get(ID3D11Resource**)` retornando `0xBAD00010`. Quem for repetir isto noutra build
  precisa reconferir (o script imprime uma calibração por desmontagem no começo do log).

---

## 8. Arquivos

| arquivo | o que é |
|---|---|
| `ngx_params.log` | trace de parâmetros + log do NGX intercalados, com marcos por passo |
| `nvngx_dlssnr_310_8_0.log` | log do snippet DLSSNR, da pasta do jogo |
| `nvngx.log` | log do core NGX (`Temp\RenoDX\dlssnr`), só erros de loader |
| `ReShade.log` | sessão completa, incluindo o shutdown limpo |
| `ReShade.ini.BEFORE` / `.AFTER` | ini antes da rodada e como o addon o reescreveu (defaults de fábrica) |
| `ReShadePreset.ini.BEFORE` | preset de shaders |
| `NGXCore-BEFORE.reg` / `NGXCore-ENABLE-LOG.reg` | estado original do registro e o que foi aplicado |
| `cmd.txt` | linha do tempo dos marcos enviados ao trace |
| `ngx_param_trace2.js`, `run_trace.py` | instrumento usado |
| `pehelp.py` | ferramenta de análise estática (PE + .pdata + capstone) |

**Registro e `ReShade.ini` foram restaurados ao estado original ao fim da rodada.**

---

## 9. O que falta, em ordem de impacto na imagem

1. ~~A função de transferência do grading.~~ **Resolvida** — §5.7. O `style_compare.py` está correto.
2. **O `structEff` sem slot no runtime AMD (§6.1).** Verificação do lado AMD, não da NVIDIA. Passa a
   ser o item de maior impacto em aberto.
2b. ~~A inicialização dos 14 coeficientes.~~ **Resolvida** — §5.8. A struct é zerada aos neutros a
   cada evaluate, antes de tudo. Não há risco de valor velho.
3. ~~O `exposure` do `forward`.~~ **Resolvido** — §5.9. Não existe: aquele argumento é o ponteiro de
   device do `DLSSNR.ControlMask`, nulo no caminho do RenoDX. Nenhuma métrica de exposição a
   replicar.
4. **O que `Intensity` (`opts+0xE0`) faz.** Vai para `stage+0xF0`, logo é parâmetro do kernel, e não
   é control (não zera history). Provavelmente o blend final com o quadro original, mas não foi
   confirmado.
5. **`UICorrection` (`opts+0x10C`).** O RenoDX manda 1; o default da DLL é 0. Não foi investigado.
6. **Os outros 11 coeficientes de grading.** Existem e estão zerados nesta build. Se uma build futura
   da NVIDIA os usar, a AMD precisa saber o que cada um faz — e lembrar que o de índice 1 tem
   neutro 1.0.

### Como repetir isto noutra build

Os RVAs acima valem para esta `nvngx_dlssnr.dll` e para este `_nvngx.dll` (driver 610.62). Numa
build diferente:

- O `ngx_param_trace2.js` tem o RVA da vtable de `NVSDK_NGX_Parameter` fixo em `KNOWN_VTABLE_RVA`.
  Ele **valida antes de usar** (confere o slot 0) e cai para descoberta dinâmica se não bater, mas o
  jeito rápido é recalcular: pegar o endereço da vtable de um objeto de parâmetros em runtime e
  subtrair a base do módulo. O script imprime uma calibração por desmontagem dos 8 slots de `Set`
  logo no começo do log — conferir que `xmm=YES` cai só nos slots de `double` e `float`.
- No `pehelp.py`, tudo é por nome de string, não por endereço: `strxref "reset temporal history"`
  reencontra a função de reset, `f32 0.0078125` reencontra o `style/128`, e a tabela de grading sai
  do `lea` que a `CG2RFindWeightByPreset` usa. Os únicos números mágicos são o stride `0x288` da
  entrada e o `0x44` do bloco de style, e ambos aparecem literalmente no código.

---

## Resposta do lado AMD (22/09) aos itens em aberto da §9

Lido no IDA sobre `dlssnr_amd_pass1.dll` v0.3.0 (`D:\pcsx2-v2.8.2-test`), worker `sub_180018670`,
bloco `0x180019070`–`0x1800190E5`:

- **Item 2, `structEff` sem slot: falso.** O worker monta **cinco** floats contíguos em `0x96F98`:
  `movq [0x96F98] <- (0x97B30, 0x97B34)` = tone e structure **cru**; `[0x96FA0] = skinEff`;
  `[0x96FA4] = structEff`; `[0x96FA8] = [0x97B3C]` = style/128. A §6.1 partiu da descrição antiga do
  handoff com três campos; a §3.2 do handoff AMD já tinha os cinco. Os quatro valores que a NVIDIA
  carrega em `0x22548`–`0x22563` (structEff, skinEff, LocalStructure cru, LocalTone) mais o style têm
  cada um o seu slot aqui.
- **Derivação (§5.2): igual.** Com `UseAutoMask == 0`, `skinEff = structEff = -1.0`
  (`dword_18006ABAC = 0xBF800000`); com a máscara ligada, `skinEff = Skin >= 0 ? Skin : LocalStructure`
  e `structEff = LocalStructure`. Mesmos dois valores, mesma regra.
- **"tone 0 — errado" (§6.1): o addon manda 1.0.** `g.tone` nasce em 1.0 e vai para `0x97B30` no
  passe 1; só os passes ≥ 2 recebem 0, como o fork de referência faz (`PassProfiles.h`). O "tone 0"
  do handoff AMD §4.2 era o **default do ini do runtime**, que o addon sobrescreve.
- **Item 3 (`exposure`): coerente.** O runtime AMD também não calcula exposição nenhuma para a rede;
  o `auto-exposure: encoded mean` que aparece no log dele é do pós, não do forward.
- **Achado colateral:** com `ToneChannels == 0` o worker zera tone e structure antes da rede
  (`pcmpeqd` + `pandn` sobre o par). O addon sempre escreve o bit 4 (política de timeout), então
  isso nunca acontece — mas é um alçapão para quem "limpar" esse campo.
- **Item 4 (`Intensity`) e §6.2 (multi-pass):** do lado AMD `Intensity` é o blend do compose, e o
  compose roda uma vez no fim da cadeia, com o passe *k* lendo a saída do *k−1*. Mesmo arranjo.
- **Item 5 (`UICorrection`):** o runtime AMD não tem o campo mapeado. Sem correspondente; fica em
  aberto dos dois lados.
