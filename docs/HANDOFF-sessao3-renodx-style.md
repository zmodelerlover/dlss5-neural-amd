# dlss5-neural-amd — handoff da sessão 3

Sessão de 10/09/2026. Base: `790d618`. **Nada commitado** — tudo na working tree de `D:\dlss5`.

```
 M build.ps1
 M src/neural/neural.cpp
```

Alvos: **NFS 2015** (D3D11, ponte) e **PCSX2** (D3D11). O add-on compilado foi copiado para os dois.

Tudo marcado como *medido* ou *decompilado* veio de execução real ou de leitura do binário.
O que é inferência está dito assim.

---

## 1. O que esta sessão fez, em uma linha cada

1. Tirou da UI os controles espelhados da NVIDIA que não têm o que fazer aqui.
2. Reescreveu o painel: descrição virou tooltip, agrupado, com cor de risco e legenda.
3. Reescreveu o Pass Count — um módulo só, N gravações. Era ele que travava a máquina.
4. Inglês/Português no próprio painel.
5. Auditou controle por controle se cada um chega em algum consumidor.
6. Investigou os presets Model A/B/C do RenoDX até o fim. **Veredito: impossível nesta rota.**

Os itens 5 e 6 acharam **seis defeitos reais**, listados na §5.

---

## 2. UI — o que saiu, o que entrou

### Saiu (eram `BeginDisabled` com um valor só, espelho da UI da NVIDIA)

`Options Mode` · `Hook Method` · `Require DLSS` · `UI Correction` · `Global Tone Strength` ·
`Model` · `Character Mask` (voltou de verdade, ver §5) · `Jitter` · `Exposure` · `Upscaling Ratio`

### Entrou

- **Tooltip `(?)` em cada controle.** O texto todo — inclusive os números medidos — foi para lá.
  O painel voltou a ser painel em vez de parede de texto cinza.
- **Grupos**: Image / Performance / Guides / Debug / Advanced / Engine / Status.
- **`Timing`** — combo *Same frame (inline)* / *Async (previous frame)*, no lugar do checkbox
  "Apply On Same Frame".
- **Cor por valor atual**, com legenda no rodapé. Vermelho = esse valor já derrubou driver.
  Âmbar = passou do que foi medido. A cor some quando o valor volta. Pega Timing, Resolution
  Scale, Pass Count e History.
- **`Save Settings` / `Reload Settings`.** Isso **não existia**: tudo que se mexia no overlay
  morria ao fechar o jogo, então cada A/B era editar o ini na mão.
- **`Language` / `Idioma`** — English por padrão, salvo no ini. Traduz o painel inteiro,
  tooltips inclusive. Precisou de `/utf-8` no `build.ps1`, senão o MSVC lia o fonte na codepage
  ANSI e os acentos viravam lixo. Verificado: os bytes UTF-8 estão na DLL.
  **Não verificado na tela**: se a fonte do ReShade não tiver os glifos Latin-1, os acentos
  aparecem como quadrado. Se acontecer, é trocar por texto sem acento.
- **`Enabled` começa desligado**, sempre, e não é salvo. Liga com Ctrl+End ou no painel.

---

## 3. Pass Count — a causa do travamento, e a reescrita

### Por que existia uma DLL por passe

Não foi escolha de design. O `dlssnr_amd_pass1.dll` guarda **todo o estado em globais do módulo,
em RVAs fixos** (`0x764c8` device, `0x764d0` queue, `0x764d8` o objeto engine). O Windows
identifica módulo por caminho, então `LoadLibrary` da mesma DLL devolve o mesmo `HMODULE` e os
mesmos globais. Partindo de "cada passe é uma instância independente", a única saída era N
caminhos → `dlssnr_amd_pass1..10.dll`.

**A premissa nunca foi testada.** Os globais que mudam por avaliação são `0x76d68` (última
command list) e `0x76d74` (job id), e os passes são sequenciais na mesma lista.

### Por que 2x travava a máquina

