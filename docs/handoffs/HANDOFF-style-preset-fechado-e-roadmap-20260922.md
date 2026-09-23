# HANDOFF — 22/09/2026 (madrugada): NR Style e NR Preset fechados, o erro do "quinto slot", paridade com a NVIDIA, e o roadmap

Este é o handoff de continuidade. Ele consolida o que a sessão de 21→22/09 descobriu, o que
mudou no addon, o que foi provado errado no meio do caminho, e o que vem depois. Os handoffs
anteriores continuam valendo como registro; onde este contradiz um deles, **este vence**.

Leia a §2 antes de qualquer coisa: é o erro que custou um dia e o motivo de o addon ter um
watchdog agora.

---

## 1. Onde está tudo

| | |
|---|---|
| Repo | `C:\Users\claudinhh\Desktop\dlss5amdrework\repo`, branch **`feed`** sobre `73d152c` |
| Estado do fonte | **nada commitado.** Modificados: `src/neural/neural.cpp`, `vk_route.inc`, `gl_route.inc`, `shaders/DLSS5_Neural_Feed.fx`, `docs/styles-model-abc.md`, `handoffs/README.md`. Novos: `docs/nvidia-parity.md`, 6 handoffs, `tools/ngx_param_trace.js` (versão que funciona), `tools/ngx_run_trace.py`, `tools/sass_window.py`, `tools/style_compare.py`. Ordem de commit na §8. |
| Build atual | `repo\build\dlss5-neural.addon64`, **600 576 bytes, 22/09 00:45**, sha256 `4ac31222...` |
| Instalado em | ETS2 `bin\win_x64` (D3D11, Launchpad + feed) e bancada PCSX2 `D:\pcsx2-v2.8.2-test`. **RDR1 continua com build anterior (594 944 bytes).** |
| Validado pelo usuário | ETS2: o efeito voltou a mudar a imagem com o build 00:45; Model A/B/C muda "mais cor" — que é o comportamento correto neste runtime (§3). |
| Pacote para outro dev | `Desktop\dlss5amdrework\dlss5-neural-amd-teste-20260921.zip` — **desatualizado** (anterior a tudo isto). |
| Logs da NVIDIA | `Desktop\nvidia-preset-style-ets2-20260921.zip` (o `.7z` de mesmo nome é a versão anterior, sem §5.5–5.9 das notas). Notas do operador copiadas em `handoffs/RESULTADO-nvidia-preset-style-ets2-20260921.md`, com "Resposta do lado AMD" no fim. |
| DLL NVIDIA analisada | `Desktop\a\nvngx_dlssnr.dll` (sha `e16bcf15...`); a da máquina NVIDIA é a patcheada pré-RTX-50 (sha `E67DEE20...`, mesmo tamanho, mesma tabela de estilos e mesmo `Evaluate`). |
| Runtime AMD analisado | `D:\pcsx2-v2.8.2-test\dlssnr_amd_pass1.dll` v0.3.0; IDB ao lado. Endereços desta sessão: worker `sub_180018670`, evaluate `sub_18002D2D0`, objeto do motor `0x180096F78`, leitor do ini `sub_180007B90`. |
| Referência pública da composição NVIDIA | fork `y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG`, branch `dlss-neural-rendering`, `OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl`. O README dele diz que a composição é a do RenoDX reimplementada. O fonte do RenoDX DLSS 5 **não é público** (o repo `clshortfuse/renodx` no `main` não tem nada de neural rendering). |

---

## 2. O erro central desta sessão, e a correção

### 2.1 O que foi afirmado (21/09, §3 do handoff anterior)
Que `97b3c` ("Scale" no ini do runtime, default 1/32) era o quinto controle da rede, o `style/128`
da NVIDIA, e que o addon rodava a rede num "style 4" desde sempre. O addon passou a escrever
`style/128` ali (build 22:24), o bloco "Model A/B/C: not portable. Closed" do overlay foi apagado,
e docs/ini/tooltips foram reescritos dizendo que o Model entrava na rede.

### 2.2 O que aconteceu
Usuário, ETS2, build 00:04: "FPS muda mas nada muda na tela; ligado = desligado; Model e pass
count não fazem nada; log diz processed, 0 skipped". Log do addon:

