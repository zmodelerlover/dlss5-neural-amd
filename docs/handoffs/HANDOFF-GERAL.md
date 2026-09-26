# HANDOFF GERAL — por onde começar, seja qual for a frente

Este é o mapa, não o manual. Ele **aponta onde pesquisar**; o detalhe está no código e nos handoffs
de sessão. Mande este quando o assunto for "vou mexer no projeto" sem saber ainda em quê.

Última revisão: 26/09/2026 (runtime v0.4.0, publicado no v0.6.7).

---

## 1. O que é

Add-on do ReShade que roda **DLSS-5 Neural Rendering em GPU AMD**.

O add-on captura o quadro apresentado, entrega a um runtime de terceiro
(`dlssnr_amd_pass1.dll`, projeto "DLSS-NR-on-AMD" v0.4.0, que roda a rede em kernels HIP
pré-compilados), e compõe a resposta de volta na tela. Um efeito ReShade companheiro
(`DLSS5_Neural_Feed.fx`) fornece movimento e profundidade.

**A rede é a da NVIDIA, byte a byte** — 153 tensores idênticos aos do `nvngx_dlssnr.dll`. Então
diferença de imagem entre AMD e NVIDIA está na composição, nos controles e na aritmética fp8, nunca
no modelo.

---

## 2. Onde está tudo

