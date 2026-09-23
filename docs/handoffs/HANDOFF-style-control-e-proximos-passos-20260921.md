# HANDOFF — sessão de 21/09/2026 (noite): o que foi feito, o que foi descoberto, e como continuar quando chegarem os logs da NVIDIA

Este é o handoff de continuidade do lado AMD. Ele resume a sessão inteira (que começou no
`HANDOFF-feed-launchpad-20260921.md` e passou pelo `HANDOFF-launchpad-multipass-20260921.md`),
registra a descoberta que muda a arquitetura do Style, e diz exatamente o que fazer com os logs que
vão chegar da máquina NVIDIA (`HANDOFF-nvidia-preset-style-ets2-20260921.md`, enviado no zip
`Desktop\nvidia-preset-style-ets2-20260921.zip`).

Leia a §3 antes de qualquer coisa: é a descoberta central e ela invalida uma conclusão que estava
documentada em `docs/styles-model-abc.md`.

---

## 1. Onde está tudo

| | |
|---|---|
| Repo | `C:\Users\claudinhh\Desktop\dlss5amdrework\repo`, branch **`feed`** sobre `73d152c` |
| Estado do fonte | **nada commitado nesta sessão.** 5 arquivos modificados (`src/neural/neural.cpp`, `vk_route.inc`, `gl_route.inc`, `shaders/DLSS5_Neural_Feed.fx`, `handoffs/README.md`) e 5 novos (3 handoffs, `tools/style_compare.py`, `tools/ngx_param_trace.js`). Ver §7 para a ordem de commit sugerida. |
| Build atual | `repo\build\dlss5-neural.addon64`, 595 456 bytes, 22:24, sha256 `a4f0f21d...` |
| Instalado em | ETS2 `bin\win_x64` (D3D11, com Launchpad), bancada PCSX2 `D:\pcsx2-v2.8.2-test` (D3D11, com Launchpad), RDR1 (D3D12, **build anterior** de 594 944 bytes, sem a correção do style) |
| Pacote para outro dev (antes do style) | `Desktop\dlss5amdrework\dlss5-neural-amd-teste-20260921.zip` — **desatualizado**: não tem a correção da §3 nem os handoffs novos |
| Pacote para a máquina NVIDIA | `Desktop\nvidia-preset-style-ets2-20260921.zip` (handoff, `ngx_param_trace.js`, `style_compare.py`, `styles-model-abc.md`) |
| DLL NVIDIA analisada | `Desktop\a\nvngx_dlssnr.dll`, 165 840 496 bytes, sha256 `e16bcf15...`, build `rel_310_8`; IDB em `Desktop\a\nvngx_dlssnr.dll.i64` |
| Runtime AMD analisado | `D:\pcsx2-v2.8.2-test\dlssnr_amd_pass1.dll` v0.3.0, sha256 `70af3fb7...`; IDB ao lado |
| Referências clonadas (descartáveis, scratchpad da sessão) | fork Matheus (`MatheusGViana/dlss-5-amd-project`), fork y4my4my4m (`OptiScaler_DLSSNR_Multipass_MFG`, branch `dlss-neural-rendering`), `clshortfuse/renodx` (sparse) |

---

## 2. O que foi feito nesta sessão, em ordem, com evidência

### 2.1 Ordem no ReShade (PCSX2, validado)
No D3D11 o ReShade dispara `addon_event::present` **antes** de renderizar os effects. O addon lia
`DLSS5N_MV` do frame anterior. Agora, com o feed ligado, `RenderEffectsAheadOfNetwork()` chama
`render_effects` antes de copiar o back buffer (`SelfIssued` para não relockar `g.lock`). Log:
`effects: DLSS5_Neural_Feed is on, so ReShade's chain now runs before the network`.

### 2.2 Um módulo do runtime por passe (PCSX2, validado pelo usuário: ruído sumiu com 2 e 3 passes)
O runtime tem um estado temporal só (engine object, buffers HIP, auto-exposure, post history).
Agora `Passes ≥ 2` em modo serial inline copia o runtime para `dlss5-pass2.dll`/`pass3.dll` e carrega
cada cópia como módulo próprio (`ArmRuntime`, `LoadExtraRuntime`, `BringUpEngines(UINT&)`, helpers
`RuntimeFor/RuntimeBusy/ResetJobs/NotifyRuntimes`). É o mesmo desenho do fork do Matheus
(`AmdPreSr.cpp::InitPass`). Log: `pass 2: running through its own copy of the runtime (dlss5-pass2.dll)`.

