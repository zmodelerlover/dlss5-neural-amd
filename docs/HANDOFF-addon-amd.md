# Handoff — addon de ReShade com neural rendering em AMD

Escrito em 09/09/2026, ao fim da sessão que fez a rede sair de "efeito exatamente zero" para
"efeito medido". Substitui nada; complementa `PROJETO.md` e o plano
`cryptic-coalescing-raccoon.md`, que continuam válidos para a rota OptiScaler/FSR.

---

## 1. Estado atual, em uma frase

A rede DLSS-NR roda numa Radeon 9070 XT, dentro de um addon de ReShade, em D3D12, no PCSX2, em
modo inline, e o resíduo que ela produz é **medido e diferente de zero**. É só isso que está
provado. Tudo o mais é hipótese.

### O que está medido

PCSX2 / God of War / back buffer 1920x974 / escala 0.50 / 1 passagem / RX 9070 XT:

| | |
|---|---|
| entrada da rede, média absoluta | 0.0259 |
| resíduo, diferença média | 0.0165 (era `0.000000` exato) |
| resíduo, diferença máxima | 0.777 |
| tempo por job | 15-16 ms |
| frames com correção fresca | 2184 de 2473 |
| spin usado / teto | 57.741 / 1.223.118 iterações |

### O que a rede recebe hoje

**Só cor.** Sem profundidade, sem motion, sem jitter, sem exposição. É um passe espacial sobre
um back buffer de 8 bits já tonemapado.

Confirmado por engenharia reversa que **a rota NVIDIA no PCSX2 recebe exatamente o mesmo**: os
strings do `renodx-dlss.addon64` mostram o modo `presentation backbuffer with dummy temporal
inputs`, selecionado quando `Require DLSS` está desligado. A resposta fraca em pele e
personagem é do contrato de entrada, não do port AMD.

---

## 2. Onde está cada coisa

