# dlss5-neural-amd — handoff

Sessão de 09/09/2026. Base: `ac6d8cb`. **Nada foi commitado** — tudo está na working tree.

```
 M README.md
 M src/neural/neural.cpp      1452 inserções, 521 remoções
 ?? tools/check_shaders.ps1
```

Tudo abaixo de "medido" veio de execução real do God of War 2 a partir do save state, não de
leitura de código. O que está em "não verificado" está marcado como tal.

---

## 1. A pergunta que abriu a sessão, e a resposta

**"Só muda cor e exposição, não parece DLSS 5."**

Estava parcialmente errada, e a parte errada é mensurável. A rede produz correção com
estrutura — mas seis bugs a estavam sabotando no caminho até a tela, e o teto real é baixo por
falta de entradas, não por ajuste.

### Por que o GTA V funciona e o PCSX2 não — resolvido

O `DLSS-NR-on-AMD` (mesmo runtime, `dlssnr_on_amd`) exige no próprio README:

> *"Windows 11, **DirectX 12 game with FSR**"* … *"Start the game and **enable FSR**"*

Ele **intercepta a chamada do FSR do jogo**. FSR é upscaler temporal, então o jogo é obrigado a
entregar Color, **Depth**, MotionVectors e jitter para ele. O add-on pega tudo do contrato que já
existe — é o `UseFsrInputs=1` do `dlssnr_on_amd.ini`.

GTA V Enhanced tem FSR. Cyberpunk tem FSR. **O PCSX2 não tem FSR, nem DLSS, nem upscaler
temporal.** Não existe chamada carregando depth para interceptar. Não é bug do add-on: a fonte
não existe nesse alvo.

---

## 2. Bugs encontrados e corrigidos

| # | bug | como se manifestava |
|---|---|---|
| 1 | **Pass Count matava o add-on** | `missing: dlssnr_amd_pass2.dll` → `g.unavailable = true`, que nunca era limpo. Add-on morto pelo resto da execução. |
| 2 | **Upsample do residual era bilinear** | Nessa proporção bilinear é um borrão. Toda alta frequência que a rede produzia era eliminada antes de chegar na tela; sobrava só a parte baixa = **cor e brilho**. |
| 3 | **Downsample para a rede era bilinear** | Lê 4 texels independente de quantos deveria cobrir. Jogava metade da imagem fora e mantinha o serrilhado; a rede gastava capacidade limpando aliasing. |
| 4 | **Combo de debug só mostrava "Off"** | A lista foi escrita no `.cpp` com **bytes NUL de verdade** em vez do escape `\0`. MSVC corta a string no primeiro NUL. |
| 5 | **Modo async zerava a correção** | `netColour` era sobrescrito com entrada nova antes do compose ler. `fix = netColour − netBase = 0`. Alternava entre correto e zero → piscava e sumia na média. |
| 6 | **Resolution Scale era inerte desde o frame 2** | O "pin" do raster comparava só tamanho e não distinguia "janela tremeu" de "usuário mexeu no slider". Pôr 1.00 deixava o raster no 0.50 do boot. |
| 7 | **`Scale=0.50` virava `0.25`** | `wcstof` respeita o separador decimal do locale. Em pt-BR "0.50" parseia como 0 e para no ponto → clamp no piso 0.25. |

Correções: (1) faz fallback para o que carregou; (2) Catmull-Rom com toggle; (3) média de área;
(4) escape correto; (5) textura `netResidual` capturada no ponto em que o par entrada/saída
comprovadamente corresponde; (6) `pinnedScale` separa mudança deliberada de tremida de janela;
(7) `_wcstod_l` no locale C.

---

## 3. O que foi adicionado

- **`dlss5-neural.ini`** ao lado do exe — settings persistem entre execuções. Foi o que
  permitiu rodar bateria de testes headless sem depender do overlay.
- **Medição `residual detail`** — variação local da correção contra a da própria imagem, e a
  razão. Separa "shift de cor/exposição" de "trabalho em estrutura". Perto de 0,000 = liso;
  subindo de 0,100 = a correção segue o detalhe da imagem.
- **Botão "Measure Residual Again"** — re-arma a medição sem reabrir o jogo.
- **Debug View**: Off / Network input / Network output / **Residual x8** / Motion vectors /
  Depth x500.
- **Sliders de flow** (gate de contraste, razão de aceite) — item 2 do plano antigo.
- **Dump dos floats do motor** `0x76e28..0x76e44` — item 3 do plano antigo.
- **`tools/check_shaders.ps1`** — extrai os shaders HLSL do `.cpp` e roda `fxc`. O `build.ps1`
  passar não dizia nada sobre eles; erro de sintaxe só aparecia como linha de log dentro do jogo.