### 2.3 Latch na validação do feed (PCSX2, medido)
`PS_History` guardava o vetor **validado**; com um zero anterior, todo movimento > 2,8 px falhava a
consistência para sempre. Máximo recebido pela rede: 2,12 px. Agora guarda o vetor cru do provider;
máximo passou a 10,7 px. Visão de debug "Vetores de movimento" satura em 8 px (era 32).

### 2.4 Miudezas de bancada
`EffectSearchPaths` com `\**\**` não acha effect nenhum (erro 123): usar `\**`. Preset do ReShade
6.x: `Techniques=`/`TechniqueSorting=` na seção **sem nome**, não em `[GLOBAL]`; Launchpad antes do
feed. Flow Quality do Launchpad em High. Vibração do controle do PCSX2 desligada
(`inis\PCSX2.ini`, `LargeMotorScale/SmallMotorScale=0`, backup `.bak-antes-vibracao`).

### 2.5 Instalações
- **RDR1** (D3D12): rodou; `engine ready`, raster 1920x1080, profundidade D3D12 encontrada, ~1/3 dos
  frames pulados por tempo a 1080p Scale 1.00. Build anterior.
- **ETS2** (D3D11): instalado limpo em `bin\win_x64` (ReShade 6.8.0.2155, addon, runtime, pesos, feed,
  Launchpad, preset). Validado: `motion from the effect, depth from the game`, `guide probe, motion:
  0% exactly still`. É o jogo de comparação com a NVIDIA.

### 2.6 NR Preset: fechado como impossível nesta DLL
`CG2RFindWeightByPreset` percorre tabela de **uma** entrada (648 bytes, id 1). Qualquer outro valor
cai em `preset %d is not available in this DLL build; falling back to shipping default preset 1`.
O valor não é usado em mais nada (rastreado de `CreateFeature` → `CreateNetwork` → descritor [0]).
OptiScaler e RenoDX só encaminham o hint; nenhum implementa preset. Se um dia vier uma DLL com mais
entradas: `tools/extract_runtime.py` por blob + um `.bin` por módulo.

---

## 3. A descoberta central: Style entra na REDE, e o runtime AMD tem a entrada

O usuário insistiu que na NVIDIA trocar o Model "muda muito" e aqui só mudava cor. Ele tinha razão.

### 3.1 NVIDIA (`nvngx_dlssnr.dll`, IDA)
- `CG2RNetworkManager::Evaluate` (`sub_180021BB0`): `style = clamp(opts[59], 0, n-1)` onde `n` é o
  número de estilos do descritor da rede (entrada de 240 bytes, offset +100); chama o forward da
  `HNetCpp::CCNetwork` (`sub_18003F490`) com `LocalTone, LocalStructure, style × 0,0078125,
  exposure, skinEff, structEff`. **`style/128` é um controle de entrada da rede.**
- O grading (exposição −0,10 EV, contraste −0,25, sat ×0,90 para style 1; sat ×0,85 para style 2)
  continua existindo, no kernel de pós-processo, escalado por `LocalToneStrength`.
- `CG2R_ResetTemporalHistoryOnControlChange` (`sub_1800179D0`) zera o history quando muda Style,
  UseAutoMask, LocalTone, LocalStructure ou Skin; loga `reset temporal history for <config> after
  control change`.
- Leitores de `opts[59]`: só o param-read (`sub_180019F30`), o reset acima e o Evaluate. Leitores de
  `opts[73..86]` (grading): só o teste "é neutro" e o empacotamento do kernel de pós.

### 3.2 AMD (`dlssnr_amd_pass1.dll`, IDA)
Worker `sub_180018670`, `0x180019070`–`0x1800190E5`: monta o vetor de controle em `0x96F98..0x96FA8`:
`ctl[0]=0x97B30` (tone), `ctl[1]=0x97B34` (structure), `ctl[2]=skin efetivo` (0x97B38, ou structure
se skin ≤ 0, ou −1 se UseAutoMask=0), `ctl[3]=structure efetivo`, e **`0x96FA8 = 0x97B3C`**, o campo
que o addon expunha como `EngineScale` ("Scale" no ini do runtime, default **0,03125 = 4/128**).
Conclusão: o runtime sempre teve a entrada de style; o addon alimentava um "style 4" fixo. Isso
explica tanto o "só cor" quanto, possivelmente, parte do visual que se via até hoje.

