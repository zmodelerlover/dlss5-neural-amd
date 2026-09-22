# HANDOFF — Launchpad na ordem certa, um runtime por passe, e o latch da validação, 21/09/2026

Sessão seguinte à `HANDOFF-feed-launchpad-20260921.md`. Diferente daquela, **tudo aqui foi
validado em jogo** (PCSX2, God of War 2, D3D11), com o log dos dois lados como evidência. Três
defeitos reais foram encontrados e corrigidos, e um quarto ponto (NR Preset) foi fechado como
impossível nesta DLL.

Leia a §6 antes de instalar em outro jogo, e a §7 antes de repetir qualquer medição.

---

## 1. Onde está tudo

| | |
|---|---|
| Repo de trabalho | `C:\Users\claudinhh\Desktop\dlss5amdrework\repo`, branch **`feed`** |
| Estado do fonte | os 9 commits da branch `feed` **+ um diff não commitado** de 4 arquivos (§4). O pacote `.zip` desta sessão foi construído desse estado. |
| Build entregue | `dlss5-neural.addon64`, 594 944 bytes |
| Runtime AMD | `dlssnr_amd_pass1.dll` v0.3.0, 7 290 880 bytes, sha256 `70af3fb7...` (bate com `tools/SHA256SUMS.txt`) |
| Pesos | `dlssnr_on_amd_weights.bin`, 147 689 451 bytes, sha256 `6bf8dc93...` |
| Bancada | `D:\pcsx2-v2.8.2-test`, GoW2 `SCUS-97481`, D3D11, upscale 5x, ReShade 6.8.0.2155 |
| Referência externa | `https://github.com/MatheusGViana/dlss-5-amd-project` — fork do OptiScaler com backend AMD (`OptiScaler/dlssnr/amd/AmdPreSr.cpp`). Faz um módulo do runtime por passe, que é o que foi replicado aqui. |

---

## 2. O que foi descoberto (fatos, com evidência)

### 2.1 O ReShade dispara o `present` do addon ANTES de renderizar os effects

No fonte do ReShade (`source/dxgi/dxgi_swapchain.cpp`, caminho D3D11): primeiro
`invoke_addon_event<addon_event::present>`, depois `present_effect_runtime`, que é quem chama
`runtime::render_effects`. O addon roda a rede dentro do `present`, então quando lia
`DLSS5N_MV` a textura ainda tinha o fluxo do **frame anterior**. A rede recebia a cor do frame N
com vetores do N−1. Um Launchpad de 8 níveis atrasado um frame mede igual ao estimador interno,
que foi exatamente a queixa que abriu a sessão.

