# HANDOFF — NR Preset e NR Style: capturar os PARÂMETROS que a DLL recebe numa placa NVIDIA (ETS2, RenoDX), 21/09/2026

Para quem vai operar (pessoa ou IA) uma máquina NVIDIA. O objetivo **não é comparar imagem**: é
registrar, de forma determinística, **quais parâmetros o mod da NVIDIA entrega à `nvngx_dlssnr.dll`
em cada Model (A/B/C) e em cada Preset**, para reproduzir o mesmo na rota AMD. Screenshots são
opcionais e secundários.

Jogo: **Euro Truck Simulator 2** (Steam), **D3D11**, executável `bin\win_x64\eurotrucks2.exe`.
Método: **RenoDX** (`renodx-dlss5.addon64` sobre ReShade 6.8 with add-on support). O operador
informa a pasta do jogo.

Sessões anteriores: `HANDOFF-launchpad-multipass-20260921.md` (estado do addon AMD). Leitura do
kernel: `docs/styles-model-abc.md` (incompleta; ver §2.3).

---

## 1. Onde está tudo

| | |
|---|---|
| Repo do addon AMD | `C:\Users\claudinhh\Desktop\dlss5amdrework\repo`, branch `feed` (diff não commitado) |
| Este handoff, script de trace, script de comparação | `C:\Users\claudinhh\Desktop\HANDOFF-nvidia-preset-style-ets2-20260921.md`, `Desktop\ngx_param_trace.js`, `Desktop\style_compare.py` (originais em `repo\handoffs\`, `repo\tools\`) |
| ETS2 nesta máquina (AMD) | `...\Euro Truck Simulator 2\bin\win_x64` — nosso mod instalado (build 22:24, 595 456 bytes), rota D3D11 com Launchpad, validado em jogo (§6) |
| DLL da NVIDIA | `C:\Users\claudinhh\Desktop\a\nvngx_dlssnr.dll`, 165 840 496 bytes, sha256 `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`, build `rel_310_8` |
| Runtime AMD | `dlssnr_amd_pass1.dll` v0.3.0, sha256 `70af3fb7...` |

---

## 2. Contexto: o que a DLL faz com cada parâmetro (IDA, endereços da 310.8)

### 2.1 Parâmetros NGX que a DLL lê

Na criação (`NGXCG2R::CreateFeature`, `cg2r.cpp:855`): `DLSSNR.Width`, `DLSSNR.Height`,
`DLSSNR.ScalingRatio`, `DLSSNR.Hint.Render.Preset`, `CreationNodeMask`, `VisibilityNodeMask`.

A cada evaluate (`sub_180019F30`, struct de opções `opts`): recursos `DLSSNR.Color`, `.MVec`,
`.Depth`, `.Output`, `.ControlMask`, `.UI`, `.UIAlpha`, `.Backbuffer`,
`.BidirectionalDistortionField` (cada um com `SubrectBaseX/Y/Width/Height`); escalares
`DLSSNR.MVecScaleX/Y`, `.Intensity` (+224), `.LocalToneStrength` (+228, `opts[57]`),
`.LocalStructureStrength` (+232, `opts[58]`), `.Style` (+236, `opts[59]`), `.UseAutoMask` (+240),
`.SkinStructureStrength` (+244, default −1), `.Reset` (+256), `.DepthInverted` (+260, default 1),
`.Enabled` (+264), `.UICorrection` (+268), `DLSS.Indicator.Invert.X/Y.Axis`.

Derivados: se `UseAutoMask` e `Skin < 0`, `Skin = LocalStructure`; um `ControlMask` presente força
`UseAutoMask = 0`.

### 2.2 Preset

`CreateFeature` → `CreateNetwork` → `CG2RFindWeightByPreset(n)`: tabela em `0x1800B0D80` com
**uma** entrada de 648 bytes (`id 1`, `WEIGHTS_HT`, `CC_SILVER_AARDWOLD`). `n != 1` → fallback e a
linha `DLSSNR: preset %d is not available in this DLL build; falling back to shipping default
preset 1`. Sempre loga `DLSSNR: %zu config(s) available:` + `[%zu] %s (backbone: %s)` e
`DLSSNR: Created feature %u (output %ux%u, network %ux%u, preset=%d -> %s)`. O `%s` é o descritor
escolhido; nesta build não pode variar. O valor do preset não é usado em mais nada.

### 2.3 Style — entra na REDE, não só no grading (correção do handoff anterior)

`CG2RNetworkManager::Evaluate` (`sub_180021BB0`) faz:

```
n     = número de estilos do descritor da rede (entrada de 240 bytes, +100)
style = clamp(opts[59], 0, n-1)
CCNetwork::forward(..., LocalTone, LocalStructure, style * 0.0078125, exposure, skinEff, structEff, ...)
```

Ou seja, o style é um **controle de entrada da rede**, no valor `style/128`. Além disso a DLL
aplica o vetor de grading (exposição −0,10 EV, contraste −0,25, saturação ×0,90 para style 1;
saturação ×0,85 para style 2) no kernel de pós-processo, escalado por `LocalToneStrength`. E
`CG2R_ResetTemporalHistoryOnControlChange` (`sub_1800179D0`) **zera o history** quando muda
qualquer um de: Style, UseAutoMask, LocalTone, LocalStructure, Skin, e loga
`DLSSNR: reset temporal history for <config> after control change`.

### 2.4 O runtime AMD tem a mesma entrada, e estava fixa no valor errado

No worker do `dlssnr_amd_pass1.dll` (`sub_180018670`, `0x180019070`–`0x1800190E5`) o vetor de
controle é montado de `0x97B30` (tone), `0x97B34` (structure), `0x97B38` (skin), e o quinto valor
em `0x96FA8` vem de **`0x97B3C`** — o campo que o addon chamava de `EngineScale`, default 0,03125
= **4/128**. O addon nunca escreveu style aí. Desde 22:24 de 21/09 ele escreve `style/128`
(0 / 0,0078125 / 0,015625), zera o history na troca, e loga:
`menu: style N -> network control 0.0078125 (style/128; EngineScale 0.031250), history reset`.
`EngineScale` fora de 1/32 vira override explícito.

**O que ainda não se sabe e a NVIDIA responde:** (a) o RenoDX escreve só `DLSSNR.Style` ao trocar
o Model, ou muda também LocalTone/Structure/Skin/Intensity/Preset junto? (b) quais valores de
`LocalToneStrength`, `LocalStructureStrength`, `SkinStructureStrength`, `UseAutoMask`, `Intensity`
o RenoDX usa por padrão? (c) o valor `n` de estilos que a rede aceita (se `reset temporal history`
aparece ao ir de 2 para 3, e se a DLL loga clamp). Tudo isso sai do trace de parâmetros, não de
foto.

---

## 3. Preparar a máquina NVIDIA

1. ETS2 pelo Steam em **DirectX** (`-rdevice dx11` ou "Launch with DirectX"). Pasta de render:
   `bin\win_x64`. SDR (o jogo não tem HDR).
2. ReShade 6.8 with add-on support contra `eurotrucks2.exe`; `renodx-dlss5.addon64` e
   `nvngx_dlssnr.dll` em `bin\win_x64`. Conferir o hash da DLL (`Get-FileHash`); se não for
   `e16bcf15...`, guardar cópia e anotar.
3. Painel do RenoDX (`Home` → Add-ons → DLSS 5 Neural Rendering): `DLSSNR v310.8.0: RUNNING`,
   `Successful NR frames` subindo. **Anotar o nome de cada controle e seu valor padrão** (modelo,
   Local Tone, Local Structure, Skin, Intensity, preset se houver, Enable Upscaling).
4. **Log do NGX** (as linhas `DLSSNR:`), registro:
   ```
   HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore   LogLevel=2 (DWORD)   EnableConsoleLogging=1 (DWORD, opcional)
   ```
   Depois da rodada, achar o arquivo:
   ```powershell
   $since=(Get-Date).AddHours(-2)
   Get-ChildItem "<jogo>\bin\win_x64","$env:LOCALAPPDATA\NVIDIA","$env:PROGRAMDATA\NVIDIA","$env:TEMP" -Recurse -Include *.log,*.txt -EA SilentlyContinue |
     ? { $_.LastWriteTime -gt $since -and (Select-String -Path $_.FullName -Pattern 'DLSSNR:' -Quiet) } | select FullName,Length
   ```
5. **Trace de parâmetros (o instrumento principal):** Python 3 + `pip install frida-tools`. Com o
   jogo no menu principal (o NGX já carregado):
   ```
   frida -n eurotrucks2.exe -l ngx_param_trace.js -o ngx_params.log
   ```
   O script engancha `NVSDK_NGX_Parameter_SetUI/SetI/SetF/SetD/SetULL` e `Create/Evaluate/Release
   Feature` em `_nvngx.dll`/`nvngx.dll` e imprime `hora  tipo  nome = valor` para todo parâmetro
   `DLSSNR.*`, só quando o valor muda. Se `frida` não conseguir anexar (anti-cheat não há no ETS2;
   pode ser permissão), rodar o terminal como administrador. Alternativa sem Frida: **API Monitor**
   (rohitab.com) filtrando `_nvngx.dll` → `NVSDK_NGX_Parameter_Set*`, exportar para texto.

---

## 4. Protocolo (parâmetros primeiro, fotos depois)

Tudo com o trace rodando e o log do NGX ligado. Entre passos, esperar 5 s. Anotar a hora de cada
ação para casar com o trace.

| # | ação | o que tem de aparecer |
|---|---|---|
| P0 | abrir o jogo, entrar em cena, NR RUNNING | NGX log: `config(s) available`, `[0] ... (backbone: ...)`, `Created feature ... preset=N -> <nome>`; trace: o bloco inicial de `DLSSNR.*` (Width, Height, Preset, e o primeiro evaluate com Intensity, LocalTone, LocalStructure, Skin, UseAutoMask, Style, DepthInverted, MVecScale) |
| S1 | Model A → Model B | trace: `DLSSNR.Style = 1` **e qualquer outro parâmetro que mude junto**; NGX log: `reset temporal history for ... after control change` |
| S2 | Model B → Model C | idem com `Style = 2` |
| S3 | Model C → Model A | idem com `Style = 0` |
| S4 | mexer Local Tone Strength 1.0 → 0.5 → 1.0 | trace: `DLSSNR.LocalToneStrength = 0.5`; NGX log: reset por control change |
| S5 | mexer Intensity ±0.2 e voltar | trace: `DLSSNR.Intensity` (confirma que não é controle: sem reset no NGX log) |
| PR | se existir controle de preset: cada valor, com "Reset NR feature" | trace: `DLSSNR.Hint.Render.Preset = N`; NGX log: `Created feature ... preset=N -> <nome>` e a linha de fallback |
| F | (opcional) Photo Mode, PrtScr em A, B, C, A | fotos para `style_compare.py`; só valem se P0..S3 já estiverem no trace |

Repetir S1–S3 uma segunda vez. Se o painel do RenoDX tiver mais controles (character mask, presets
de qualidade), mexer em cada um uma vez com o trace ligado: é barato e vira dicionário.

---

## 5. Análise e entrega

1. **Tabela de parâmetros por Model**, montada do trace: para A, B e C, o valor de cada
   `DLSSNR.*` escalar. A pergunta a responder em uma linha: *o RenoDX muda só `Style`, ou muda mais
   coisa junto?* Isso decide o que a AMD tem de copiar.
2. **Defaults do RenoDX**: LocalTone, LocalStructure, Skin, UseAutoMask, Intensity, MVecScaleX/Y,
   DepthInverted, Reset. Hoje a AMD usa tone 0, structure 1, skin −1, UseAutoMask conforme o
   overlay, Scale=style/128. Diferenças aqui explicam diferença de imagem tanto quanto o style.
3. **Preset**: contagem de `config(s) available` e o `<nome>` após `->`. Um config e o mesmo nome
   = preset sem conteúdo nesta build; nomes diferentes = DLL diferente, mandar a DLL.
4. **Reset de history**: em quais trocas o NGX log imprime `reset temporal history`. A AMD hoje
   zera no Style; se a NVIDIA zera também em Local Tone/Structure/Skin/AutoMask, replicar.
5. Se houve fotos: `style_compare.py --off A.png --on B.png --style 1 --floor A2.png` mede só a
   parte de grading; com o style entrando na rede, `on vs pred` **não** vai ao piso, e isso é
   esperado agora. O número interessa só como ordem de grandeza.

Trazer: `ngx_params.log`, o log do NGX, `ReShade.log`, `ReShade.ini`, ini do addon RenoDX, hash da
DLL, anotações dos controles e defaults, e as fotos se houver.

---

## 6. Estado nesta máquina (AMD)

- **ETS2**, `bin\win_x64`: ReShade 6.8.0.2155 (`dxgi.dll`), addon 22:24 (595 456 bytes), runtime
  `70af3fb7`, pesos `6bf8dc93`, `DLSS5_Neural_Feed.fx`, Launchpad + `mmx_*.fxh` + bluenoise,
  preset com Launchpad antes do feed e `OPTICAL_FLOW_Q=2`, screenshots em `dlss5-shots\`. Validado
  em jogo antes da correção do style: `effects: DLSS5_Neural_Feed is on...`, `motion from the
  effect, depth from the game`, `guide probe, motion (from the effect): 0% exactly still`. A
  correção do style (§2.4) ainda **não foi vista em jogo**: na próxima rodada, trocar Model A/B/C
  e conferir a linha `menu: style N -> network control ...`; a diferença visual agora deve ser da
  mesma natureza que a da NVIDIA, não só cor.
- Log AMD a guardar por rodada: `dlss5-neural.log` (controles: linhas `settings:`, `resolved tuning
  per pass`, `menu: style`), `dlssnr_on_amd.log`.
- **PCSX2** (bancada) recebeu o mesmo addon. **RDR1** (D3D12) tem o build anterior instalado.

## 7. Armadilhas

- `EngineScale` no ini/overlay diferente de 0,03125 desliga o style na rede (vira override). Deixar
  no padrão para o Model mandar.
- Trocar Model e olhar em seguida: history zerado, 1–2 s de ruído. Esperar.
- Log do NGX ≠ ReShade.log. Sem o registro da §3.4 as linhas `DLSSNR:` não existem.
- Frida precisa anexar depois de `_nvngx.dll` carregar; o script espera por ela, mas anexar cedo
  demais em alguns jogos mata o processo. Anexar no menu principal.
- OpenGL no ETS2: nada carrega. `\**\**` no ReShade.ini: nenhum shader carrega (erro 123).