### 3.3 O que foi mudado no addon (build 22:24, **não validado em jogo**)
- No loop de passes: `At<float>(r, rt::kScale) = StyleControl()`, onde `StyleControl()` = `style/128`
  se `EngineScale` está no default 1/32, senão o valor manual (override explícito).
- Troca de Model no overlay: `g.historyValid = 0` + log
  `menu: style N -> network control 0.0078125 (style/128; EngineScale 0.031250), history reset`.
- Grading continua igual (compose), como na NVIDIA.
- Docs e tooltips do overlay **ainda dizem** "não são três redes, é só grading". Corrigir
  `docs/styles-model-abc.md`, o help do combo e o comentário do ini (`neural.cpp` ~1672 e ~6005).

### 3.4 Primeira coisa a fazer na próxima sessão
Abrir o ETS2 (DirectX), `Ctrl+End`, trocar Model A → B → C e confirmar: (a) a linha de log acima;
(b) que a diferença visual agora é de natureza (iluminação/detalhe), não só de cor; (c) que `Model A`
(controle 0) não ficou pior do que o antigo "style 4" — se ficou, isso é dado, não defeito: a NVIDIA
roda em 0 por padrão.

---

## 4. Quando chegarem os logs da NVIDIA: o que fazer com cada um

O operador de lá entrega (ver handoff da NVIDIA, §5): `ngx_params.log` (trace Frida), o log do NGX,
`ReShade.log`, `ReShade.ini`, ini do addon RenoDX, hash da DLL, anotações.

1. **`ngx_params.log` → tabela por Model.** Para A, B, C, listar todo `DLSSNR.*` escalar. Pergunta:
   *o RenoDX muda só `Style`, ou muda LocalTone/Structure/Skin/Intensity/Preset junto?* Se muda
   mais, o combo da AMD tem de fazer o mesmo conjunto de escritas. Mapa NVIDIA → AMD:

   | NGX (NVIDIA) | opts | runtime AMD | addon |
   |---|---|---|---|
   | `DLSSNR.LocalToneStrength` | 57 | `0x97B30` | `Tone` / `Pass1Tone` (`TuningFor`) |
   | `DLSSNR.LocalStructureStrength` | 58 | `0x97B34` | `Structure` |
   | `DLSSNR.Style` | 59 | `0x97B3C` = style/128 | `Style` (novo) |
   | `DLSSNR.UseAutoMask` | 60 | `0x97B40` | `AutoMask` |
   | `DLSSNR.SkinStructureStrength` | 61 | `0x97B38` | `Skin` |
   | `DLSSNR.Intensity` | +224 | não é controle da rede; na AMD é o compose (`Intensity`) | `Intensity` |
   | `DLSSNR.DepthInverted` | +260 | `0x97B10` (fixo em 1) | não exposto |
   | `DLSSNR.MVecScaleX/Y` | +216/220 | `Packet.scaleX/Y` (1.0) | `MotionScale` |
   | `DLSSNR.Hint.Render.Preset` | criação | não existe (um peso só) | — |

2. **Defaults do RenoDX** (o primeiro evaluate no trace). Comparar com os nossos: tone 0 no passe 1
   (`TuningFor`), structure 1, skin −1 (= "segue structure" quando AutoMask), AutoMask 1. Qualquer
   diferença é candidata a explicar diferença de imagem tanto quanto o style. Ajustar defaults do
   ini/overlay para os do RenoDX **só depois de medir**, um de cada vez.

3. **Log do NGX.** `config(s) available` = 1 e o nome após `->` igual em todos os presets fecha o
   preset de vez (registrar no `docs/styles-model-abc.md`). As linhas `reset temporal history` dizem
   em quais trocas a DLL zera o history: hoje a AMD zera só no Style; se a NVIDIA zera em
   Tone/Structure/Skin/AutoMask também, replicar (`g.historyValid.store(0)` nos handlers).

4. **Se o trace mostrar `Style` com valores fora de 0..2**, ou se o NGX log mostrar clamp, anotar
   `n`: a AMD pode expor mais estilos do que o menu da NVIDIA.

5. **Se o hash da DLL de lá for outro**, pedir a DLL antes de qualquer conclusão.

