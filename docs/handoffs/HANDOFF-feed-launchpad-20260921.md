# HANDOFF — guias externas, styles em HSL, e uma caçada errada, 21/09/2026

Sessão seguinte à `HANDOFF-models-abc-20260921.md`. Duas metades bem diferentes:

**A primeira metade produziu trabalho sólido** — oito commits na branch `feed`, todos construídos e
com verificador próprio. **A segunda metade foi uma caçada a um bug que eu mesmo criei na bancada**,
custou dois resets de driver e várias horas, e terminou com tudo restaurado ao estado pré-sessão.

Leia a §7 antes de repetir qualquer experimento nesta bancada.

---

## 1. Onde está tudo

| | |
|---|---|
| Repo de trabalho | `C:\Users\claudinhh\Desktop\dlss5amdrework\repo` |
| Branch nova | **`feed`** — 8 commits sobre `models` |
| Branch anterior | `models` — a da sessão passada, ainda intacta |
| Remotes | `origin` = `https://github.com/zmodelerlover/dlss5-neural-amd.git`, `local` = `D:\dlss5` |
| Bancada PCSX2 | `D:\pcsx2-v2.8.2-test` — GoW2 `SCUS-97481`, D3D11 (`Renderer = 3`), upscale 5x |
| ISO | `D:\iso ps2\God of War 2 VERSÃO I.A.iso` |
| Save state | `D:\pcsx2-v2.8.2-test\sstates\SCUS-97481 (2F123FD8).01.p2s` |
| DLL da NVIDIA | `C:\Users\claudinhh\Desktop\0.3.0\nvngx_dlssnr.dll` (também em `Desktop\a\` e no NBA 2K27) |
| Runtime AMD | `dlssnr_amd_pass1.dll`, 7 290 880 bytes, ao lado do jogo |

### Repos de terceiros clonados para leitura (scratchpad, descartáveis)

| | |
|---|---|
| DLSS5-Feeder | `https://github.com/jlrouzies-fr/DLSS5-Feeder` — de onde veio o desenho do `.fx` |
| OptiScaler DLSS-NR | `https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG`, branch `dlss-neural-rendering` |
| Deep Fried Chicken | não tem repo público; os docs estão em `DLSS5-Feeder/external/deepfried/` |
| Magpie / lmxxf | `referencias\Magpie-DLSS5-AMD-0.26.zip`, fonte em `D:\lmxxf-src` |

---

## 2. Os commits da branch `feed`

```
73d152c Put the depth stretch on a switch, next to the guide it changes
45ac908 Give each pass its own history instead of sharing the chain's last output
ac0fe1c Name the models the way both consumers name them
e72807d Let a selected style reach the frames the network sits out
c432112 Keep ReShade's own render targets out of the guide observation
21019fa Document the companion effect and what it replaces
05ff491 The style vector's saturation slot works in HSL, not HSV
63545f7 Take motion and depth from a ReShade optical-flow shader when one is installed
1a452da Give the network the depth range it was trained on, not the raw buffer   <- cherry-pick de `styles`
```

Diff contra `origin/master`: 8 arquivos, +3744 / −57. Arquivos novos:
`shaders/DLSS5_Neural_Feed.fx`, `tools/feed_fx_check.py`.

**Nada disso foi validado em jogo.** Compila, passa nos verificadores, e só.

---

## 3. O que foi descoberto (fatos, com evidência)

### 3.1 O Feeder usa Launchpad, mas não é o recomendado dele

`DLSS5_MV_PROVIDER` no `DLSS5_Feed.fx` escolhe entre cinco: `0` texMotionVectors, `1` iMMERSE
Launchpad, `2` VORT, **`3` LumeniteFX Kernel (o recomendado)**, `4` LumeniteFX QuantMotion. O
Feeder não estima movimento nenhum — ele lê o shader que você instalar.

Instalado nesta máquina: **só o Launchpad**, em
`C:\Users\claudinhh\AppData\Local\FiveM\FiveM.app\plugins\reshade-shaders\Shaders\iMMERSE\`.
O qUINT que está lá é o de RTGI, sem `qUINT_motionvectors.fx`.

O Launchpad desta versão roda pirâmide de **8 níveis** com filtro entre cada um, contra os 2 níveis
de block matching raio 4 do nosso estimador. `Deferred::MotionVectorsTex`, RG16F, resolução cheia.
Esta versão **não** tem o `IPC::PredicationBuffer` das versões novas.

### 3.2 Natural e Cinematic são o Model B e o Model C

`DLSSNR.Style` — mesmo campo, três valores, dois conjuntos de rótulos em circulação:

| valor | RenoDX | Deep Fried Chicken / OptiScaler |
|---|---|---|
| 0 | Model A | Default |
| 1 | Model B | **Natural** |
| 2 | Model C | **Cinematic** |

Três fontes independentes confirmam: `kNRStyleItems[] = { "Default", "Natural", "Cinematic" }` em
`DLSS5-Feeder/src/dlss5-feed32.cpp` (copiado literalmente da string table do DFC); `nrStyleNames[] =
{ "Default (standard)", "Natural", "Cinematic" }` em `OptiScaler/dlssnr/DlssNr_Menu.cpp`; e o
comentário em `OptiScaler/Config.h:265` — *"0 default (standard), 1 natural, 2 cinematic"*.

Ou seja: **já estava implementado**. O overlay agora mostra os dois nomes.

### 3.3 NRPreset não tem onde ser aplicado

`DLSSNR.Hint.Render.Preset` → `CG2RFindWeightByPreset` → escolhe um **descritor de pesos**, não cor.
A tabela de recursos da DLL tem **duas** entradas e só:

```
#10/WEIGHTS_HT/#1033     147.695.410 bytes  (140,9 MB)   <- a rede
#16/#1/#1033                   1.184 bytes                <- version info
```

`WEIGHTS_HT` é o nome para onde o descritor aponta em `record+16` e o que o runtime AMD procura.
Um segundo preset precisaria de um segundo blob; não existe. Daí:

```
DLSSNR: preset %d is not available in this DLL build; falling back to shipping default preset %d ('%s')
```

Confirmação: os três `nvngx_dlssnr.dll` do disco têm hash idêntico (`E16BCF15...`), e os doze
`dlssnr_on_amd_weights.bin` também (`6BF8DC93...`). Um cérebro, copiado doze vezes.

O OptiScaler **não implementa** preset — `DlssNr_Proxy.cpp:109` só faz
`SetUInt(params, "DLSSNR.Hint.Render.Preset", ...)` e entrega ao NGX. Nós não temos NGX: o
`InitFn` do runtime AMD recebe um **caminho de arquivo de pesos**, e não há campo de preset na
struct de opções.

### 3.4 O slot de saturação do style é HSL, não HSV — e estava errado no nosso código

Lido do SASS (`docs/third-party/cg2r_post_process_kernel.sm120.sass`):

```
FADD  R9, R13, R14        ; max + min
FMUL  R2, R9, 0.5         ; L = (max+min)/2          <- lightness HSL
FSETP.GT P1, PT, R2, 0.5  ; ramo em L > 0.5
@P1  S = d / (2 - max - min)
@!P1 S = d / (max + min)
LDCU UR4, c[0x0][0x4d0]   ; opts[78]
FFMA.SAT R11, R0, UR4, RZ ; S' = saturate(S * (1 + k))
```

Nada disso existe em HSV, onde `V = max` e `S = d/max` sem ramo. O nosso shader fazia HSV, e o
`tools/style_check.py` **passava** porque tinha sido escrito da mesma leitura errada — validava a
forma fechada contra a própria premissa.

Corrigido para a forma fechada do round-trip HSL: `c' = L + (S'/S)(c - L)`, que vale porque
`d = 2*S*min(L, 1-L)` dos dois lados do ramo. O teste agora roda contra `colorsys.rgb_to_hls`.

Também entrou o `saturate` intermediário depois do contraste, que o kernel faz (`FADD.FTZ.SAT`).

Ordem dos três slots conferida nas **duas** cópias da cadeia no kernel (`0x2ef0` e `0x5600`):
exposição `0x4c4` → contraste `0x4c8`/`0x4cc` → saturação `0x4d0`. Bate com o shader.

### 3.5 Onde o style é aplicado — e um defeito real

Está no lugar certo: última expressão antes do store, depois do `ToSrgb`, sobre valores
display-referred em `[0,1]`. Igual ao kernel, que roda no fim do evaluate sobre a saída da rede —
que no Neural Rendering *é* o frame apresentado.

**O defeito:** o frame composto só era colado de volta quando a rede produziu correção naquele
frame (`CompositionIsFresh`). Num frame pulado o frame cru do jogo saía **sem grading**, então um
style selecionado piscava junto com a taxa de skip. A NVIDIA não tem esse frame. Corrigido: nesses
frames o compose roda com a correção forçada a zero (identidade) e o grading é a única coisa que
ele faz. Com `Style=0` o gate antigo não muda.

### 3.6 History era compartilhado entre passes

Havia **uma** textura de history para a cadeia, guardando a saída final do frame anterior. O passe 2
recebia a saída do passe 2 do frame passado enquanto sua entrada era o passe 1 deste frame — nenhum
passe recebia a amostra temporal do próprio estágio.

A referência confirma o desenho certo — tooltip do OptiScaler:
*"ten passes is ten model runs in one frame. **Each also holds an NGX feature with its own history**"*.

Mudado para `history[kMaxPasses]`, cópia dentro do loop logo após cada passe, `historyValid` virou
bitmask. **Não validado em jogo** — ver §7.

---

## 4. O que entrou de código

### `shaders/DLSS5_Neural_Feed.fx` (novo)

Effect companheiro. Declara a textura do provider byte a byte como o provider declara, converte para
delta-UV e valida. Saídas: `DLSS5N_MV` (RG16F, delta-UV) e `DLSS5N_Depth` (R32F).

Validação portada do Feeder, sem a máscara (nosso Packet não tem esse slot): hipótese estática em
estrutura 3x3 com média local removida + histerese de dois frames, teste de profundidade
(disoclusão), teste de consistência do vetor. O teste de luma foi omitido — é `mask-only` e está
desligado por padrão no Feeder.

Default `DLSS5N_MV_PROVIDER = 1` (Launchpad), porque é o que existe nesta máquina.

### `src/neural/neural.cpp`

- `Guide::external` + `FeedTexture()` + `AdoptFeedEffect()`: pega as texturas do effect pelo
  `find_texture_variable` e alimenta o guide existente. Preferência: **vetores do jogo > effect >
  estimador**. Só na rota D3D11.
- `TechniqueOn()` / `AnyMvProviderOn()`: recusa a textura se a técnica não estiver ligada, porque
  uma textura não escrita alimenta zeros, que é pior que o estimador.
- `g.inEffects` em `reshade_begin_effects`/`reshade_finish_effects`, e `ObserveD3D11` ignora binds
  nessa janela. **Sem isso o tally de guides via os render targets do próprio ReShade** — todo shader
  de fluxo óptico escreve em RG16F do tamanho da tela, que é exatamente a descrição que o código usa
  para reconhecer um velocity buffer.
- `SettleGuide` volta a rodar incondicionalmente e limpa `external`, para o jogo poder retomar o slot.
- Chave de ini `FeedEffect` + checkbox no overlay.
- `DepthNormalise` ganhou checkbox (antes só existia no ini).
- Probe de guias rearma quando a fonte do motion troca de mãos, e a linha diz qual fonte mediu.
  Antes ele latchava após uma leitura, tomada nos primeiros segundos enquanto o ReShade ainda
  compilava — ou seja, **sempre media o estimador, nunca o provider**.

### `tools/feed_fx_check.py` (novo)

O `.fx` é ReShade FX: o `build.ps1` não olha para ele e o `fxc` não lê. Este script reescreve a
sintaxe do ReShade em SM5 (sampler → Texture2D + SamplerState, `tex2Dlod` → `SampleLevel`, uniforms
→ constantes no default) e roda `fxc` em cada pixel shader. Também confere que cada entry point
escreve o número de render targets que o passe liga, e que as texturas dos cinco providers estão
declaradas.

### Verificadores, todos passando

```
powershell -File tools\check_shaders.ps1   -> All shaders compile.
python tools\style_check.py                -> HSL round trip confere
python tools\feed_fx_check.py              -> PS_Guides 3 targets, PS_History 4 targets
python tools\runtime_offsets_check.py      -> PASS
.\build.ps1 -Target neural                 -> OK
```

---

## 5. O que NÃO está estabelecido

- **Nada foi validado em jogo.** Nem o effect, nem a validação de vetores, nem o history por passe,
  nem a correção HSL, nem o grading em frames pulados.
- **Que o `models` bate com a NVIDIA pixel a pixel.** Continua `RASTREADO`, não `MEDIDO`.
- **Que o history por passe resolve o ruído de iluminação com 2+ passes.** A hipótese veio de uma
  observação do usuário (1 passe + history = sem ruído; 2 passes + history = ruído) e da referência,
  mas o teste nunca rodou limpo.
- **Que normalizar a profundidade ajuda.** Agora há *três* sinais contra: o §8 do handoff anterior,
  e duas leituras desta sessão. Ver §6.
- **Só a rota D3D11** pega guias do effect. D3D12, Vulkan e OpenGL não.
- **Exposição continua `nullptr`** — quarto slot do Packet, nunca alimentado.

---

## 6. A normalização de profundidade parece ser a operação errada

O probe, com o rearme novo, mostrou os dois lados:

```
frame 123: depth min 0.000000  max 0.001995  mean 0.001970   -> escala 501.1x travada
frame 800: depth min 0.000000  max 1.000000  mean 0.987549   <- já escalado
```

A média é **98,7% do máximo**: o grosso dos pixels do PS2 já está colado no teto da própria faixa.
Multiplicar por `1/max` não espalha nada — joga quase todo pixel em 0,99. A geometria continua
ocupando ~1% da faixa, só que agora no teto, e com `DepthInverted=1` isso lê como *"a cena inteira
está encostada na câmera"*.

Normalizar por range `(d-min)/(max-min)` daria o mesmo, porque `min = 0`. O que resolveria seria
percentil, ou não normalizar. **Sugestão: inverter o default para `DepthNormalise=0`** — mas só
depois de uma medição limpa, que esta sessão não conseguiu fazer.

---

## 7. Os erros desta sessão — leia antes de mexer na bancada

### 7.1 Instalei uma pasta inteira para pegar um shader

Copiei `reshade-shaders\Shaders\iMMERSE\` inteira do FiveM para instalar o Launchpad. Vieram junto
`MartysMods_RTGI_DIFFUSE.fx` e `MartysMods_RTGI_SPECULAR.fx`. O ReShade compilou e **reescreveu o
preset ligando todas as técnicas que encontrou**:

```
Techniques=MartysMods_Launchpad, MartysMods_NEWGI_Diffuse, MartysMods_SPECGI_Specular, MartysMods_AntiAliasing, DLSS5_Neural_Feed
```

RTGI é global illumination por ray marching **em cima do depth buffer**. Num jogo de PS2 com
profundidade 0..0,002 ele produz sangramento de cor, brilho nas bordas e manchas nas áreas escuras —
que foi exatamente o que apareceu na tela, e que eu passei várias rodadas atribuindo ao nosso código.

**Regra: copiar só o arquivo necessário, e conferir o `Techniques=` do preset depois de qualquer
instalação de shader.**

### 7.2 Liguei o efeito no boot e derrubei o driver duas vezes

Pus `StartOn=1` para automatizar medições. As duas vezes deu `DXGI_ERROR_DEVICE_REMOVED`:

```
bridge: first full round trip done at 1920x974.
swapchain going away (resize 1) after 4 frames
bridge: rebuilding after a failure (attempt 1 of 10).
bridge colour-in: CreateTexture2D 1920x971 fmt 28 failed 0x887A0005
```

O PCSX2 redimensiona a swapchain ~4 frames depois do boot; a ponte se reconstrói no meio disso e o
device morre. **`StartOn=0` existe por esse motivo. Não mude.** Ligue com `Ctrl+End` só com o jogo
em cena estável.

Efeito colateral: driver AMD pós-TDR fica degradado até reiniciar a máquina. Depois disso o PCSX2
ficou marcando 50% de velocidade com GPU em 15%, o que não é normal.

### 7.3 Mudei mais de uma variável entre observações

Troquei `Scale` de 1.0 para 0.5 na mesma janela em que entrou o history por passe. As medições
ficaram assim, e não distinguem nada:

| Scale | raster | entrada | resíduo | resíduo/entrada |
|---|---|---|---|---|
| **1.00** | 1920x971 | 0.2279 | 0.0242 | **0.11** |
| 0.50 | 960x486 | 0.3370 | 0.3844 | **1.14** |
| 0.50 | 960x486 | 0.3330 | 0.3738 | **1.12** |
| 0.50 | 960x486 | 0.0966 | 0.1189 | **1.23** |

A correlação com `Scale` é forte, mas está confundida com a mudança de código. **Fica em aberto:
a rede pode não funcionar bem a 960x486** — o projeto do lmxxf tem faixas fixas (720/900/1080) e
acolchoa a entrada, o que sugere que ela espera tamanhos específicos. Testar `Scale` isolado.

### 7.4 Não tirei snapshot do estado inicial da bancada antes de editar

Editei `dlss5-neural.ini` várias vezes por fora enquanto o overlay também autosalvava. Perdi a
noção de qual valor era do usuário e qual era meu. Só deu para restaurar porque a bancada já tinha
backups antigos por acaso.

**Regra: copiar o ini e o addon ativo antes do primeiro `Set-Content`.**

### 7.5 A causa final do "bugado" nunca foi isolada

Com o build original (`models`, 578048), ajustes de fábrica, nenhum shader instalado, a imagem
continuava errada. Isso **descarta todo o código desta sessão**. Restou: driver degradado pelos
TDRs, ou a configuração de 3 passes / meia resolução / só cor que o overlay tinha acumulado
(`⚠ A rede está rodando só com cor`, Passes 3, Scale 0.50). Não foi fechado.

---

## 8. Estado em que a bancada foi deixada

Restaurado a partir do backup do próprio bench, de **17:56** — antes desta sessão (18:07).

```
dlss5-neural.addon64   578048 bytes, 18:00:41   (o "models" da v0.6.0)
dlss5-neural.ini       de dlss5-neural.ini.normal (17:56)
  StartOn=0  Scale=1.13  Passes=1  Style=0
  Intensity=1  ResidualLimit=0.25  MotionScale=1  Guard=2
  Depth=1  History=1  Motion=1  GameGuides=1  Encoding=0