Não era o TDR, que foi o que a sessão 2 assumiu ao pôr `kInlineMaxPasses = 3`. Duas avaliações a
0.50 somam ~32 ms contra um timeout de 2 s. O cap nunca ia resolver.

O que 2 passes realmente faziam:

- **2 `InitEngine` completos = 2× 147 MB de pesos + 2× buffers de ativação em VRAM**, ao lado do
  working set do jogo. Estouro de VRAM em alocação HIP com o jogo residente derruba o desktop
  inteiro. É o candidato mais forte — **inferido do desenho do código, não medido**.
- **O portão de job só olhava o passe 0** (`g.lastJobs[0]`, `runtimes[0]`), então os outros
  podiam estar em voo enquanto o add-on resubmetia por cima.

### O que ficou

- **Um módulo só, gravado N vezes.** Some a exigência de `dlssnr_amd_pass2..10.dll` —
  **pode apagar esses 9 arquivos**.
- O portão de job passou a cobrir todos os passes por construção.
- **Teto de 3** (`kMaxPasses`), default 1. O cap vale no slider, no `clamp` do `LoadSettings`
  e no `WantedPasses` porque os três leem a mesma constante.
- Liberado em inline até 3, com cor: âmbar a partir de 2, vermelho só em inline + >1 passe +
  Scale > 1.00. A trava dura anterior era mais rígida do que o próprio diagnóstico justificava.

### Estado

**Não verificado.** Ninguém rodou `Passes=2` desde a mudança. A causa provável foi removida; o
crash não foi reproduzido nem confirmado ausente. O teste que fecha é rodar a mesma cena com 1 e
com 2 em async e comparar `measure, residual`.

---

## 4. Auditoria da UI — cada controle chega em quem?

**Funciona, com medição por trás:** Enabled · Structure Intensity (resíduo cai 25× em 0) ·
Resolution Scale · Overall Intensity · Bicubic · Debug View (5 modos) · Measure Residual Again.

**Funciona, caminho rastreado até o consumidor, sem medição:** Encoding · Timing · Pass Count ·
Read Guides From The Game · Depth · History · Motion Vectors · Motion Scale · Flow Gate/Ratio.

Motion Scale e os sliders de flow só aparecem no ramo em que são lidos — UI e código conferem.

---

## 5. Os seis defeitos achados

### 5.1 Diffuse White rodava ao contrário do próprio rótulo

O código fazia `kWhite = 203.0f / white` — o **recíproco** — e usava 203 nos dois modos de
encoding. Subir o slider *escurecia* o que a rede via.

Na config da NFS (`Encoding=1`, `DiffuseWhite=100`, que é o automático documentado para linear
BT.709 e portanto deveria dar escala exatamente 1.0) ele multiplicava a imagem linear inteira
por **2.03**.

Agora é `white / referência`, com referência 100 no Linear e 203 no scRGB-nl.

**Isso muda a imagem.** É a hipótese §6 do handoff da sessão 2, e ela estava meio certa: a
conversão para linear já estava ligada, mas o fator em cima dela estava dobrando o sinal.

### 5.2 `0x76e1d` não é "movimento válido" — é `Temporal`

O nome era chute. O leitor de ini do próprio motor lê a chave `Temporal` para esse byte
(decompilado, `sub_180003940`).

Isso explica a medição da sessão 2 que ficou como "não está explicado": `Temporal=1` foi a única
execução em que o motor reportou movimento diferente de zero. Não é coincidência — acumular no
tempo é o que dá ao vetor de movimento algo para onde apontar.

Virou combo **Auto / Off / On**. Auto segue `haveMotion`, que é o comportamento antigo.

### 5.3 Depth Inverted vinha invertido

Os dois runtimes usam **1** como padrão: a DLL da NVIDIA põe `options+260 = 1` quando o
parâmetro não vem, e o inicializador estático do porte AMD põe `dword_180076E10 = 1`. Nosso
default era `false`. A gente invertia o padrão do motor em toda execução.