---

## 4. Dados medidos

### Varredura de parâmetros (GoW2, save state, scale 0.50, inline)

| config | input | residual | max | var. correção | ratio |
|---|---|---|---|---|---|
| baseline (Structure=1) | 0,338068 | 0,021457 | 0,199707 | 0,002769 | 0,184 |
| **Structure=3** | 0,338068 | **0,022852** | 0,218262 | **0,004175** | **0,277** |
| Structure=0 | 0,338068 | 0,000851 | 0,042236 | 0,000646 | 0,043 |
| Tone=3 | 0,338068 | 0,021457 | 0,199707 | 0,002769 | 0,184 |
| Skin=−1 (auto) | 0,338068 | 0,021124 | 0,192871 | 0,002738 | 0,182 |
| Passes=3 | 0,338376 | **0,000000** | 0,000000 | 0,000000 | 0,000 |

**Conclusões duras:**

- **A rede funciona.** Structure=0 derruba o residual **25×**. O que chega na tela vem dela.
- **A correção tem estrutura.** Ratio 0,18–0,28, contra ~0,06 que um ganho uniforme
  (exposição/saturação pura) daria. Não é filtro de cor.
- **Tone é inerte.** Números byte a byte idênticos ao baseline. Bate com a nota do documento do
  RenoDX de que Global Tone não é visível no caminho NGX recuperado.
- **Skin mal importa.** 1,5%.
- **Structure é o único controle artístico que move o resultado.** Deixar em 3.
- **Passes > 1 quebra tudo.** Residual exatamente zero. **Deixar em 1.**

### Variáveis de ambiente do runtime

Achadas por strings no `dlssnr_amd_pass1.dll` (lê via `GetEnvironmentVariableA`):
`DLSSNR_NOBLEND`, `DLSSNR_NOPOSTHIST`, `DLSSNR_NO_REPACK`, `DLSSNR_SLOW_PREPOST`,
`DLSSNR_STAGES`, `DLSSNR_WBLOG`. Também o tensor `block70.layer0.blend_scale` e a chave de ini
`UseAutoMask`, que o add-on nunca escreve.

| env | residual | ratio |
|---|---|---|
| nenhuma | 0,022559 | 0,271 |
| `DLSSNR_NOBLEND=1` | 0,022962 | 0,297 |
| `DLSSNR_NOPOSTHIST=1` | 0,022597 | 0,287 |
| `DLSSNR_NO_REPACK=1` | 0,022376 | 0,275 |

Nenhuma abre o efeito. Movem 1–2%. `DLSSNR_STAGES` e `UseAutoMask` **não foram testados**.

### O que o motor reporta

```
staging ready: colour 959x504 dxgi 10; motion dxgi 34; depth dxgi 41;
               exposure no; residual on
history on   (51 de 52 jobs)
depth off
network job done in 15-16 ms   (scale 0.50)
```

Floats do motor logo após init (padrões dele, antes de escrevermos):
```
[76e28]=0,000  [76e2c]=0,000  [76e30]=0,000(tone)  [76e34]=1,000(structure)
[76e38]=-1,000(skin)  [76e3c]=0,031  [76e40]=0,000  [76e44]=0,000
```
O **−1,000 em skin é sentinela** — só faz sentido se alguém lê e interpreta como "automático".
É a evidência mais forte de que esse offset é lido. O add-on sobrescrevia com 1.0, desligando o
automático, antes mesmo de tocar no slider. `[76e3c]=0,031` é valor vivo que nunca escrevemos.

### O teto, e por quê

Residual ~6,4% do sinal (0,0215 contra 0,338). Satura: Structure 1→3 dá +6% de magnitude.

As quatro entradas do `Packet`, neste alvo:

| entrada | estado |
|---|---|
| cor | ✅ |
| motion | estimada da imagem, e **morta na maioria das cenas** (`99% of blocks still`) |
| depth | **ausente no D3D12** |
| exposure | **nunca preenchida** (`exposure no`) |

O RenoDX na NVIDIA alimenta `DLSSNR.Depth`, `DLSSNR.MVec`, `DLSSNR.UI`, `DLSSNR.UIAlpha`. Três
delas não existem aqui. Rede de aparência com só cor produz correção modesta e coerente — que é
exatamente o medido. **Não é ajuste errado, é entrada faltando.**

---

## 5. Depth — o achado decisivo