reshade-shaders\       vazio (nada instalado)
ReShade.ini            EffectSearchPaths de volta a '...\Shaders\**\**'
RELEASE-v0.5.3.addon64 de volta (eu tinha renomeado para .off)
```

Nada foi apagado. O que saiu do caminho:

| onde | o quê |
|---|---|
| `reshade-shaders\_removido-por-mim\` | `Shaders\` e `Textures\` que eu copiei (Launchpad, RTGI, SMAA, o `.fx`) |
| `reshade-shaders\_desligados\` | RTGI Diffuse, RTGI Specular, SMAA |
| `ReShadePreset.ini.criado-por-mim` | o preset que eu escrevi |
| `dlss5-neural.addon64.latest` e `.feed` | build final desta sessão (588 800) |
| `dlss5-neural.addon64.prev-active` | o que estava ativo quando cheguei |
| `dlss5-neural.ini.bak-antes-de-voltar` | o ini antes de eu começar a restaurar |

**`RELEASE-v0.5.3.addon64` colide com o nosso pelo mesmo nome de add-on** (`Failed to register
add-on, because another one with the same name ("dlss5 neural") was already registered`). É ruído
no log, não quebra nada, mas vale tirar da pasta para um teste limpo.

---

## 9. Ordem sugerida para a próxima sessão

1. **Reiniciar a máquina** antes de qualquer medição. Dois TDRs nesta sessão.
2. **Confirmar a linha de base:** build `models`, ajustes de fábrica, efeito ligado com `Ctrl+End`
   em cena estável. Anotar `measure, residual` e comparar com 0,0058–0,013 do handoff anterior. Se
   não bater, o problema é anterior a tudo isto e é o que deve ser perseguido primeiro.
3. **Só então** testar `Scale` isolado: 1.00 contra 0.50, mesma cena pelo save state, nada mais
   mudando. É a variável mais suspeita que sobrou (§7.3).
4. **Depois** trazer a branch `feed` de volta, um commit por vez, medindo entre cada um.
5. Se o effect for testado: copiar **só** `MartysMods_LAUNCHPAD.fx` + `MartysMods\*.fxh` +
   `Textures\iMMERSE\iMMERSE_bluenoise_opt.png`, e conferir o `Techniques=` do preset depois.

---

## 10. O que não repetir

- Instalar pasta de shaders inteira. Só o arquivo necessário, e conferir o preset depois.
- `StartOn=1` nesta bancada. Nunca.
- Mudar duas variáveis entre duas medições.
- Editar o ini por fora enquanto o overlay está aberto — ele autosalva por cima.
- Atribuir um artefato visual ao nosso código antes de confirmar que ele aparece com o efeito
  desligado.