E o campo é real — `0x76e10` é lido de dez lugares. O tooltip anterior dizia "NÃO VERIFICADO,
pode não fazer nada"; estava errado e foi substituído.

### 5.4 `Tonemap` era forçado em 0

O padrão do motor é **-1**. O `InitEngine` escrevia 0 na inicialização — ninguém escolheu isso e
nunca foi medido contra nada. Agora segue o controle, que vem com o -1 do motor.

### 5.5 `UseAutoMask` nunca era escrito

É o **Character Mask** do RenoDX, em `0x76e40`, padrão 1. O add-on sempre dependeu do default.
É a explicação candidata para os 1,5% que a Pele mede: desligue e veja se a Pele para de fazer
até isso.

### 5.6 Dois campos que ninguém sabia que existiam

- **`Scale`** em `0x76e3c`, padrão `0.03125` = exatamente 1/32. **É o "0,031" que o handoff da
  sessão 1 anotou como "valor vivo que nunca escrevemos".** Identificado: chave de ini do motor
  chamada `Scale`. Nada a ver com a nossa Escala de Resolução. Exposto como **Engine Scale**.
- **`ToneChannels`** em `0x76e44`, padrão 0. Efeito desconhecido. Exposto para A/B.

---

## 6. A struct de opções do runtime AMD, mapeada

Veio de decompilar o leitor de ini (`sub_180003940`) e o inicializador estático
(`sub_180010220`). **Não é chute.**

```
0x76E10 int   DepthInverted   (default 1)
0x76E14 byte  ?               (default 0)   <- o add-on escreve 1, significado desconhecido
0x76E1C bool  Enabled
0x76E1D bool  Temporal
0x76E1E bool  UseFsrInputs
0x76E1F bool  UseDepth
0x76E20 int   Tonemap         (default -1)
0x76E28 qword estado da UI do seletor de pesos ("press Enter" / "waiting...")
0x76E30 float LocalTone       (default 0.0)
0x76E34 float LocalStructure  (default 1.0)
0x76E38 float SkinStructure   (default -1.0, sentinela de automático)
0x76E3C float Scale           (default 0.03125)
0x76E40 int   UseAutoMask     (default 1)
0x76E44 int   ToneChannels    (default 0)
0x76E48 int   ?               (default 1000)
```

Bloco derivado em **`0x764F8` = engine+0x20** (o objeto do motor é `h+0x764D8`):
`{LocalTone, LocalStructure}` mascarados pelo AutoMask, depois skin/structure derivados com a
mesma regra da NVIDIA, depois `Scale`.

Superfície de ini do runtime, completa: `Enabled · Temporal · UseFsrInputs · UseDepth · Tonemap ·
Interop · Inline · InlineWaitMs · LocalTone · LocalStructure · SkinStructure · Scale ·
UseAutoMask · HipDevice · ToneChannels`. Mais 6 env vars: `DLSSNR_NOBLEND · DLSSNR_NOPOSTHIST ·
DLSSNR_NO_REPACK · DLSSNR_SLOW_PREPOST · DLSSNR_STAGES · DLSSNR_WBLOG`.

`VIT512_OLD` **também é env var**, lida como bitmask: cada bit troca um `hipLaunchKernel` por uma
versão legada. E `vit512a`, `vit512b`, `vit512_attn`, `vit512_conv1`, `vit512_conv2`,
`vit512_ffwd` são **rótulos de profiling de estágios**, não variantes de modelo.

---

## 7. Model A/B/C — investigação completa, e o veredito

