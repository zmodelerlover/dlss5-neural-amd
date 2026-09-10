# dlss5-neural-amd — handoff da sessão 2

Sessão de 09/09/2026. Base: `ac6d8cb`. **Nada foi commitado** — tudo na working tree de `D:\dlss5`.

```
 M README.md
 M src/neural/neural.cpp
 ?? tools/check_shaders.ps1
```

Alvo novo desta sessão: **Need for Speed 2015** (`D:\SteamLibrary\steamapps\common\Need for Speed`),
Frostbite, **D3D11**, sem FSR e sem DLSS.

Tudo marcado como *medido* veio de execução real. O que está sem verificação está dito assim.

---

## 1. As três perguntas da sessão

1. Por que o PCSX2 não dá o resultado do RenoDX na NVIDIA.
2. Fazer funcionar na NFS 2015 com o máximo de fontes que o jogo permite.
3. Por que o resultado ainda parece filtro de cor e não neural rendering.

A 1 e a 2 foram resolvidas. A 3 tem uma causa provável identificada e **não verificada visualmente**.

---

## 2. O erro de DirectX — resolvido, com causa raiz

O jogo morria com `screen->ResizeBuffers(...) failed` / `DXGI_ERROR_INVALID_CALL`.

### Causa

`dlssnr_amd_pass1.dll` **importa `d3d12.dll` estaticamente**. Trazer essa biblioteca para um processo
D3D11 faz o ReShade instalar os hooks que ele mantém à espera dela — o log dele diz literalmente
*"Installing delayed hooks for d3d12.dll (Just loaded via LoadLibrary)"* — e fazer isso com uma
swapchain D3D11 já viva quebra o próximo `ResizeBuffers`.

### A bissecção que provou (medida)

| configuração | resizes | resultado |
|---|---|---|
| sem add-on | 12 tentados, 0 falhas | roda |
| add-on probe (não importa biblioteca gráfica) | 8 tentados, 0 falhas, 1800+ frames | roda |
| nosso add-on **sem executar código nenhum** (`Events=0`, `NoBridge=1`) | 1 falha | morre |
| **d3d12.dll nunca carregada** | 5 tentados, **0 falhas** | roda |
| **d3d12.dll carregada, nenhum device criado** (`Stage=1`) | 1 falha | morre |

As duas últimas linhas são a resposta inteira: **o carregamento sozinho dispara**, não o código.

### O conserto (três partes)

1. **Nenhuma biblioteca gráfica na import table.** `D3D12CreateDevice`,
   `D3D12SerializeRootSignature`, `CreateDXGIFactory1` e `D3DCompile` passaram a ser resolvidos à mão.
   `dumpbin /dependents` agora mostra só USER32/KERNEL32/bcrypt/CRT — igual ao probe e igual ao
   `renodx-dlss.addon64` do ShortFuse (verificado: ele também não importa nada gráfico).
2. **D3D12 entra como cópia privada** — `dlss5-runtime\dx12p.dll` com `D3D12Core.dll` ao lado.
   Identidade de módulo é o caminho completo, então o ReShade não tem hooks registrados para esse
   nome. `LOAD_WITH_ALTERED_SEARCH_PATH`. Sem o core dá `D3D12_ERROR_INVALID_REDIST` (0x887E0003).
3. **A import do runtime é reapontada.** `dx12p.dll` tem exatamente o mesmo comprimento de
   `d3d12.dll`, então uma cópia do runtime com esse nome sobrescrito importa a nossa —
   `dlss5-pass1.dll`. O arquivo original não é tocado, e é ele que continua sendo conferido no SHA-256.

Um jogo já em D3D12 pula tudo isso: a `d3d12.dll` dele foi carregada e hookada muito antes.

### Resultado medido

**10.920 frames, 2 resizes, 0 falhas, 0 hooks de d3d12**, job da rede em 15–16 ms, história ligada.

### Consertos menores no caminho

- **Access violation no frame 1**: a tally de binds guardava `ID3D11Resource*` cru entre frames;
  alvo liberado pelo jogo virava ponteiro pendurado. Agora tem contagem de referência e a descrição
  é lida no bind, enquanto o recurso está provadamente vivo.
- **Byte de "motion válido" (`0x76e1d`) estava fixo em 1**, dizendo ao motor que o campo era válido
  no primeiro frame, antes de existir. Agora segue `haveMotion`.
- Fechamento no `destroy_swapchain`, com dreno por event query, e **portão de present** na janela do
  resize (o `ResizeBuffers` roda em outra thread — medido, thread 35300 — e a thread de render podia
  reagarrar o back buffer entre o desmonte e a chamada).
- Fences de GPU entre devices trocadas por sincronização de CPU. Isso sozinho levou de **1 frame → 55 → 518**.