| | |
|---|---|
| repo | `Desktop\dlss5amdrework\repo`, branch **`feed`** sobre `73d152c` — **nada commitado** |
| o add-on inteiro | `src/neural/neural.cpp`, **7752 linhas** |
| rotas alternativas | `src/neural/vk_route.inc`, `gl_route.inc` |
| endereços do runtime | `src/neural/runtime_offsets.h` — **só este arquivo pode nomear endereço** |
| efeito companheiro | `shaders/DLSS5_Neural_Feed.fx` |
| ponte 32-bit | `src/x86bridge/` (tem overlay próprio, `overlay32.inc`) |
| build | `build.ps1` → `build/dlss5-neural.addon64` |
| bancada | `D:\pcsx2-v2.8.2-test` (PCSX2 + ReShade + runtime + pesos) |
| dossiê da sessão de 22/09 | `Desktop\dlss5amdrework\dossie-rocm-spike-20260922\` |

---

## 3. Mapa do `neural.cpp`

O arquivo é grande. Navegue por estas âncoras, não leia do começo.

| linha | o quê |
|---|---|
| ~725, ~756 | `kRuntimeSha256` e `RuntimeHashMatches` — o portão de hash do runtime |
| ~1247, ~1511 | o struct global `g`, onde moram os atômicos de todo controle |
| 1732 / 1955 | `LoadSettings` / `SaveSettings` — o ini, ida e volta |
| ~1643 | onde o ini comentado é gerado quando não existe |
| 2158 | `DrainReadbacks` — **a medição**: `measure, residual` e o ratio `gRes/gIn` |
| ~2478–2523 | o bloco que emite as linhas de medição e o watchdog `g.inert` |
| 2541 / 4726 | `RecordNetwork` — grava a avaliação na lista de comandos |
| 3141 | `BridgePresent` |
| 3384 | `InitHip` — acha a GPU por LUID |
| 3628 / 3696 | `ArmRuntime` / `InitEngine` — carrega o runtime, `LoadLibrary` em ~3678 e ~3733 |
| ~5245–5300 | **os controles escritos no runtime por frame** (tone, structure, skin, scale, ToneChannels) |
| ~5455 | o gatilho da medição (frame 240, depois a cada 1800) |
| 5728 | `OnPresent` — a entrada de cada quadro |
| **6121–6212** | **os helpers da UI**: `T()`, `Help()`, `Tag()`, `struct Risk` |
| **6213** | **`OnOverlay`** — daqui até ~7600 é a interface inteira |

### As 8 seções do overlay

`Imagem` 6309 · `Desempenho` 6659 · `Guias` 6877 · `Depuração` 7223 · `Motor` 7282 ·
`Avançado` 7421 · `Experimental` 7474 · `Estado` 7532.

### Para adicionar um controle

Copie o caminho do `depthInverted`, que é o exemplo mais limpo e aparece em cinco lugares:

```
~1511  declaração    std::atomic<int> depthInverted { 1 };
~1808  LoadSettings  g.depthInverted.store(flag(L"DepthInverted", true) ? 1 : 0);
~1939  SaveSettings  flag(L"DepthInverted", g.depthInverted.load() != 0);
~5280  escrita       At<UINT>(r, rt::kDepthInverted) = ...      (só se for pro runtime)
~7010  UI            Checkbox + Help(en, pt) + Tag(...)
```

Esqueça um dos cinco e falha em silêncio: sem o Load a chave do ini é ignorada, sem o Save o valor
não sobrevive ao fechar.

---

## 4. Quero mexer em X → leia Y

| frente | leia | olhe |
|---|---|---|
| **interface / overlay** | esta §3 | `neural.cpp` 6121–7600 |
| composição, como a imagem é formada | `docs/nvidia-parity.md` | `shaders/DLSS5_Neural_Feed.fx`, `kComposeShader` |
| Model A/B/C, estilo | `docs/styles-model-abc.md` | `NeuralStyle()` |
| falar com o runtime, offsets | `src/neural/runtime_offsets.h` (leia o cabeçalho inteiro) | `tools/runtime-patches.json` |
| medir se um controle faz algo | `dossie.../medicoes/README.md` | `DrainReadbacks` 2158 |
| kernels HIP, pesos, engenharia reversa | `dossie.../LEIA-PRIMEIRO.md` | `tools/carve_amd_kernels.py`, `read_amd_weights.py` |
| o que já foi tentado e fechado | `handoffs/README.md` (explica a cadeia) | — |
| instalar / empacotar | `docs/install.md` | `tools/package-release.ps1` |

---

## 5. Compilar e checar

```powershell
.\build.ps1 -Target neural          # -> build\dlss5-neural.addon64
```

Antes de dar qualquer coisa por pronta, os quatro checks:

```
python tools\runtime_offsets_check.py <dlssnr_amd_pass1.dll>
python tools\feed_fx_check.py
python tools\style_check.py
python tools\compose_check.py
```

Reinstalar exige o jogo fechado — a DLL está carregada.

---

## 6. As regras duras

1. **Endereço do runtime só em `runtime_offsets.h`.** Literal em `.cpp` é bug; o checker falha.
2. **Todo texto visível passa por `T(en, pt)`.** O overlay é bilíngue por construção.
3. **Todo controle leva `Tag(...)`** — `kMeasured`, `kTraced`, `kUnknown`, `kDanger`. A tag diz ao
   usuário se aquilo foi medido, lido no disassembly, ou é palpite. Não minta na tag.
4. **Leitura que vira texto precisa de medição antes.** Custou um dia (§2 do handoff de 22/09).
5. **Contradição entre duas leituras é pedido de medição, não debate.**
6. **Chave de ini não prova que a feature existe.** Confira no fonte E no binário.
7. **Commit: só a linha de assunto.** Sem corpo, sem atribuição de IA, sem `Co-Authored-By`.
8. **Screenshot não é evidência.** Número de log é.

---

## 7. O que está fechado (não reabrir sem motivo novo)

- **NR Preset** — a DLL tem um conjunto de pesos só; o combo não faz nada nem na NVIDIA.
- **Model A/B/C entrar na rede pela rota atual** — o vetor de condicionamento vive só em LDS.
  *Reaberto em parte pelo v0.4.0:* o próprio runtime lê a chave `Style` do ini dele e manda
  `Style/128` para a lane 10 (lido nos kernels). Chega na saída: no framecheck (quadro
  sintético), sem o pino, `Style=2` move a saída em 0,008 de média; no jogo não foi medido. O
  overlay do runtime standalone grava `Style` de volta nesse ini, então a pasta de um jogo pode
  trazer 1 ou 2. O `ArmRuntime` fixa o campo em 0 (`rt::kStyle`) por cima do ini, que é o zero
  do v0.3.0; `ToneCurve` e `ToneLift` ficam fixos nos padrões pelo mesmo motivo. Com os pinos,
  um ini com tudo isso dá a mesma saída byte a byte
  (`daniel-runtime/analysis-reshade/fix2/lt/`). Qualquer outro valor, ou mostrar na UI, exige
  medição no jogo antes (regra 4).
- **Kernel custom com ROCm pra levar controle à rede** — mesma razão, por construção.
- **Embarcar hiprtc** — 111,8 MiB pra economizar 60 ms.
- **Textura D3D12 compartilhada pro kernel** — zera em silêncio devolvendo `hipSuccess`.
- **Checkboxes de `NOBLEND`/`NOPOSTHIST`** — medidos, +66% e +31% de força, mas trazem flicker;
  rejeitado.
- **Ângulo de câmera diferente.**

## 8. O que está aberto, em ordem de valor

1. **A saída da rede no v0.4.0 não é a do v0.3.0, e a causa não foi achada.** No framecheck
   (quadro sintético 960x540, mesmas configurações, RX 9070 XT) o |resíduo| médio do v0.4.0 é
   1,18x o do v0.3.0 no 1º quadro, 1,15x no 12º e 1,24x no 20º, com a mesma estrutura (correlação
   0,985 a 0,989). Já aparece no 1º quadro, que não tem histórico. Não são os kernels novos de
   gfx12 (`DLSSNR_NO_REG=1` dá saída idêntica bit a bit) nem `UseGameExposure`/`Residual`. Não
   bloqueia mais: o A/B no jogo contra o v0.3.0 (GTA IV, ponte 32-bit) foi aprovado em 26/09, e o
   v0.6.7 saiu no mesmo dia, junto com o AMD-NR-ReShade-Installer v0.6.1, que trocou o runtime, o
   add-on e a ponte no payload. Scripts e medições em
   `daniel-runtime/analysis-reshade/loadtest/`. Próximo passo: bisectar com v0.3.1/v0.3.3; os
   endereços deles foram lidos na mesma análise (`map-options-code/map_all.json`,
   `map-data-state/datamap_*.pkl`, `map-patches/patch_sites.json`), mas não foram conferidos.
2. Mapear `VarParams` no `k_swin_var<32,true>`, o kernel que realmente embarca. No v0.4.0 a
   lane 10 é `VarParams+0x5c`; em gfx12 o caminho padrão virou `k_reg_swin32<20>`.
3. Lane 10 — o quinto input de condicionamento. Lido no v0.4.0: é o `Style/128` do ini do runtime
   (no v0.3.0 era zero fixo). No framecheck, sem o pino, muda a saída; o add-on fixa em 0. Falta
   medir o efeito na imagem de um jogo antes de qualquer controle.
4. Comparação A/B com a máquina NVIDIA na mesma cena.
5. RDR1 em D3D12 — a rota nunca foi exercitada.
6. `DepthInverted` num jogo com depth real.

---

## 9. A cadeia de handoffs

`handoffs/README.md` é o índice e explica a precedência: **o mais recente vence** onde contradiz.

Os dois que importam hoje:

- `HANDOFF-rocm-spike-knobs-e-paridade-de-pesos-20260922.md` — a paridade de pesos, os kernels
  legíveis, as chaves escondidas do runtime, e a §8 com os erros de método e a correção de cada um.
- `HANDOFF-style-preset-fechado-e-roadmap-20260922.md` — o estado do NR Style/Preset, o erro do
  "quinto slot", a paridade de composição e a ordem de commit sugerida.