`runtime::render_effects` recusa rodar duas vezes no mesmo frame (`_effects_rendered_this_frame`),
e aceita ser chamado por um addon com o RTV do back buffer ("behaves as if this was called from
within present"). Com `rtv == 0` ele retorna sem desenhar, então precisa de uma view real.

### 2.2 O runtime AMD é uma instância só, e isso quebra o history com 2+ passes

Pelo IDA, no `dlssnr_amd_pass1.dll`:

- `0x97090` (kHistory) só é lido pelo **worker** (`sub_180018670`), em tempo de execução, não no
  `RecordFn`. Por isso o modo serial inline (que espera o passe terminar antes de escrever o
  ponteiro do próximo) era o que tornava o history por passe válido no addon.
- O estado temporal do runtime mora em globals do módulo e num único objeto de engine
  (`kEngineObject`), com buffers HIP alocados nele: auto exposure (`autoExpo` no dump), o kernel
  `_Z11k_reproject12ReprojParams`, e um "post history" interno que o env `DLSSNR_NOPOSTHIST`
  desliga. Com 2 passes por frame esse estado vê pass1, pass2, pass1, pass2… e o "anterior" de
  cada passe é a saída do outro estágio.
- Isso bate com a observação do usuário: 1 passe + history limpo; 2 ou 3 passes com history,
  ruído nas iluminações. O addon já entregava `history[slot]` certo por passe; o problema era
  dentro do runtime.

O fork do Matheus resolve carregando `dlssnr_amd_pass1.dll`, `pass2.dll`, `pass3.dll` como
**módulos separados** (`InitPass(i)`, "Initialized independent AMD pass N"), e publica cada um
só depois de o worker do anterior terminar ("All runtimes use HIP stream 0"). É por isso que o
arquivo do runtime tem `pass1` no nome. O `DllMain` do runtime (`sub_180005440`) abre o próprio
log, lê o ini, instala `SetUnhandledExceptionFilter` + handler vetorado e sobe uma thread própria;
nada disso impede uma segunda carga.

### 2.3 A validação do feed tinha um latch que zerava todo movimento acima de 2,8 px

No `DLSS5_Neural_Feed.fx`, o passe `History` guardava em `DLSS5N_PrevMV` o vetor **já
validado**. O teste de consistência zera o vetor atual se `|mv − prev| > 1,4 + 0,5·|mv|` px.
Com `prev = 0`, qualquer `|mv| > 2,8` px falha e é zerado, e o zero se propaga para o frame
seguinte. Bastava um frame zerado por qualquer motivo (o começo do movimento, por exemplo) e todo
pan acima de 2,8 px/frame sumia enquanto durasse.

Medido no log do runtime (`job N motion: mean |mv|`), maior componente por amostra:

| sessão | fonte do movimento | máx | média |
|---|---|---|---|
| anterior | estimador interno | 17,9 px | 3,27 px |
| esta, antes do fix | Launchpad, validado | **2,12 px** | 0,59 px |
| esta, depois do fix | Launchpad, validado | 10,7 px | 1,58 px |

O teto de 2,12 px logo abaixo do limiar de 2,8 px é a assinatura do latch.

### 2.4 NR Preset não tem como funcionar nesta DLL

`DLSSNR.Hint.Render.Preset` escolhe um **conjunto de pesos**. A `nvngx_dlssnr.dll` tem exatamente
um (`WEIGHTS_HT`) e ela mesma loga `preset %d is not available in this DLL build; falling back to
shipping default`. Não há segundo cérebro para carregar, em nenhuma plataforma. O que existe e já
está implementado são os Modelos A/B/C (`Style`), que são grading de cor sobre o frame pronto.
Detalhe completo em `docs/styles-model-abc.md`.

### 2.5 Miudezas que custaram tempo

- **`EffectSearchPaths=.\reshade-shaders\Shaders\**\**`** (com `\**\**` duplo) faz o ReShade
  não achar effect nenhum e logar `Failed to resolve search path ... error code 123`. O certo é
  `\**` simples, como na instalação do FiveM. A bancada estava assim desde a sessão anterior.
- **Preset do ReShade 6.x**: `Techniques=` e `TechniqueSorting=` ficam na **seção sem nome** no
  topo do arquivo, não em `[GLOBAL]`. Um `[GLOBAL]` é ignorado e o ReShade ordena por nome de
  arquivo, o que pôs `DLSS5_Neural_Feed` **antes** do Launchpad — de novo um frame atrasado.
- O `TechniqueOn()` do addon devolvia estado oscilante na primeira rodada (15 flips em 400
  frames). Não voltou a acontecer depois de o preset ser escrito certo; a linha de status agora
  imprime frame e estado bruto das duas técnicas para diagnosticar se reaparecer.
- A visão de debug "Vetores de movimento" saturava em ±32 px. Um pan real de 3 a 5 px virava
  cinza quase liso, enquanto os 134 px de lixo do estimador pareciam "campo forte". Agora satura
  em 8 px.

---

## 3. O que entrou de código (diff não commitado sobre `73d152c`)

### `src/neural/neural.cpp`

- **`RenderEffectsAheadOfNetwork(dev, back)`**: quando `DLSS5_Neural_Feed` está ligado, chama
  `g.effects->render_effects(...)` no início do `BridgePresent`, antes de copiar o back buffer,
  com um RTV criado para o back buffer (criado e destruído por frame). Marcado `SelfIssued`
  porque o ReShade reporta os binds da própria cadeia de volta pelo `OnBindDepthStencil`, que
  pega `g.lock` — já segurado no present. Só na rota D3D11. Log:
  `effects: DLSS5_Neural_Feed is on, so ReShade's chain now runs before the network`.
- **Um módulo do runtime por passe**: `g.runtimes[kMaxPasses]`, `g.lastJobs[]`,
  `g.recordedMask`, `g.runtimeFile`. `ArmRuntime(h)` (o que era o miolo de `InitEngine`),
  `LoadExtraRuntime(slot)` (copia o runtime já carregado para `dlss5-pass{N}.dll` ao lado do exe
  e carrega como módulo próprio), `BringUpEngines(UINT &wanted)` (sobe as cópias sob demanda e
  **limita `wanted`** ao que subiu, logando por quê). Helpers `RuntimeFor(slot)`,
  `RuntimeBusy()` (contador de job de todos os módulos), `ResetJobs()`,
  `NotifyRuntimes(queue, n, lists)` (notify só para os módulos que gravaram naquela lista).
  As cópias só existem em **modo serial inline** (o padrão); batch e async continuam com um
  módulo só. Cada cópia carrega os pesos de novo: ~150 MB de VRAM por passe e um engasgo de
  1–2 s na primeira vez que o Pass Count sobe. Log por passe:
  `pass 2: running through its own copy of the runtime (dlss5-pass2.dll)`.
- Linha de status do feed com `[frame N: feed handle/state, launchpad handle/state]`.
- Visão de debug 4 (`Motion vectors`) satura em 8 px; texto de ajuda EN/PT atualizado.

### `src/neural/vk_route.inc`, `src/neural/gl_route.inc`

Só a troca das expressões de job/notify pelos helpers acima. Comportamento idêntico com um
módulo.

### `shaders/DLSS5_Neural_Feed.fx`

`PS_History` grava `mv = ProviderMV(uv)` (vetor cru do provider) em vez do vetor validado.
Resolve o latch da §2.3.

### Verificadores

```
.\build.ps1 -Target neural              -> OK (594 944 bytes)
python tools\runtime_offsets_check.py   -> PASS
python tools\feed_fx_check.py           -> PS_Guides 3 targets, PS_History 4 targets
```

---

## 4. Resultados medidos em jogo

- Feed adotado e estável: `DLSS5_Neural_Feed.fx: enabled; motion from the effect, depth from the
  game [frame 0: feed handle 1 state 1, launchpad handle 1 state 1]`.
- `guide probe, motion (from the effect): 2% exactly still, mean |d| 0.334 px, max 11.84 px`
  contra `95% still, max 134.8 px` do estimador.
- Pass Count 2 e 3 com history: `pass 2: running through its own copy…`, `pass 3: …`,
  `network job N done … history on, zero-copy` para cada módulo (os três escrevem no mesmo
  `dlssnr_on_amd.log`, os ids de job aparecem intercalados — normal). **O usuário confirmou que o
  ruído nas iluminações com 2 e 3 passes sumiu.**
- Depois do fix do latch, a rede recebeu até 10,7 px de movimento (era 2,12).

---

## 5. O que NÃO está estabelecido

- Se o campo do Launchpad no PS2 é **bom o bastante** além de "melhor que o estimador". Ele
  filtra o fluxo de forma bilateral pela profundidade, e a profundidade que o ReShade vê no PS2
  é praticamente plana (0 a 0,002). O filtro vira um borrão espacial. Isso é do Launchpad. Flow
  Quality foi posto em High no preset da bancada; não foi medido contra Low.
- Se as cópias do runtime se comportam em **jogos D3D12/Vulkan nativos**: as rotas usam os
  mesmos helpers, mas só a D3D11 (bridge) foi testada.
- O fork do Matheus zera a flag de history do runtime em resize, troca de guia e pausa maior
  que 250 ms. Aqui isso é coberto pelo `historyValid` do addon; os casos de pausa não foram
  medidos.
- Tudo que os handoffs anteriores deixaram em aberto continua: `Scale` < 1,0 sem medição limpa,
  `DepthNormalise=1` provavelmente errado para o PS2 (três sinais contra), exposição `nullptr`.
- **Bridge 32-bit (`dlss5-neural.addon32` + `host64.exe`) não foi reconstruída** nesta sessão.
  Nenhuma das mudanças a toca, mas ela também não ganha nada: nem o feed nem as cópias por passe.

---

## 6. Instalar em outro jogo — o que importa

Ordem que funcionou, além do `docs/install.md` de sempre:

1. ReShade **with full add-on support**, 64-bit, na API certa do jogo.
2. `dlss5-neural.addon64` + `dlssnr_amd_pass1.dll` + `dlssnr_on_amd_weights.bin` na pasta de
   onde o jogo renderiza (a mesma do proxy DLL do ReShade).
3. Shaders: **só os arquivos necessários**, nunca a pasta iMMERSE inteira (a sessão anterior
   copiou tudo, o ReShade ligou RTGI junto e o resultado foi atribuído ao nosso código por horas):
   ```
   reshade-shaders\Shaders\ReShade.fxh
   reshade-shaders\Shaders\ReShadeUI.fxh
   reshade-shaders\Shaders\DLSS5_Neural_Feed.fx
   reshade-shaders\Shaders\iMMERSE\MartysMods_LAUNCHPAD.fx
   reshade-shaders\Shaders\iMMERSE\MartysMods\mmx_{global,depth,math,camera,deferred,texture,hash}.fxh
   reshade-shaders\Textures\iMMERSE\iMMERSE_bluenoise_opt.png
   ```
   Launchpad vem de `https://github.com/martymcmodding/iMMERSE` (não redistribuir junto).
4. `ReShade.ini`: `EffectSearchPaths=.\reshade-shaders\Shaders\**` e
   `TextureSearchPaths=.\reshade-shaders\Textures\**` (um `**` só).
5. `ReShadePreset.ini` **sem `[GLOBAL]`**, Launchpad antes do feed nos dois campos:
   ```
   Techniques=MartysMods_Launchpad@MartysMods_LAUNCHPAD.fx,DLSS5_Neural_Feed@DLSS5_Neural_Feed.fx
   TechniqueSorting=MartysMods_Launchpad@MartysMods_LAUNCHPAD.fx,DLSS5_Neural_Feed@DLSS5_Neural_Feed.fx
   ```
6. Depois de qualquer instalação de shader, **conferir o `Techniques=` que o ReShade regravou**.
7. `dlss5-neural.ini`: `FeedEffect=1` é o padrão; `Passes`, `History=1`, `Inline=1` (serial
   inline é o único modo com um módulo por passe). Não usar `StartOn=1` em bancada: PCSX2
   redimensiona a swapchain no boot e derruba o device.
8. Ligar com Ctrl+End em cena estável. Conferir no `dlss5-neural.log`:
   - `effects: DLSS5_Neural_Feed is on…` (ordem certa),
   - `DLSS5_Neural_Feed.fx: enabled; motion from the effect…` (feed adotado),
   - `guide probe, motion … (from the effect)` com poucos % "still" e máximo plausível,
   - com Passes ≥ 2: `pass N: running through its own copy of the runtime`.
   E no `dlssnr_on_amd.log`: `history on` em cada `network job`, e `job N motion: mean |mv|`
   subindo quando a câmera anda.
9. Para comparar Launchpad com e sem: desmarcar `MartysMods_Launchpad` no overlay do ReShade. O
   addon volta ao estimador na hora e rearma a medição 120 frames depois. A diferença é
   **temporal** (estabilidade das iluminações com a câmera andando), não aparece num frame
   parado nem no "Resíduo x8".

Um jogo que entrega o próprio velocity buffer continua preferido ao effect; o effect entra só
onde a observação de guias não acha nada (PCSX2, e a maioria dos jogos antigos).

---

## 7. Os erros desta sessão

- Heredoc com aspas simples dentro do Bash da sessão quebrou duas vezes; o patch foi para um
  arquivo `.py` e rodou de lá. Sem consequência no repo.
- O primeiro patch do runtime por passe tinha `g.lastJob = 0;` dentro do próprio `ResetJobs()`,
  e a substituição textual global o teria transformado em recursão infinita. O `assert` de
  contagem pegou antes de salvar. Vale a regra: **contar as ocorrências antes de substituir**.
- O preset foi escrito com `[GLOBAL]` uma vez (formato errado) e a primeira rodada mediu com o
  feed **antes** do Launchpad. Só o log do addon mostrou.

---

## 8. Estado em que a bancada foi deixada

```
D:\pcsx2-v2.8.2-test\
  dlss5-neural.addon64            594 944  (o build desta sessão)
  dlss5-pass2.dll, dlss5-pass3.dll         (criados pelo addon; podem ser apagados)
  RELEASE-v0.5.3.addon64.off               (renomeado para não colidir de nome)
  ReShade.ini                              SearchPaths corrigidos (backup ReShade.ini.bak-antes-feedtest)
  ReShadePreset.ini                        Launchpad + feed, OPTICAL_FLOW_Q=2
  reshade-shaders\                         só os arquivos da §6
  inis\PCSX2.ini                           vibração do controle desligada (backup .bak-antes-vibracao)
  dlss5-neural.ini                         Style=2, History=1, Passes conforme o último toque no overlay
```

Backups com carimbo: `dlss5-neural.ini.bak-*-antes-feedtest`, `dlss5-neural.addon64.bak-*`,
logs por rodada em `dlss5-neural.log.run1..run6` e `dlssnr_on_amd.log.run1..run6`.

---

## 9. Ordem sugerida para a próxima sessão

1. Commitar o diff da §3 (um commit por tema: ordem no ReShade; runtime por passe; latch do
   feed; debug view) e atualizar `docs/install.md` com a §6.
2. Testar um jogo D3D12 nativo com Passes=2 e history: é a rota que usa os helpers novos sem ter
   sido exercitada.
3. Medir Launchpad Low vs High no mesmo save state: `guide probe` + `mean |mv|`, nada mais
   mudando.
4. Decidir `DepthNormalise` para o PS2 com medição limpa (ver handoff anterior, §6).
5. Se quiser o comportamento do fork do Matheus em pausa/resize, zerar `historyValid` também em
   `gap > 250 ms`; hoje só alt-tab, minimizar e troca de swapchain fazem isso.