```
measure, network input: mean absolute 0.454943
measure, residual at 1920x1080: mean 0.000240, max 0.009766
```

Resíduo 100x abaixo do normal (God of War: 0,021). A rede devolvia a entrada.

### 2.3 A causa, lida no IDA (`dlssnr_amd_pass1.dll`)
- O worker (`0x180019070`–`0x1800190E5`) monta **quatro** floats de controle em `0x96F98..0x96FA4`:
  tone, structure cru, skinEff, structEff. O próprio runtime loga `ctl (%.2f %.2f %.2f %.2f)`.
- `0x96FA8` (objeto+48) recebe `97b3c`. Não é controle. No evaluate (`sub_18002D2D0`), objeto+32
  e +40 vão para o kernel **pre** (`off_18006BC68` modo 20, o que alimenta a rede); objeto+48 vai
  para o kernel **post** (`off_18006BC68` modo 32, o que escreve `out`), como argumento logo
  depois do ponteiro de history. O default do construtor (`sub_18001EB80`) é `0x3D000000` = 1/32
  em +48 e `0x3D800000` = 1/16 em +52.
- Escrever 0 (Model A) ali zera a contribuição da rede na saída. B (1/128) e C (2/128) ficavam a
  1/4 e 1/2 do efeito. **Exatamente o sintoma.**
- A inferência de ontem veio de dois fatos verdadeiros mal combinados: os cinco floats são
  contíguos na memória, e o forward da NVIDIA recebe cinco valores. Adjacência não é semântica.

### 2.4 O que o build 00:45 faz a respeito
1. `97b3c` volta a receber `EngineScale` (default 1/32). `StyleControl()` foi removido.
2. `LoadSettings` recusa `EngineScale < 1e-4` (usa 1/32 e loga WARNING). O slider virou
   **"Output Scale (97b3c)"**, mínimo 0,0001, vermelho fora do default, tag MEASURED.
3. **Watchdog** (`g.inert`): a medição do resíduo roda completa no frame 240 e depois em silêncio
   a cada 1800 frames. Se `resíduo médio < entrada média / 1000`, loga
   `WARNING: the network is returning its input unchanged (...)` com o checklist (97b3c,
   intensity, structure, log do runtime) e a linha de status do overlay fica **vermelha**. Quando
   volta, loga que voltou. Cobre qualquer causa deste sintoma, não só esta.
4. `ControlsChanged` compara o output scale no lugar do "style control".
5. Combo do Model, ini, `docs/styles-model-abc.md` e `docs/nvidia-parity.md` reescritos dizendo a
   verdade (§3).

### 2.5 Lição para o método
Uma leitura de IDA que muda o comportamento do addon **precisa de uma medição antes de virar
texto de overlay e doc.** O addon já tinha a linha `measure, residual`; bastava ter olhado. Agora
ele olha sozinho.

---

## 3. Estado final do NR Style e do NR Preset

### 3.1 NR Style (Model A/B/C)
| | NVIDIA (medido, RTX 3050, ETS2, RenoDX) | AMD (este runtime) |
|---|---|---|
| Entrada da rede | `style/128` no forward; `n = 3` estilos; clamp unsigned a 2 | **não existe.** A rede recebe 4 controles e nada mais |
| Grade no quadro pronto | style 1: −0,10 EV, −0,25 contraste, −0,10 sat; style 2: −0,15 sat; escalado por `clamp(LocalTone,0,1)`; operador verificado instrução a instrução no SASS sm_86 | **igual**, no `kComposeShader` (`NeuralStyle()`), agora escalado por Tone também |
| Reset de history na troca | sim | não precisa (é só grade); os outros 4 controles zeram, como lá |
| O que se vê | muda iluminação/detalhe **e** cor | muda **só cor** — validado pelo usuário: "é mais cor" |

**Encerrado.** Não há como o addon dar o lado "rede" do Model: os kernels HIP do port são GCN
pré-compilado sem fonte, e o controle não existe na assinatura deles. Só quem compilou o port
poderia acrescentar. Não reabrir sem um runtime novo.