6. Fotos, se vierem: `style_compare.py` mede só a parte de grading; com o style na rede, `on vs pred`
   não vai ao piso e isso é esperado. Não usar como veredito.

---

## 5. O que continua em aberto (além do §4)

- **Validar a correção do style em jogo** (§3.4). É a pendência número um.
- **Docs e overlay** ainda descrevem o style como só grading (§3.3).
- **Pacote para o outro dev** está desatualizado; regerar com o build 22:24 e os handoffs novos
  (`tools/package-release.ps1 -Private` ou o mesmo staging manual desta sessão).
- **RDR1** está com o build anterior; atualizar se for usado.
- **`EngineScale` no overlay**: o texto diz "Engine Scale, Reset to 1/32, UNKNOWN". Renomear para o
  que é (controle de estilo / override) e mudar a tag para TRACED.
- **Depth normalise** no PS2 continua sem medição limpa (três sinais contra); ETS2 tem profundidade
  real (3,4% da faixa, escala 29,7x), outro caso a olhar.
- **Rotas D3D12/Vulkan** com cópias do runtime por passe: não exercitadas (RDR1 rodou com 1 passe).
- **Bridge 32-bit** não reconstruída; não ganha nada disto.

---

## 6. Armadilhas confirmadas hoje

- Nunca tirar conclusão de screenshot; o usuário rejeitou prints como evidência, com razão.
- `EngineScale` fora de 0,03125 desliga o style na rede (vira override).
- Heredoc Bash com aspas simples no meio quebra; patches Python vão em arquivo `.py` no scratchpad.
- Substituição textual global: contar ocorrências antes (o `ResetJobs()` quase virou recursão).
- Preset do ReShade em `[GLOBAL]` é ignorado; `\**\**` não acha shader.
- Reinstalar o addon exige o jogo fechado (DLL carregada).

---

## 7. Commits sugeridos (uma linha, sem corpo, sem atribuição), nesta ordem

1. ReShade chain runs before the network when the feed effect is on
2. One runtime module per pass, so each pass keeps its own temporal state
3. Keep the provider's raw vector in the feed history, not the validated one
4. Motion debug view saturates at 8 px
5. Style drives the network's control slot, not only the grade
6. Handoffs, style_compare and the NGX parameter trace

Antes do 5, validar em jogo (§3.4). Antes de tudo, `python tools\runtime_offsets_check.py`
(PASS hoje) e `python tools\feed_fx_check.py` (PASS hoje).

---

## 8. Fechamento (21/09, madrugada): os logs da NVIDIA chegaram e o Style/Preset foi corrigido

Resultado da máquina NVIDIA em `RESULTADO-nvidia-preset-style-ets2-20260921.md` (cópia das notas
do operador; os logs brutos, 20 MB, ficaram em `Desktop\nvidia-preset-style-ets2-20260921.7z`).
O que ele fechou e o que foi feito no addon (build 23:49, 595 968 bytes, sha256 `a5ab092b...`):

| pergunta da §4 | resposta medida | feito no addon |
|---|---|---|
| RenoDX muda mais que `Style` ao trocar Model? | **Não.** Só `DLSSNR.Style` + pulso de `Reset`. | Combo já só mexia em `style`; mantido. |
| Defaults do RenoDX | tone 1, structure 1, skin 1, AutoMask 1, intensity 1, MVecScale 1/1, DepthInverted 0, preset 1 | Os nossos já eram iguais (o "tone 0 no passe 1" da §4.2 estava errado: `g.tone` é 1.0; TuningFor zera só nos passes ≥ 2, como o fork). Nada mudou. |
| Quando a DLL zera o history | Style, UseAutoMask (exato); LocalTone, LocalStructure, Skin, skinEff, structEff (`|Δ| > 1e-5`). **Não** em Intensity. | `ControlsChanged()` no loop de passes, antes de ler o history: uma comparação por passe, cobre overlay, ini reload, perfil por passe e override. Log `pass N: reset temporal history after control change (...)`. O reset manual do combo saiu. |
| `n` de estilos | **3**, medido em runtime; clamp unsigned a 2 | Já clampávamos 0..2. Confirmado. |
| Hash da DLL | `E67DEE20...`, mesmo tamanho: build patcheada pré-RTX-50; tabela de estilos e `Evaluate` idênticos | Nada a pedir. |
| NR Preset | `1 config(s) available`, `preset=1` nas duas features, fallback nunca apareceu | **Encerrado dos dois lados.** Texto do combo, ini e doc dizem isso agora. |