Mesmo jogo, mesmo add-on, mesma sessão, só trocando o renderer:

| API | eventos de depth |
|---|---|
| **D3D12** | **0** em 600 frames |
| **D3D11** | `depth seen (D3D11): 2048x1792 format 19 samples 1` — **todo frame** |

2048×1792 é a resolução interna do PCSX2 com `upscale_multiplier = 4` (512×448 × 4). É o depth
real da cena.

**Hipótese testada e rejeitada:** o `probe.cpp` registra `draw`/`draw_indexed` e o `neural.cpp`
não (as funções `OnDraw`/`OnDrawIndexed` existiam e nunca eram registradas). Registrei. **No
D3D12 continuou zero.** Não era o evento — é a API.

O `dlss5-probe.log` que já estava na pasta havia capturado depth: `1536x1254 R32G8X24_TYPELESS`,
41 draws, dumpado para `.ppm`. Aquela execução era D3D11.

Também adicionado (mas **sem valor comprovado**, porque o D3D12 não entrega bind nenhum): hook de
`clear_depth_stencil_view` que copia o depth **antes do PCSX2 limpar** — o plano antigo registra
que lido no present ele vem uniformemente zero.

---

## 6. A ponte D3D11 — passos 1 a 3 prontos e verificados

| passo | estado |
|---|---|
| 1. Device de trabalho na LUID do jogo | ✅ `LUID game 00000000:00013CE5, ours 00000000:00013CE5 -> same adapter` |
| 2. Rede no device novo, cor atravessando | ✅ |
| 3. Travessia de volta | ✅ |
| 4. Cor da resolução de render | ⬜ |
| 5. **Depth** | ⬜ **próximo** |

### Verificação do passo 3

| | input | residual | ratio |
|---|---|---|---|
| D3D12 (baseline) | 0,338068 | 0,0202–0,0229 | 0,271–0,289 |
| **D3D11 ponte** | **0,338002** | **0,020782** | **0,278** |

Idênticos dentro da variação entre execuções. **O transporte está correto.**

Performance: 3240 frames, **14 pulados (0,4%)**. Melhor que o D3D12.

### Arquitetura implementada

```
D3D11:  CopyResource(bridgeIn.on11, back buffer)  →  Signal(crossFence)
D3D12:  Wait(crossFence)
        CopyResource(crossLocal, bridgeIn.on12)
        RecordNetwork(...)              ← pipeline inteiro, sem mudança
        CopyResource(bridgeOut.on12, composed)
        Signal(backFence)
D3D11:  Wait(backFence)  →  CopyResource(back buffer, bridgeOut.on11)
```

As duas travessias são `CopyResource` puro. Recurso compartilhado vindo de outro device fica em
COMMON, que promove sozinho para COPY_SOURCE/COPY_DEST em fila direta — **não há barreira para
errar**. O compose no lado D3D11 do `session.cpp` desapareceu: o D3D12 entrega a imagem pronta.

### Refactors que viabilizaram

- **`RecordNetwork(cmd, colourSrc, colourFmt, outTarget, runNetwork, wanted)`** — as ~500 linhas
  do miolo do `OnPresent` viraram função que grava em qualquer command list. O caminho D3D12
  passa a back buffer, a ponte passa texturas compartilhadas. Nada lá dentro sabe qual é.
- **`DrainReadbacks(nw, nh)`** — a leitura do flow probe e da medição estava inline no
  `OnPresent`, então a ponte gravava as cópias e **jogava os números fora**. Os dois caminhos
  chamam agora.
- **`g.device = g.workDevice`** — uma atribuição move o pipeline inteiro. Tudo abaixo
  (`InitPipeline`, `EnsureResources`, `InitEngine`, o `Packet`) já rodava em cima do que quer que
  `g.device` fosse. O plano antigo estava certo: *"é menor do que parece"*.

---

## 7. Passo 5 — o que falta, concretamente

O plano registra que **`R32G8X24_TYPELESS` não compartilha** entre os dois devices. Compartilham:
`R32_FLOAT`, `R32_TYPELESS`, `R16_FLOAT`, `R16G16_FLOAT`, `R16G16B16A16_FLOAT`, `R8G8B8A8_UNORM`
(com bind de UAV inclusive).

Então o depth tem que ser **convertido no lado D3D11 antes de atravessar**:

1. `SnapshotDepth` — copiar o depth para textura D3D11 privada no momento do bind
   (`session.cpp` já tem, `game11ctx->CopyResource`)