| caminho | o que é |
|---|---|
| `D:\dlss5` | projeto do addon. Repositório git, remoto `zmodelerlover/dlss5-neural-amd` |
| `D:\dlss5\src\neural\neural.cpp` | o addon inteiro |
| `D:\dlss5\src\probe\probe.cpp` | sonda de render targets. **Foi ela que achou a profundidade do PCSX2** |
| `D:\dlss5\tools\patch_runtime.py` | reconstrói o runtime sem o teto do spin |
| `D:\dlss5\build.ps1` | build sem projeto do VS; descobre o toolchain sozinho |
| `D:\dlss5\external\reshade\` | headers do ReShade + `imgui.h`/`imconfig.h` da tag `v1.92.5-docking` |
| `D:\v17s` | árvore do OptiScaler-DLSSNR-PreSR-Multipass com os patches do `AmdPreSr.cpp` |
| `D:\v17s\version.dll` | runtime **original**, sem patch nenhum. É a entrada do `patch_runtime.py` |
| `D:\v17s\dlssnr_amd_nocap.dll` | runtime reconstruído sem o teto |
| `D:\v17s\analysis\runtime-patches.json` | a especificação dos cinco patches, com offsets e bytes |
| `D:\DLSS-NR-Project` | dossiê original do projeto |
| `D:\pcsx2-v2.3.14-windows-x64-Qt` | instalação de teste, com o addon montado |

### Hashes que importam

| sha256 (16 primeiros) | o que é |
|---|---|
| `106223723fd9266c` | `version.dll` original, entrada do patcher |
| `fe96f5897861602d` | runtime com os cinco patches, **inclusive o teto**. É o que o OptiScaler instala |
| `81efaadc8d0deaa2` | runtime com quatro patches, **sem o teto**. É o que o addon usa hoje |
| `07a1e2ca3fbf6c9c` | `dxgi.dll` da build v1.2 do OptiScaler, a que funcionava no GTA |

### Backups feitos nesta sessão

- `D:\pcsx2-...\runtime-com-teto\` — as três DLLs com teto, para voltar atrás
- `%USERPROFILE%\Documents\PCSX2\inis\PCSX2.ini.bak-dlss5`
- `%USERPROFILE%\Documents\PCSX2\gamesettings\SCUS-97399_*.ini.bak-dlss5` (GoW 1)
- `%USERPROFILE%\Documents\PCSX2\gamesettings\SCUS-97481_*.ini.bak-dlss5` (GoW 2)
- `D:\pcsx2-...\ReShade.ini.bak-dlss5`
- `D:\SteamLibrary\...\Grand Theft Auto V Enhanced\v17-experimento-20260909\` — estado v1.7 do GTA

---

## 3. Como o addon funciona

```
back buffer → preparação de cor → raster da rede → [rede] → resíduo → composição → back buffer
```

- **Gancho: `present`.** Não `reshade_finish_effects`. Esse último só dispara quando o ReShade
  tem efeito carregado, e numa instalação sem shaders nunca é chamado. Custou uma noite.
- Só a correção da rede é reamostrada; a imagem cheia volta intacta. Escala menor custa detalhe
  na correção, não na imagem.
- Formato: núcleo único + tabela `kTargets`, uma linha por alvo. É o formato do RenoDX.
- Em qualquer API que não seja D3D12 o addon se declara indisponível e sai do caminho.

### Offsets do runtime que o addon escreve

Descobertos pelo `AmdPreSr.cpp` do fork e reaproveitados. Válidos **apenas** para o binário de
7.156.224 bytes cujo hash o addon verifica.

| RVA | o que é |
|---|---|
| `0x764c8` | `ID3D12Device*` |
| `0x764d0` | `ID3D12CommandQueue*` |
| `0x764d8` | contexto passado ao init |
| `0x76f20` | índice do dispositivo HIP |
| `0x76be0` | 1 = inline, 0 = async |
| `0x76c8c` | interop por memória externa |
| `0x76e1c` | habilitado |
| `0x76e1d` | armar avaliação neste frame |
| `0x76e1e` | entradas fornecidas por nós |
| `0x76e1f` | usar profundidade |
| `0x76e20` | tonemap: -1 auto, 0 desligado |
| `0x76e10` / `0x76e14` | profundidade invertida / convenção explícita |
| `0x76e30` / `0x76e34` / `0x76e38` | tone / structure / skin |
| `0x765f0` / `0x765f8` | view do histórico / flag de histórico válido |
| `0x76c14` | contador de jobs concluídos |
| `0x76c18` | contador de timeouts |
| `0x76c68` | ponteiro para a palavra de abort do watchdog |
| `0x76d68` | command list pendente. **É o contrato de aceitação da gravação** |
| `0x76d74` | id do job atual |
| `0x767f8` / `0x767fa` | pronto / falha nativa |
| `0x12380` | `Init(void*, const std::string*)` |
| `0xa0b0` | `Record(Packet*)` |
| `0x4640` | `Notify(queue, n, lists)` |
| `0xc520` | shutdown dos workers |

---

## 4. Os bugs que custaram a sessão, e a causa raiz de cada um

Registrados porque todos eram invisíveis sem instrumentação, e qualquer um deles sozinho fazia
o efeito ser zero.

1. **Gancho errado.** `reshade_finish_effects` não dispara sem shader carregado.
   → passou para `present`.
2. **`Notify` com lista vazia.** Eu chamava `Notify(queue, 0, nullptr)`. O engine casa a lista
   pendente dele com o array recebido; com zero listas nunca casa, e o worker nunca é publicado.
   → passa `(queue, 1, &cmd)`.
3. **Notificar depois de submeter.** O worker precisa já estar esperando quando a captura roda
   na GPU. Avisado depois do flush, ele chega tarde e o job não conclui.
   → `Notify` imediatamente **antes** do `flush_immediate_command_list()`.
4. **Recusa tratada como fatal.** O engine aceita **uma avaliação pendente por vez**; gravar por
   cima é ignorado em silêncio. Eu desligava tudo na primeira recusa, no frame 4.
   → recusa é pulo, não erro. Portão antes de gravar, com escape de 500 ms.
5. **Resíduo velho em frame novo.** Nos frames pulados eu devolvia a imagem crua; alternar cru e
   composto **é** o flicker. Aplicar o resíduo anterior tira o flicker mas arrasta.
   → resolvido de verdade pelo item 6, que permite inline.
6. **O teto do spin.** O quinto patch do instalador do OptiScaler reescreve o shader de espera
   de `i < maxIter` para `i < min(maxIter, 262144u)`, cerca de 6 ms a ~4000 iter/ms. A rede leva
   16 ms a meia escala e 125-187 ms cheia. Inline dava timeout **todo frame**, e outro patch do
   mesmo conjunto faz timeout preservar a entrada. Saída bit-idêntica à entrada.
   → runtime reconstruído sem esse patch. É o `patch_runtime.py`.
7. **Encoding errado.** `scRGB-nl` sobre back buffer SDR de 8 bits lineariza uma imagem que já é
   sRGB e ainda escala por 203/branco. Média de entrada 0.0064, praticamente preto.
   → `Encoding = sRGB` em fonte SDR. Entrada subiu para 0.0273.
8. **Tonemap automático.** Em `-1` o engine olha o formato, vê FP16, conclui linear HDR e aplica
   tonemap + auto-exposição sobre imagem já tonemapada. Duplo tonemap.
   → `0x76e20 = 0`.

**A lição transferível:** a medição de resíduo (entrada vs saída da rede, num frame, escrita no
log) foi o que separou "a rede não faz nada" de "a rede faz e a composição apaga". As duas
coisas são idênticas na tela e pedem correções opostas. Ela está no addon e deve continuar lá.

---

## 5. O que não funciona, e o que se sabe do porquê

| | situação |
|---|---|
| **Profundidade** | O PCSX2 escreve `512x512 R32G8X24_TYPELESS` de verdade; a sonda acha. Mas `bind_render_targets_and_depth_stencil` do ReShade **não chega ao addon** no caminho D3D12, nem assinando os eventos de draw como a sonda faz. Toda a conversão e o alias typeless estão escritos, atrás da chave `Depth` no menu, desligados. |
| **Motion** | Não existe no PS2 e não vai existir. Jogos da época não computavam movimento por pixel. |
| **Exposição** | Não é alimentada; o engine auto-expõe. |
| **Upscaling** | Não há caminho no runtime AMD: o `Packet` não tem recurso de saída, a rede escreve na própria textura de entrada. |
| **Model A/B/C** | Não há campo de estilo no engine AMD. |
| **UI Correction, máscara de personagem** | São flags do runtime NGX. O offset de `UseAutoMask` não foi localizado. |

---

## 6. Roadmap, por valor decrescente

### 6.1 Ligar a profundidade (maior retorno)

É o único guia real disponível no PCSX2 e a rota NVIDIA entrega dummy nele. Ligar coloca o
projeto **à frente da referência** nesse alvo.

O bloqueio é o evento do ReShade, não o encanamento. Caminhos a tentar, nesta ordem:
1. Descobrir por que a sonda recebia o bind e o addon não. **Diferença conhecida ainda não
   testada: a sonda rodava com o PCSX2 em D3D11.** Vale confirmar se o evento chega em D3D11 e
   não em D3D12 — se for isso, o guia da referência (§6.1) já dá a resposta: a rota NVIDIA usa
   PCSX2 em D3D11 com um device D3D12 auxiliar, e seria preciso a ponte D3D11→D3D12.
2. Alternativa sem o evento: rastrear `init_resource` e escolher pelo formato/tamanho.
3. Alerta de alinhamento: o depth é 512x512 e o back buffer 1920x974. Aspectos diferentes.
   Alimentar depth desalinhado pode ser pior que zero. Validar visualmente antes de confiar.

### 6.2 Trocar a rede

O DLSS-NR é denoiser de ray tracing. Para conteúdo de emulador, uma rede de restauração ou
super-resolução treinada em imagem de baixa resolução é a escolha certa. A arquitetura já toma
a rede como componente; trocar não exige mexer no resto. Foi decisão de projeto.

### 6.3 Testar fora do PCSX2

Um jogo D3D12 com contrato de upscaler entrega cor, profundidade e motion reais — tudo o que a
rede foi feita para receber. É onde ela deve parecer o que aparece nos vídeos. **Ninguém testou.**

### 6.4 Motion estimado

Optical flow. O §11.4 do dossiê já avaliou como categoricamente pior que um dispatch real. O
objetivo é medir o quanto pior, não presumir que resolve.

### 6.5 Multipass e escala acima de 100%

Implementados no addon (1-3 passagens, escala 0.25-2.00) mas **não avaliados** depois das
correções de encoding e do teto do spin. Todos os testes de passagem foram feitos enquanto o
resíduo era zero, então não valem nada. Vale refazer.

---

## 7. Armadilhas de configuração

Duas custaram uma noite cada. Registradas para não repetir.

1. **PCSX2 tem configuração por jogo** em `Documents\PCSX2\gamesettings\<SERIAL>.ini`. Uma
   linha `Renderer = 3` ali **vence a configuração global**. O God of War (`SCUS-97399`) estava
   travado em D3D11 e o addon era ignorado inteiro. `15` é D3D12.
2. **O ReShade desabilita addons no `ReShade.ini`.** Uma linha
   `DisabledAddons=dlss5 neural@dlss5-neural.addon64` em `[ADDON]` faz o addon nunca carregar,
   sem erro nenhum no log.

Outras:
- O `imgui.h` tem que ser **exatamente** 19250 e do branch **docking**. Versão próxima compila
  e desalinha a tabela de funções em silêncio.
- Rodar `dlssnr_amd_pass*.dll` que não seja o hash esperado com estes offsets trava o jogo. A
  verificação de hash no addon é proposital.

---

## 8. Estado dos outros alvos

| alvo | estado |
|---|---|
| **GTA V Enhanced** | **Restaurado à build v1.2**, que era a que funcionava bem. `dxgi.dll` = `07a1e2ca…`, ini original sem `WorkingScale`. Não tocar sem motivo. O estado v1.7 está em `v17-experimento-20260909\`. |
| **NBA 2K27** | Com a build de patch6 (`11d00682…`). Dela, só a correção de exposição typeless se provou. Os travamentos de 3,4 s continuam — são do handshake inline do OptiScaler, não do custo da rede. |
| **NFS 2015** | Tier A medido pela sonda (depth `R32G8X24`, colour `R11G11B10`, motion `R16G16_FLOAT` 67 draws). DX11, então fora do alcance deste addon. Precisa da rota de recursos. |
| **RPCS3** | Nunca testado. |

---

## 9. Comandos úteis

```powershell
# compilar o addon
cd D:\dlss5 ; .\build.ps1 -Target neural