O usuário estava certo: **muda a imagem no RenoDX.** A primeira conclusão desta sessão ("não
existe") veio de duas inferências fracas — a string não aparecer no binário AMD e a chave não
estar no leitor de ini. `DLSSNR.Style` é **parâmetro NGX**, não chave de ini; não apareceria em
nenhum dos dois lugares.

### O que os presets são

Extraído do `renodx-dlss (1).addon64`:

```
'DirectNeuralRenderingStyle' / 'Model'
'Selects Neural Rendering Model A, Model B, or Model C through the prerelease DLSSNR.Style field.'
'Model A'  'Model B'  'Model C'
```

### O caminho, ponta a ponta, no `nvngx_dlssnr.dll`

Encontrado em `D:\SteamLibrary\steamapps\common\NBA 2K27\nvngx_dlssnr.dll` (165 MB).

```
DLSSNR.Style (int, options+236, default 0)
  -> clampado por um count vindo da descrição de rede: *(uint*)(base + 240*idx + 100)
  -> busca numa tabela de 8 slots x 68 bytes em record+100 (record estático em 0x1800B0D80)
  -> descritor = entry+8, ou record+40 se não achar  (= Model A)
  -> lerp de cada slot marcado na máscara, escalado por LocalToneStrength clampado em [0,1]
  -> escreve options+292..+344  (14 floats)
  -> copiados contíguos para o bloco de parâmetros dos kernels
```

O consumidor (`sub_18001D5F0`), decompilado:

```c
float t = opts[57];              // = options+228 = LocalToneStrength, clampado em [0,1]
uint mask = *(uint*)desc;
if (mask & 0x0001) opts[73] = (desc[1] - 0.0f) * t + 0.0f;
if (mask & 0x0002) opts[74] = (desc[2] - 1.0f) * t + 1.0f;   // único com neutro 1.0
if (mask & 0x0004) opts[75] = (desc[3] - 0.0f) * t + 0.0f;
... até (mask & 0x2000) -> opts[86]
```

### A tabela, byte a byte

Dois slots preenchidos dos oito. Os outros têm `valid = 0`.

```
entrada 0 (rec+100)            entrada 1 (rec+168)
+0  valid   = 1                +0  valid   = 1
+4  styleId = 1                +4  styleId = 2
+8  mode    = 0x34             +8  mode    = 0x20
+16 1.0f                       +16 1.0f
+20 -0.10f                     +20 0.0f
+28 -0.25f                     +28 0.0f
+32 -0.10f                     +32 -0.15f
```

| | Model A | Model B | Model C |
|---|---|---|---|
| mode | `0x00` | `0x34` | `0x20` |
| `opts[75]` | — | **−0.10** | — |
| `opts[77]` | — | **−0.25** | — |
| `opts[78]` | — | **−0.10** | **−0.15** |

**Model A = Style 0**, sem entrada na tabela, descritor neutro em `record+40`, `mode = 0`.
Não sobrescreve nada. É o baseline literal — e é por isso que só há duas entradas para três
modelos. Confere com o campo `record+36 = 3`.

**Não são três redes.** A DLL da NVIDIA tem 156 nomes de tensor `block*`, cada um aparecendo
exatamente uma vez. Um conjunto de pesos só, três configurações sobre ele.

### Por que não dá para portar

Os metadados HIP dos 34 kernels dão o tamanho do struct que cada um recebe por valor:

```
168  k_swin_var / k_align_probe      64  k_import(ImportParams)
128  k_export(ExportParams)          48  k_expand / k_mean / k_qkv_attn
 80  k_pre_block_1h_32_fp8            40  k_conv_res / k_qkv / k_ffwd
 72  k_ffwd_inpview                   32  k_post_block_1h_32_fp8
                                      32  k_final_head(HeadParams)
                                       8  k_reproject
```

O vetor de style são 14 floats = **56 bytes**. `k_final_head` — exatamente onde controles de
aparência entrariam — tem **32 bytes no total**. `k_post_block` idem. Não cabe.

Cadeia de evidência, cada elo verificado:

1. As constantes `-0.10`, `-0.25`, `-0.15` não existem no binário AMD.
2. O bloco de opções está mapeado campo a campo; nenhum slot é Style.
3. A superfície de condicionamento do porte são 4 floats mais o Scale. A da NVIDIA é isso **mais**
   os 14.
4. Nenhum código no binário escreve um span de 14 floats.
5. Os structs dos kernels do caminho de aparência não têm espaço.

Quem portou compilou a rede **com o style neutro embutido**. Os kernels são code objects GCN
pré-compilados dentro da DLL, sem fonte. Adicionar as entradas exigiria recompilar os kernels.

**Model A é o único que existe deste lado. Encerrado — não reabrir.**

---

## 8. Documentação e release — o que foi publicado

Tudo commitado no branch `master` e empurrado para o remote **`preview`**
(`dlss5-neural-amd-preview`). **`origin` — o repo público — continua em `ac6d8cb` e não recebeu
nada.**

```
da8c72f  Split the quick start into a DirectX 11 path and a PS2 emulator path
7d2c2a1  Say plainly that the target table is cosmetic and add ETS2 to it
8988e6e  Refocus the readme on D3D11 games and emulators
b1f039f  Correct the quick start for D3D11 and tighten the settings docs
5dec807  Say plainly that tokens are the metered cost
9feb048  Add a Ko-fi section and the GitHub sponsor button
4cff54a  State plainly that Model A/B/C will not be implemented
7bd28f2  Date the v0.3.0 entry                          <- a tag v0.3.0 aponta AQUI
a9009fb  Document the runtime option struct and the v0.3.0 changes
356f780  Rework the overlay and the pass path, and correct four engine defaults
```

**A tag `v0.3.0` ficou em `7bd28f2`, oito commits atrás do topo.** Não pega o "will not be
implemented", nem a seção de doação, nem a reescrita do README. Decidir se move ou se a próxima
tag cobre.

### `CHANGELOG.md` (novo, na raiz)

Entrada `v0.3.0` escrita **contra o que está público**, não contra a sessão anterior — então
cobre as sessões 2 e 3 juntas. Tem "If you are upgrading" no topo (começa desligado; pode apagar
`pass2..10.dll`) e "Known, and not fixed" no fim.

### README — refocado em DX11 + emuladores

O foco declarado do projeto mudou nesta sessão: **jogos Direct3D 11 e emuladores**. O README
inteiro foi reescrito em cima disso, e o Quick start está separado em **Case 1 — DirectX 11
games** e **Case 2 — PS2 emulator (PCSX2)**, com os arquivos e o "ligar" fora das abas porque são
idênticos nos dois.

Alvos rodados, agora declarados no topo: **ETS2** (o melhor resultado, comparável ao mesmo
network na NVIDIA), **PCSX2**, **NFS 2015**.

Também tem seção `## Keeping this going` com botão Ko-fi (`T6T213OVFE`) e `.github/FUNDING.yml`
para o botão Sponsor. O `<script>` do widget Ko-fi **não funciona** em README do GitHub — é
sanitizado — então é o botão estático.

### O achado que mais importava para adoção

**A tabela `kTargets` é puramente cosmética.** `tier` e `scale` só vão para o log, `note` só
aparece no status, e nada é gated por ela. Um jogo fora da lista roda idêntico.

Mas o README dizia *"adding a game is one row in a table"*, o que fazia parecer whitelist — quem
tem qualquer outro jogo concluiria que precisa clonar, editar C++ e recompilar, e desistiria
antes de tentar. Corrigido no README **e** com um comentário no próprio `kTargets` avisando para
não ler como whitelist. ETS2 adicionado à tabela e os sufixos "Direct3D 12" removidos das notas,
que contradiziam as instruções novas.

**`eurotrucks2.exe` é chute** para a linha do ETS2. Se estiver errado o único efeito é o painel
dizer *uncatalogued target* — não quebra nada.

### Erros de README corrigidos

Todos eram afirmações que ficaram falsas depois das sessões 2 e 3:

- O passo 5 mandava usar **D3D12** e dizia que `Renderer = 3` (D3D11) "é o problema, não a
  solução". Invertido.
- O passo 6 dizia que **nada é salvo entre execuções**. Existe Save Settings.
- Não avisava que **começa desligado** — a primeira coisa que um leigo precisa saber.
- A seção "What the network can actually be fed" terminava em *"the depth buffer is empty, and
  that is the wall"*. Conclusão da sessão 2 que a própria sessão 2 resolveu. 68 linhas → 41.
- `dlss5-neural.addon64 73728` no "deu certo se a saída for assim" — o build tem 181248.
- Três linhas de Troubleshooting obsoletas, incluindo uma que ensinava a copiar e renomear
  `pass2.dll`, que não existe mais.
- `src/session/session.cpp` descrito como "small session logger" — é o harness da ponte.

**Nota de método:** o verificador de âncoras que eu usava estava errado — colapsava espaços, e o
GitHub troca **cada** espaço por um hífen, então um travessão removido deixa dois hífens no slug.
Os 8 links internos conferem contra os 24 headings com o verificador corrigido.

---

## 9. Arquivos e estado

### `D:\dlss5`
Working tree limpa, tudo commitado. Os três alvos (`neural`, `probe`, `session`) compilam do
zero; `tools\check_shaders.ps1` passa nos 8 shaders.

### `D:\SteamLibrary\steamapps\common\Need for Speed`
`dlss5-neural.addon64` atualizado. `dlss5-neural.ini` com `Tone=1`.
**`dlssnr_amd_pass2..10.dll` podem ser apagados** — só o `pass1` é usado agora.

### `D:\pcsx2-v2.8.2-test`
`dlss5-neural.addon64` atualizado. Mesmos `pass2/3` descartáveis.

### Referências da investigação (não são do projeto, não mexer)
- `C:\Users\claudinhh\Desktop\renodx-dlss (1).addon64` — a tabela de UI do RenoDX
- `D:\SteamLibrary\steamapps\common\NBA 2K27\nvngx_dlssnr.dll` — o Style inteiro
- IDBs (`.i64`) gerados ao lado dos dois binários

---

## 10. O que fazer em seguida, em ordem de valor

1. **Olhar o Diffuse White na tela.** É a correção de maior impacto visual (§5.1) e ninguém viu o
   resultado ainda. Deveria ser feito antes de o público ver o release.
2. **Conferir os acentos do painel em português.** Se a fonte do ReShade não tiver os glifos
   Latin-1, aparecem quadrados. Também vale conferir antes de publicar.
3. **Medir os cinco controles novos** com `measure, residual`: Character Mask, Engine Scale,
   Temporal explícito, Tone Channels, Depth Inverted no padrão certo.
4. **Confirmar que `Passes=2` não trava mais.** Async, mesma cena, 1 contra 2.
5. **Medir os guides em gameplay** — pendente desde a sessão 2, e só quem joga consegue.
6. **Decidir a tag `v0.3.0`** (§8) e se/quando `origin` recebe.

## 11. O que não perseguir

- **Model A/B/C.** Encerrado com evidência completa (§7), e declarado como *will not be
  implemented* no README e no CHANGELOG. Motivo público: o porte AMD é closed source e os kernels
  são code objects pré-compilados.
- **Rodar a rede numa GPU diferente da do jogo.** Perguntado por uma usuária (jogo numa placa,
  network na 9070 XT). O add-on casa o LUID do adaptador do jogo porque o transporte é textura
  compartilhada e fence, e isso só funciona entre devices da **mesma** GPU física. Cross-adapter
  exigiria readback para RAM e re-upload por PCIe todo frame nos dois sentidos. Anotado como
  pedido, não como plano.
- **Injetar o ReShade dentro do Lossless Scaling / Magpie.** Nesse ponto só existe a imagem final
  capturada — não há depth para achar. O caminho certo é injetar no jogo; o LS captura o frame já
  processado depois.
- **Ray Reconstruction.** Sem ray tracing não há ruído para reconstruir.
- **Igualar o vídeo da NVIDIA com a rede sozinha.** Boa parte vem da reconstrução temporal
  (DLAA/SR), que é outra DLL e não tem porte AMD.
- **`Passes>1` como ajuste de qualidade.** Nunca mediu melhor. É instrumento de medição.