2. `EnsureDepthPath` — compute shader D3D11 que lê o snapshot como
   `R32_FLOAT_X8X24_TYPELESS` e escreve num `Bridge` `R32_FLOAT` com UAV
3. `ConvertDepth` — dispatch, salvando e restaurando o estado do CS do jogo
4. Atravessar e alimentar como `packet.depth`, ligando `0x76e1f`

**Os três primeiros já existem prontos e medidos em `src/session/session.cpp`** — basta portar,
como foi feito com o `Bridge` e o `CreateOn`. Trechos relevantes: `SnapshotDepth`,
`EnsureDepthPath`, `ConvertDepth`.

Armadilhas já pagas, do plano antigo — não redescobrir:

- Depth do PS2 tem **valor máximo ~0,002**. Visualizador que mapeia para 0–255 mostra preto puro
  e `%.6f` imprime `0.000000`. Escalar antes de julgar. (O Debug View "Depth x500" já faz.)
- **Resize orfana views.** Reconstruir SRV **e** UAV sempre que a textura por baixo for
  recriada, não só quando a view for nula.
- Escolher o alvo de cor **casando o tamanho do depth**, não por área. A swapchain (1918×1008)
  tem *mais* pixels que o alvo de render (1536×1254).

---

## 8. Estado atual da máquina

- **PCSX2 em `D:\pcsx2-v2.8.2-test`**, `Renderer = 3` (**Direct3D 11**, a ponte está ativa).
  Voltar para `Renderer = 15` dá o caminho D3D12 antigo sem depth. Backup: `inis\PCSX2.ini.bak`.
- Logs ligados: `EnableSystemConsole`, `EnableVerbose`, `EnableFileLogging`, `EnableTimestamps`.
- `dlssnr_amd_pass2.dll` e `pass3.dll` são cópias do pass1 (hashes conferem). **Só servem se
  Passes>1 for consertado** — hoje produz zero. Podem ser apagados.
- `dlss5-neural.ini` com `Structure=3`, `Scale=0.50`, `Inline=1`, `Passes=1`, `Depth=1`.
- Logs anteriores arquivados como `prev-*.log` na pasta do PCSX2.

### Como rodar

```powershell
$iso = (Get-ChildItem 'D:\iso ps2' -Filter '*.iso' |
        Where-Object { $_.Name -like 'God of War 2*' } | Select-Object -First 1).FullName
Start-Process 'D:\pcsx2-v2.8.2-test\pcsx2-qt.exe' -ArgumentList @(
  '-portable','-batch','-nogui','-statefile','D:\pcsx2-v2.8.2-test\gow2.p2s',
  '-logfile','D:\pcsx2-v2.8.2-test\emulog.txt','--',$iso)
```

O nome da ISO tem caractere não-ASCII. **Resolver por wildcard**, nunca por literal num `.ps1`:
o Windows PowerShell lê script sem BOM como ANSI e transforma o caminho em lixo. Isso custou uma
execução falhada.

Uma execução curta por vez, 25–30 s, janela (não fullscreen), sempre do save state — com
`-fastboot` metade da execução é logo e menu, onde a imagem é plana e o optical flow não tem o
que casar.

---

## 9. Lista honesta do que não foi resolvido

- **Passes > 1 produz residual zero.** Não investigado.
- **`exposure no`** — um dos quatro slots do `Packet` continua vazio. Não investigado se o motor
  aceitaria.
- **Optical flow morre na maioria das cenas** (`99% still`). Os sliders de gate/ratio existem mas
  não foram calibrados de verdade.
- **`DLSSNR_STAGES` e `UseAutoMask`** — encontrados no binário, nunca testados.
- **Overall Intensity** é mistura pós-rede, não `DLSSNR.Intensity`. Nenhum offset equivalente foi
  localizado neste build.
- **Nada foi commitado.**

## 10. A expectativa, dita sem rodeio

Mesmo com depth funcionando, **não há evidência de que isso chegue no resultado do RenoDX na
NVIDIA**. Depth serve principalmente para reprojeção temporal e detecção de desoclusão — melhora
estabilidade, não afia textura diretamente. O teto de ~6,4% medido vem de a rede rodar só com
cor, e depth cobre uma das três entradas que faltam.

O que depth destrava com certeza: a rede parar de trabalhar às cegas em geometria, e o passo 4
(cor vinda de 1536×1254 ou 2048×1792 em vez do downscale da swapchain), que é o primeiro passo do
plano que deveria melhorar algo visível.

O critério para saber se valeu continua sendo o mesmo número: `measure, residual detail`. Hoje
0,278 com Structure=3.