# reconstruir o runtime sem o teto do spin
python tools\patch_runtime.py D:\v17s\version.dll D:\v17s\analysis\runtime-patches.json saida.dll

# rodar o PCSX2 com um jogo, sem interação
D:\pcsx2-v2.3.14-windows-x64-Qt\pcsx2-qt.exe -batch -- "D:\iso ps2\God of War 1 Versão IA.iso"
```

Logs a olhar, na pasta do executável:
- `dlss5-neural.log` — o addon: HIP, rasters, ritmo, e a medição do resíduo
- `dlssnr_on_amd.log` — o engine: modo, staging, tempo por job, auto-exposição, FAULTs
- `ReShade.log` — se o addon carregou, e qual API o processo criou

---

## 10. Publicação

Repositório: `https://github.com/zmodelerlover/dlss5-neural-amd` — público, MIT, um commit,
release `v0.1.0` com o `.addon64`.

**Não vão para o repositório, e o `.gitignore` barra:** `dlssnr_amd_pass*.dll`,
`dlssnr_on_amd_weights.bin`, `renodx-dlss.addon64`. Os pesos são derivados da NVIDIA e o runtime
vem de um projeto de terceiro sem licença declarada. Isso é o bloqueio que o plano original já
tinha registrado para o dia em que o projeto saísse do uso pessoal.