---

## 3. As fontes da NFS 2015 — implementado e medido

O `dlss5-probe.log` do jogo mostra, em gameplay a 1920×1080:

```
1161c70e0  1920x1080  R32G8X24_TYPELESS   2373 draws   <- depth
1e25aece0  1920x1080  R16G16_FLOAT          65 draws   <- motion vectors
1e25ad420  1920x1080  R11G11B10_FLOAT     1990 draws   <- cor da cena, HDR linear, pré-tonemap
```

### O que foi construído

Uma máquina genérica de *guides* em D3D11: conta quantas vezes o jogo liga cada depth-stencil e cada
render target de dois canais float por frame, escolhe o mais ligado, tira **uma cópia por frame** no
present e atravessa a ponte.

- **depth**: snapshot privado → compute D3D11 → `R32_FLOAT` compartilhado (os formatos typeless de
  depth não abrem no segundo device).
- **motion**: já é `R16G16_FLOAT`, que compartilha, então é um `CopyResource`. Reamostrado para o
  raster do motor com o botão `MotionScale` para sinal/unidade.
- O estimador de optical flow é **desligado** quando há vetores reais.

### Estado medido

O motor confirma, com as palavras dele:

```
staging ready: colour 960x540 dxgi 10; motion 960x540 dxgi 34; depth 960x540 dxgi 41; exposure no
```

Três dos quatro slots do `Packet` chegam. **Mas** a sonda numérica que eu adicionei diz, em menu:

```
guide probe, depth 640x360: min 1.000000 max 1.000000 mean 1.000000  <-- CHAPADO
guide probe, motion 640x360: 100% exatamente parado
```

e o próprio motor: `job N motion: mean |mv| = (0.000, 0.000), depth on`.

**Em menu isso é esperado** — tela 2D não tem profundidade nem movimento. **Nunca foi medido em
gameplay de verdade.** Essa é a verificação que falta e que só o usuário pode fazer, dirigindo.

A sonda re-arma a cada 600 frames até ver dados reais, e os números aparecem no overlay em
**Guides → "Measured depth ... motion ..."**.

---

## 4. Por que o PCSX2 não alcança a NVIDIA

Não é ajuste. São três das quatro entradas que não existem.

- **motion**: o GS do PS2 não tem buffer de velocidade. `GSVertex` é UV, RGBA, XYZ, fog. O add-on
  estima por block match, e no God of War 2 mediu **99% dos blocos parados**.
- **depth**: existe, mas em D3D12 o ReShade entrega **0 binds em 600 frames**; em D3D11 entrega
  todo frame. E só existe entre um bind e o clear do PCSX2. Valor máximo ~**0,002**.
- **exposure**: nunca preenchida.

O RenoDX na NVIDIA alimenta `DLSSNR.Depth`, `DLSSNR.MVec`, `DLSSNR.UI`, `DLSSNR.UIAlpha` porque o
jogo já calcula tudo isso para o upscaler temporal dele — daí o `UseFsrInputs=1` e a exigência de
"jogo DirectX 12 com FSR". O PCSX2 não tem chamada de upscaler para interceptar.

---

## 5. Pesquisa: como funciona do lado NVIDIA

### Os projetos