### 3.2 NR Preset
**Encerrado dos dois lados.** A DLL da NVIDIA tem uma entrada de pesos (`preset=1`, log
`1 config(s) available`, fallback nunca aparece, `CG2RFindWeightByPreset` percorre tabela de um).
O combo `NRPreset` do Deep Fried Chicken não faz nada nem em placa NVIDIA. Se um dia vier uma DLL
com mais entradas: `tools/extract_runtime.py` por blob + um `.bin` por módulo.

---

## 4. O que os logs da NVIDIA fecharam (resumo; detalhe em `RESULTADO-nvidia-...md` e `docs/nvidia-parity.md`)

- Trocar Model escreve só `DLSSNR.Style` + pulso de `Reset`. Nada mais.
- Defaults do RenoDX: Style 0, Intensity 1, LocalTone 1, LocalStructure 1, Skin 1, AutoMask 1,
  MVecScale 1/1, DepthInverted 0 (default interno da DLL é 1), Preset 1. **Os nossos já eram
  iguais** (o "tone 0" citado em handoffs anteriores era o default do ini do runtime, que o addon
  sobrescreve com 1.0).
- Reset de history: Style, UseAutoMask (exato); LocalTone, LocalStructure, Skin, skinEff, structEff
  (`|Δ| > 1e-5`). Não em Intensity. → `ControlsChanged()` no addon.
- Derivação com AutoMask off: skinEff = structEff = −1. O worker AMD faz igual
  (`0x18006ABAC = 0xBF800000`).
- O "exposure" do forward não existe: é o ponteiro do `ControlMask`, nulo no RenoDX.
- Struct reinicializada aos neutros a cada evaluate; style 0 é identidade de verdade.
- `GlobalToneStrength` não é lido por esta DLL. `UICorrection`: RenoDX manda 1, DLL default 0, AMD
  não tem o campo — aberto dos dois lados, impacto desconhecido.
- Achado colateral no worker AMD: `ToneChannels == 0` zera tone e structure antes da rede. O
  addon sempre escreve o bit 4, então nunca acontece. Comentado no ponto da escrita.

---

## 5. Paridade da composição (o que "igual à NVIDIA" significa agora)

Na NVIDIA a tela **também é uma composição** (RenoDX/fork): razão de luminância em dois ramos,
OkLab, guarda 2.0 dos dois lados, blend de cor. Com a rede vendo o próprio frame, os dois ramos
viram razão 1 e o OkLab vira identidade; o que sobra é o caminho de razão do nosso compose.

Mudanças desta sessão para fechar as diferenças que restavam:
- **Intensity > 1**: eleva a razão a `1 + (s−1)` em vez de esticar o resíduo (como o RenoDX).
- **Network Output** aplica o grade do Model (dbg 6 no compose).
- **`DepthInverted`** virou chave de ini + checkbox sob Depth (default 1).
- **Reset de history** nos mesmos controles da DLL (§4).
- **Grade escalado por Tone** como a DLL.

Diferenças que ficam, de propósito (cada uma com a chave que desliga, em `docs/nvidia-parity.md`):
`ResidualLimit=0.25` e `EdgeFade` (nossos, contra blow-up de tile do port), bicúbico no upsample
(fork é bilinear), proxy HDR (fork: SoftKnee+sRGB; nós: linear×k), e **o Model não entrar na
rede** (§3). Receita para reproduzir a configuração NVIDIA do ETS2 só para comparar:
`Motion=0 Depth=0 Temporal=2`.

**Ninguém mediu a mesma cena nas duas máquinas.** A aritmética é igual; se a imagem é, só a
comparação diz.

---

## 6. Outras mudanças no addon desde `73d152c` (todas sem commit)

Da sessão de 21/09 (validadas no PCSX2 pelo usuário):
1. ReShade chain roda antes da rede quando o feed está ligado (`RenderEffectsAheadOfNetwork`).
2. Um módulo do runtime por passe (`dlss5-pass2.dll`, `pass3.dll`).
3. `PS_History` guarda o vetor cru; vista de debug de movimento satura em 8 px.