Mais duas correções que saíram da análise estática de lá:

- O grade do estilo é escalado por `LocalToneStrength` limitado a 0..1 **na DLL**. O addon aplicava só
  `StyleStrength`. Agora `StyleGradeStrength() = StyleStrength × clamp(Tone, 0, 1)`; com os defaults
  (1 e 1) não muda nada, com Tone 0.5 o grade cai pela metade como na NVIDIA.
- Overlay e ini reescritos: o combo do Model explica as duas metades (controle da rede + grade), tag
  MEASURED; o bloco "Model A/B/C: not portable to this runtime. Closed" foi apagado; "Engine Scale"
  virou "Style Control Override" (tag TRACED, botão "Follow Model", aviso amarelo quando está fora de
  1/32); o tooltip de Local Tone deixou de dizer só "inerte" e explica que é o slot 1 da rede e a
  escala do grade. `docs/styles-model-abc.md` ganhou a seção de correção no topo.

### O que continua em aberto

1. **Validar em jogo** (§3.4) — ainda não feito nesta sessão. Trocar A → B → C no ETS2 e conferir a
   linha `pass 1: reset temporal history after control change (style control 0.007812, ...)`.
2. **Derivação skinEff/structEff com AutoMask=0**: na DLL da NVIDIA os dois vão a −1. Na AMD isso é
   feito dentro do worker do runtime (§3.2), não no addon; não foi conferido se `ctl[3]` também vai
   a −1. Só importa com Character Mask desligado (não é o default).
3. **DepthInverted**: RenoDX manda 0, o addon pina 1 (`rt::kDepthInverted`). Fora do escopo de
   style/preset; anotado.
4. Pacote pro outro dev e RDR1 seguem com build anterior.

Ordem de commit da §7 continua; o item 5 agora é "Style drives the network's control slot and the
history resets on control change".

## 9. Paridade com a NVIDIA além do style (22/09, madrugada)

Pergunta do usuário: "funciona igual ao da NVIDIA agora?" Resposta medida em `docs/nvidia-parity.md`.
Resumo do que se descobriu e do que mudou:

- **Na NVIDIA a tela também é uma composição**, não a saída bruta da DLL. O fork OptiScaler DLSS-NR
  (`y4my4my4m`, branch `dlss-neural-rendering`, `dlssnr.hlsl`) diz no README que reimplementa a
  composição do RenoDX: razão de luminância em dois ramos, correção de matiz OkLab, guarda 2.0 dos
  dois lados, blend de cor. Com a rede vendo o próprio frame (proxy == original) os dois ramos viram
  razão 1, o OkLab vira identidade, e o que sobra é exatamente o caminho de razão do nosso
  `kComposeShader`. Já éramos iguais em Intensity ≤ 1.
- **Intensity > 1**: o fork eleva a razão a `1 + (s-1)` em vez de esticar o resíduo. Portado.
- **Derivação skin/structure com AutoMask=0**: o worker AMD (`0x180019070..E5`) põe os dois em −1.0
  (`0x18006ABAC` = `0xBF800000`). Igual à NVIDIA. Nada a mudar.
- **Achado colateral no worker**: com `ToneChannels == 0` o runtime zera LocalTone e LocalStructure
  antes da rede. O addon sempre escreve o bit 4, então nunca acontece; está comentado no ponto da
  escrita para ninguém "limpar" isso.
- **Grade no ETS2**: RenoDX também roda no Present lá. Posição igual. Nada a mudar.
- **Entradas temporais**: no ETS2 a NVIDIA rodou com MV/depth falsos. Não é para copiar; é para
  reproduzir só na comparação: `Motion=0 Depth=0 Temporal=2`.
- **DepthInverted**: agora é chave de ini (`DepthInverted`, default 1) e checkbox sob Depth. O 0 do
  RenoDX foi medido com depth falsa e não vale como leitura.
- **Network Output** agora aplica o grade do Model (dbg 6 no compose). Continua fora de qualquer
  comparação: a NVIDIA nunca mostra isso.