| Projeto | O que é |
|---|---|
| [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) | O que roda em PCSX2 / RPCS3. Aberto. |
| [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) | D3D11 + Vulkan, tem "substitute contract" para jogos sem DLSS |
| `renodx-dlss5` (Krish) | Consumidor neural |
| `renodx-dlss` (ShortFuse) | **Rota Present — é a que nós copiamos** |
| [OptiScaler PreSR](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) / [NR-before-SR](https://github.com/Markxiao94/OptiScaler-DLSSNR-NR-before-SR) | NR antes do SR |
| [DLSS5-Swapper](https://github.com/rakanki911/DLSS5-Swapper) | **Instalador** (JavaScript, 3.3k estrelas). Não renderiza nada. **Sem AMD.** |

### O mecanismo, citado

Feeder: *"monta uma requisição DLSS **sintética** a partir do quadro que o ReShade processa, de um
depth acessível e de motion vectors estimados, **executa uma avaliação DLSS real**, e copia o
resultado de volta."* / *"a chamada DLSS está sendo feita **pelo injetor, não pelo jogo**."*

dlss5-bridge: *"**DLAA at back-buffer size**... Requires `synth=1`."*

**Eles não colocam o upscaler do jogo. Eles criam o upscaler.** Instanciam uma feature NGX DLSS/DLAA
de verdade num device D3D12 privado e a avaliam. O consumidor neural engancha *essa* avaliação.

### As três DLLs, e o que temos

| Peça | DLL NVIDIA | Temos? |
|---|---|---|
| Rede de aparência (NR) | `nvngx_dlssnr.dll` | **Sim** — `dlssnr_amd_pass1.dll` |
| Reconstrução temporal (SR/DLAA) | `nvngx_dlss.dll` | **Não.** Equivalente AMD: FSR 3.1/4 **Native AA** |
| Denoiser de ray tracing (RR) | `nvngx_dlssd.dll` | **Irrelevante** — ver abaixo |

**Ray Reconstruction está fora de escopo, permanentemente.** É denoiser de amostras de raio; precisa
de um quadro ruidoso de RT para limpar. NFS 2015 (raster puro) e PCSX2 não lançam um raio. Não é
difícil, é sem entrada.

### A rede é a mesma

Kernels HIP dentro do `dlssnr_amd_pass1.dll`: `k_swin_var<32|64|128|256>`, `swin_layer`,
`k_attention`, `k_conv_res`, `k_conv_res2`, `k_contract2`, `vit512a`, `vit512b`, `VIT512_OLD`.
Pesos: `dlssnr_on_amd_weights.bin`, 147 MB, extraídos do `nvngx_dlssnr.dll` build 310.8.0.0.
Alvos: `gfx1201, gfx1200, gfx1100, gfx1101, gfx1102`. Versão **v0.2.14, alpha**.

**Mesma arquitetura, mesmos pesos, backend HIP em vez de CUDA.** A rede não é a diferença.

### Multipass — a rota que dá o resultado

Do README do DLSS5-Swapper: *"**Multipass/DLSS Tool Route** — instala o build do ShortFuse que roda
passes neurais **até dez vezes por quadro**, enganchando **`Present`** em vez de pegar carona no DLSS."*

Do binário do ShortFuse: *"Passes adicionais consomem a saída do Neural Rendering anterior
diretamente; **a preparação da fonte e a composição da saída rodam só uma vez**."*

Ou seja: **mesmo ponto de hook que o nosso**, e a diferença declarada é multipass.

---

## 6. A hipótese principal do "parece só cor" — NÃO VERIFICADA

Do README do Feeder: *"o quadro é decodificado para **luz linear em FP16** na entrada e re-codificado
na saída, para que **o consumidor neural receba o HDR linear que ele espera**."*

O nosso `kCopyShader` com `Encoding=0` (o default até agora):

```hlsl
float3 v = c.rgb;
if (mode != 0) v = ToLinear(saturate(v)) * k;   // mode==0 -> não converte nada
```

A rede recebia valores **sRGB** tratados como luz linear. Domínio errado. E o `kComposeShader` soma
o resíduo **em espaço sRGB** e ceifa (`saturate`) — com resíduo de `max 0.43` medido, qualquer pixel
acima de 0,57 estoura. Bate com os três sintomas relatados: cor estourada, cara de filtro, e
"aumentar o valor só mexe na exposição".

O ShortFuse documenta os automáticos de diffuse white: **100 nits para linear BT.709**, 250 para
BT.2100 PQ e scRGB linear, 203 para scRGB-nl. **O nosso default era 500**, que não corresponde a
nenhum.

**Mudança aplicada no ini da NFS (só config, sem código):** `Encoding=1`, `DiffuseWhite=100`.
**Falta olhar na tela.** É ao vivo no overlay: combo **Encoding** e slider **Diffuse White**.

---

## 7. O que travou a máquina

Eu subi o teto de `Passes` de 3 para 10 (como o ShortFuse) e deixei `Passes=4` no ini com
`Inline=1`. Inline é **parada síncrona da GPU**: 4 passes × 15 ms passa do watchdog, e o próprio
`dlssnr_on_amd.ini` avisa que uma parada assim dispara o TDR do Windows e remove o device D3D12.

**Configuração segura atual: `Passes=1`.**

**Não há trava no código.** O ini ainda aceita `Passes=10` com `Inline=1`. Multipass só deve ser
testado com `Inline=0`, subindo de 1 para 2. **Pôr essa trava é a primeira tarefa do próximo que
mexer nisso.**

---

## 8. Medições registradas

Bateria com `Scale=0.50`, em menu (não em gameplay — os números de gameplay não existem ainda):

| config | resíduo médio | ratio | motor: mean \|mv\| |
|---|---|---|---|
| A: Passes=1, Temporal=0 | 0,013327 @640×360 | 0,973 | (0,000, 0,000) |
| B: Passes=1, **Temporal=1** | 0,017170 @960×540 | 0,672 | **(0,036, 0,004)** |

As duas rodaram em resoluções diferentes de back buffer, então **não são comparáveis direto**. O que
interessa: `Temporal=1` foi a única configuração em que o motor reportou movimento diferente de zero.
Não está explicado.

`Passes=2` e `Passes=3` com a barreira nova: **nunca chegaram a rodar** (a bateria foi interrompida,
depois veio o travamento).

---

## 9. Estado dos arquivos

### `D:\dlss5` (working tree, nada commitado)
- `src/neural/neural.cpp` — todo o trabalho desta sessão
- `tools/check_shaders.ps1` — extrai os 8 shaders HLSL e roda `fxc`. **Todos compilam.**

### `D:\SteamLibrary\steamapps\common\Need for Speed`
```
d3d11.dll                  ReShade 6.8.0 add-on build
dlss5-neural.addon64       nosso add-on
dlss5-neural.ini           config (ver abaixo)
dlssnr_amd_pass1..10.dll   runtime (2..10 são cópias do 1, hashes conferem)
dlss5-pass1.dll            cópia gerada do runtime, import reapontada para dx12p.dll
dlss5-runtime\dx12p.dll    cópia privada da D3D12.dll do sistema
dlss5-runtime\D3D12Core.dll
dlssnr_on_amd_weights.bin  147 MB
dlssnr_on_amd.ini          UseDepth=1, Temporal=0, InlineWaitMs=100
_addons-off\               probe e session, fora do caminho
```

### ini seguro atual
```ini
[dlss5]
Inline=1
Structure=3
Skin=1
Tone=0
Depth=1
Motion=1
History=1
Bicubic=1
GameGuides=1
MotionScale=1.0
Passes=1
Scale=0.50
Encoding=1
DiffuseWhite=100
```

### Chaves de diagnóstico (default = desligado quando ausentes)
`NoBridge=1` não levanta a ponte · `NoBackBuffer=1` roda tudo sem tocar na swapchain ·
`Stage=1|2|3` para antes da d3d12 / antes do motor / tudo · `Events=<máscara>` 1 bind, 2 draw,
4 clear, 8 destroy_swapchain, 16 overlay.

Foram o que resolveram o erro de DirectX. Valem manter.

### Harness de teste
`nfstest.ps1` e `battery.ps1`, copiados para o Desktop ao lado deste arquivo. O `nfstest.ps1` sobe o
jogo e mata no instante em que sabe a resposta (PASS / ERROR-DIALOG / NO-FRAMES / RESIZE-FAILED) —
veredito em ~15 s em vez de 150.

---

## 10. Roadmap, em ordem de valor

1. **Verificar `Encoding=1` + `DiffuseWhite=100` na tela.** Config já aplicada, falta olhar. É a
   hipótese principal do "parece só cor" e custa alternar um combo no overlay.
2. **Trava de segurança**: recusar `Passes>3` quando `Inline=1`. Foi o que travou a máquina.
3. **Medir os guides em gameplay.** A sonda e o overlay já reportam sozinhos; precisa alguém dirigindo.
   Se depth continuar chapado e motion zerado numa corrida, os guides pegaram o buffer errado, e o
   próximo passo é capturar o depth **antes do clear** em vez de no present (o caminho D3D12 já tem
   esse código; falta a versão D3D11).
4. **Verificar o multipass** com `Inline=0`, `Passes=2`. A barreira UAV entre passes está no código
   e nunca foi exercitada. É a diferença declarada da rota que dá o resultado que o usuário quer.
5. **Cor HDR linear pré-tonemap como entrada** (`R11G11B10_FLOAT`, 1990 draws). Hoje lemos o back
   buffer 8-bit já tonemapeado e com HUD. Exige interceptar no meio do quadro, não no present —
   é trabalho de arquitetura, não de ajuste.
6. **FSR 3.1/4 Native AA depois da rede.** É o equivalente AMD do DLAA e a peça que falta para a
   reconstrução temporal. **Ressalva:** a NFS 2015 já tem TAA própria, então empilhar outra rende
   menos do que num jogo sem TAA.
7. **Nunca testados**: `DLSSNR_STAGES` (variável de ambiente achada no binário), `UseAutoMask`
   (chave de ini que o add-on nunca escreve), variantes de modelo `vit512a` / `vit512b`.
8. **Slot de exposure** continua vazio. O alvo 1×1 não é identificável com confiança pela contagem
   de binds, e nunca se demonstrou que o motor lê esse slot.
9. **Nada foi commitado.**

---

## 11. O que não perseguir

- **Ray Reconstruction** nestes alvos. Sem ray tracing não há ruído para reconstruir.
- **Igualar o vídeo da NVIDIA com a rede sozinha.** Boa parte do "cada textura melhorou" vem da
  reconstrução temporal (DLAA/SR), que é outra DLL e não tem porta AMD.
- **`Passes>1` com `Inline=1`.** Trava a máquina.