Desta sessão (validadas no ETS2 só no que o usuário reportou: efeito voltou, Model muda cor):
4. Reset de history por `ControlsChanged`, grade × Tone, Intensity > 1 por potência, Network
   Output com grade, `DepthInverted`, watchdog de rede inerte, output scale protegido.
5. Textos: combo do Model, ini, "Output Scale (97b3c)", tooltip de Local Tone (TRACED, não mais
   "inerte" sem contexto), Intensity.

---

## 7. Roadmap

Em ordem de impacto na imagem, com o que cada item exige:

1. **Medir o build 00:45 no ETS2 de forma completa.** Confirmar no log: `measure, residual` na
   casa de 0,01–0,03; nenhuma linha `WARNING: the network is returning its input`; status verde.
   Trocar Intensity 0 ↔ 1 e ver diferença. *Custa 5 minutos. Sem isto nada abaixo vale.*
2. **Comparação A/B com a máquina NVIDIA na mesma cena.** `tools/capture_pair.py` aqui; do lado de
   lá, capturas com Model A e os defaults. Medir resíduo, não olhar foto. É o único jeito de
   responder "está igual".
3. **RDR1 (D3D12)**: instalar o build 00:45 e rodar; as rotas D3D12/Vulkan com cópias do runtime
   por passe nunca foram exercitadas.
4. **`DepthInverted` num jogo com depth real**: 0 vs 1, olhar o resíduo. ETS2 serve (depth real,
   3,97% da faixa, escala 25,2x).
5. **Depth normalise** no PS2 continua sem medição limpa (três sinais contra).
6. **`UICorrection`**: descobrir o que a DLL faz com ele (RenoDX manda 1) e se o port tem
   equivalente escondido.
7. **Pacote para o outro dev**: regerar com o build 00:45 e estes handoffs
   (`tools/package-release.ps1 -Private`).
8. **Bridge 32-bit**: não reconstruída; não ganha nada disto.

Fora de alcance (não gastar tempo): estilo entrando na rede na AMD (§3.1); NR Preset (§3.2);
ângulo de câmera; qualquer coisa que exija recompilar os kernels HIP.

---

## 8. Commits sugeridos (uma linha, sem corpo, sem atribuição), nesta ordem

1. ReShade chain runs before the network when the feed effect is on
2. One runtime module per pass, so each pass keeps its own temporal state
3. Keep the provider's raw vector in the feed history, not the validated one
4. Motion debug view saturates at 8 px
5. Reset the history on the controls the NVIDIA DLL resets on, and scale the grade by tone
6. Intensity above one amplifies the luminance ratio instead of the residual
7. Network output mode carries the model's grade
8. DepthInverted is an ini key
9. Refuse a zero output scale and warn when the network returns its input
10. Model A/B/C is the grade only on this runtime; docs, ini and overlay say so
11. Handoffs, the NVIDIA measurement, the NGX trace and the SASS helper

Antes de tudo: `python tools\runtime_offsets_check.py`, `python tools\feed_fx_check.py`,
`python tools\style_check.py`, `python tools\compose_check.py` (todos PASS em 00:45).

---

## 9. Armadilhas confirmadas nesta sessão

- **Adjacência não é semântica.** Cinco floats contíguos no objeto do motor: quatro são controle,
  o quinto é do kernel de pós. Sempre seguir o consumidor, não o produtor.
- **Uma leitura de IDA que muda comportamento precisa de medição antes de virar doc.** A linha
  `measure, residual` já existia; agora é automática (watchdog).
- O `.7z` e o `.zip` da NVIDIA têm o mesmo nome; o `.zip` (00:11) é o atualizado.
- `git clone --filter=blob:none` + `sparse-checkout` falhou ao buscar blobs; `curl` no
  `raw.githubusercontent.com` resolveu.
- A sessão do IDA MCP expira (TTL); reabrir com `idle_ttl_sec` maior.
- Heredoc Bash com aspas simples dentro funciona quando o delimitador é `'EOF'`; o que quebra é
  heredoc sem aspas.
- Reinstalar o addon exige o jogo fechado (DLL carregada).
- Nunca tirar conclusão de screenshot; o usuário rejeita prints como evidência, com razão.