- **Continua diferente, de propósito**: `ResidualLimit=0.25` e `EdgeFade` (nossos, contra o blow-up
  de tile do port AMD), bicúbico no upsample (fork é bilinear), e o proxy HDR (fork: SoftKnee+sRGB;
  nós: linear×k). Cada um com a chave de ini que o desliga anotada no doc.

Build 00:04 de 22/09, 599 552 bytes; instalado em ETS2 e PCSX2. **Continua sem validação em jogo** — a pendência
número um não mudou.

### 9.1 Segundo pacote da NVIDIA (zip de 00:11)

Notas do operador atualizadas (§5.5–5.9, 6.1, 6.2, 9) em `RESULTADO-nvidia-preset-style-ets2-20260921.md`,
com uma seção "Resposta do lado AMD" no fim. O que ele fechou: o operador de grading verificado
instrução a instrução no SASS sm_86 (nosso compose e o `style_check.py` já batem), a struct é
reinicializada aos neutros a cada evaluate, e o "exposure" do forward não existe — é o ponteiro de
device do `DLSSNR.ControlMask`, nulo no RenoDX. O que ele deixou aberto e eu respondi pelo IDA da AMD:
`structEff` **tem** slot (`0x96FA4`), a derivação é igual, e o tone que mandamos é 1.0. Sem mudança
de código. `sass.py` dele virou `tools/sass_window.py`; o SASS sm_86 (454 KB) ficou no zip.

## 10. ERRO da §3 corrigido (22/09 00:45): 97b3c NÃO é o controle de estilo

O usuário abriu o ETS2 com o build 00:04 e reportou o sintoma antigo: FPS muda, "processed, 0
skipped", mas ligado/desligado, Model e pass count não mudam nada na tela. O log do addon mediu:

```
measure, network input: mean absolute 0.454943
measure, residual at 1920x1080: mean 0.000240, max 0.009766
```

Resíduo 100x menor que o normal (God of War: 0,021). A rede devolvia a entrada.

Causa, lida no IDA do `dlssnr_amd_pass1.dll`:
- O worker monta **quatro** floats de controle em `0x96F98..0x96FA4` (tone, structure cru, skinEff,
  structEff). O próprio log do runtime imprime `ctl (%.2f %.2f %.2f %.2f)`, quatro valores.
- `0x96FA8` (objeto+48), que recebe `97b3c` ("Scale", default 1/32), **não é controle**: no evaluate
  (`sub_18002D2D0`) objeto+32/+40 vão para o kernel *pre* que alimenta a rede, e objeto+48 vai
  para o kernel *post* que escreve a saída (argumento depois do ponteiro de history).
- A §3.2 deste handoff inferiu "quinto slot = style" pela adjacência e pela ordem do forward da
  NVIDIA. Errado. Escrever `style/128` ali (0 para Model A) zerou a saída do kernel de pós;
  B e C ficaram a 1/4 e 1/2 do efeito. Foi isso do build 22:24 até o 00:04.

Conclusão: **neste runtime a rede não tem entrada de estilo.** Model A/B/C na AMD é só o grade,
como o `docs/styles-model-abc.md` original dizia. O bloco "not portable, closed" do overlay estava
certo e foi apagado ontem por engano; o texto do combo, do ini e dos docs foi corrigido de novo.

O que o build 00:45 (600 576 bytes) faz:
1. Volta a escrever `EngineScale` (default 1/32) em `97b3c`. `StyleControl()` foi removido.
2. `LoadSettings` recusa `EngineScale < 1e-4` (usa 1/32 e avisa); o slider "Output Scale (97b3c)"
   não desce de 0,0001 e fica vermelho fora do default.
3. **Watchdog**: a medição do resíduo roda uma vez completa (frame 240) e depois em silêncio a cada
   1800 frames. Se `resíduo médio < entrada média / 1000`, loga `WARNING: the network is returning
   its input unchanged (...)` com o checklist (97b3c, intensity, structure, log do runtime) e a
   linha de status do overlay fica vermelha: "...but the network is returning its input
   unchanged". Quando volta ao normal, loga que voltou. Isso cobre qualquer causa deste sintoma,
   não só esta.
4. `ControlsChanged` compara o output scale no lugar do "style control"; trocar Model não zera
   history (é só grade, não precisa).

Instalado no ETS2 e no PCSX2. Validar: abrir o ETS2, esperar a linha `measure, residual` (frame
240) e conferir que a média voltou à casa de 0,01–0,03, e que ligado/desligado difere.
