# HANDOFF — 22/09/2026 (madrugada, sessão 2): a spike do ROCm, as chaves escondidas do runtime, e a paridade de pesos com a NVIDIA

Continuação direta de `HANDOFF-style-preset-fechado-e-roadmap-20260922.md`. Onde este contradiz
aquele, **este vence** — e ele contradiz em dois pontos que importam (§3 e §5).

Esta sessão **não mudou uma linha do addon**. O build que está instalado é byte a byte do mesmo
tamanho do de 00:45. O que mudou foi o mapa: o que dá pra ler, o que dá pra medir, e o que está
provado que não adianta tentar.

Leia a §8 antes de continuar o trabalho. É a lista de erros desta sessão, e três deles são da mesma
família do "quinto slot" que custou um dia.

---

## 1. Onde está tudo

| | |
|---|---|
| Repo | `C:\Users\claudinhh\Desktop\dlss5amdrework\repo`, branch **`feed`** sobre `73d152c` |
| Fonte | **nada commitado**, e esta sessão não acrescentou modificação nenhuma a `src/`. Os arquivos `M` no `git status` são todos da sessão anterior |
| Build | `repo\build\dlss5-neural.addon64`, **600 576 bytes** — mesmo tamanho do build de 00:45, porque a única mudança de código desta sessão foi feita e depois removida (§7) |
| Instalado | PCSX2 `D:\pcsx2-v2.8.2-test`. ETS2 e RDR1 não foram tocados |
| Novo no repo | `spike/rocm-custom-kernel/` (relatório + 5 checks runnable), `tools/carve_amd_kernels.py`, `tools/read_amd_weights.py`, `tools/knob_sweep.ps1` |
| Runtime analisado | `D:\pcsx2-v2.8.2-test\dlssnr_amd_pass1.dll`, sha256 **`70af3fb757f83f71…`** — este é o valor certo, ver §8.1 |
| Pesos | `D:\pcsx2-v2.8.2-test\dlssnr_on_amd_weights.bin`, 147 689 451 bytes, magic `DLSSNRW1`, 153 tensores |
| Toolchain | ROCm **7.1** em `C:\Program Files\AMD\ROCm\7.1`. Não existe ROCm 10 nesta máquina, e nada da spike precisou de superfície mais nova |

---

## 2. O que esta sessão fez

1. Testou a tese "dá pra tirar a limitação do kernel HIP pré-compilado escrevendo um kernel custom
   com as APIs do ROCm". **Refutada** — mas o motivo pelo qual a §3.1 do handoff anterior tinha
   fechado o assunto também é falso (§4).
2. Descobriu que os kernels do port nunca foram opacos e escreveu a ferramenta pra ler (§4).
3. Provou que os pesos da AMD são os da NVIDIA, byte a byte (§3).
4. Achou seis chaves de ambiente não documentadas dentro do runtime e mediu duas (§6).
5. Construiu dois checkboxes pra controlar essas chaves ao vivo, mediu, e **removeu** (§7).

---

## 3. O achado maior: a rede da AMD é a da NVIDIA, byte a byte

`tools/read_amd_weights.py` parseia o arquivo de pesos e o parse se valida sozinho: o índice termina
exatamente em `data_base` (5673) e `data_base + soma(tamanhos)` dá o tamanho do arquivo, exato.

Contra o `nvngx_dlssnr.dll` (`Desktop\a\`, 165 840 496 bytes):

- os **153 nomes de tensor** aparecem literalmente dentro dele;
- amostra de **25 tensores: 25/25 byte-idênticos** lá dentro, de `block0.layer0.layer` (21 696 B) a
  `block37.layer1.layer` (4 196 352 B).

```
python tools/read_amd_weights.py <weights.bin> --nvidia <nvngx_dlssnr.dll>
```

### Por que isso muda o trabalho

`docs/nvidia-parity.md` termina com *"a aritmética é igual; se a imagem é, só a comparação diz"*, e
a §5 do handoff anterior registra que ninguém mediu a mesma cena nas duas máquinas.

**Metade disso está resolvida sem medir cena nenhuma: a rede é idêntica.** Não é "parecida", não é
"reimplementada" — é o mesmo tensor, os mesmos bytes. Então toda diferença que sobra na tela está
em três lugares, e nenhum deles é o modelo:

1. a composição em volta (nossa e a do RenoDX),
2. os quatro-versus-cinco inputs de condicionamento (§5),
3. a aritmética dos kernels fp8.

Isso mata de vez a hipótese "o port tem uma rede pior" e estreita o item 2 do roadmap de
"eles são iguais?" para "onde a composição diverge?".

---

## 4. Os kernels nunca foram opacos

A §3.1 do handoff anterior fechou o NR Style com *"os kernels HIP do port são GCN pré-compilado sem
fonte… Só quem compilou o port poderia acrescentar"*.

Sem fonte, verdade. Opaco, **não**.

O runtime carrega um clang offload bundle no offset de arquivo `0x98200`, nove alvos, ELFs AMDGPU
crus e sem compressão. Um code object AMDGPU guarda o metadado numa note section — ao contrário de
um cubin CUDA, ele **nomeia cada kernel e descreve cada argumento** sem uma linha de disassembly:

```
python tools/carve_amd_kernels.py <runtime.dll> out/
llvm-readobj --elf-output-style=GNU --notes out/gfx1201.co
```

Saem 34 kernels nomeados: `k_import`, `k_pre_block_1h_32_fp8`, `k_swin_var<32|64|128|256>`, `k_qkv`,
`k_qkv_attn`, `k_attention`, `k_ffwd`, `k_conv_res`, `k_expand`, `k_contract2`, `k_dec_upsample`,
`k_final_head`, `k_post_block_1h_32_fp8`, `k_export`, `k_reproject`, `k_repack`, `k_mean`, mais o par
de fence de GPU `k_flag_set`/`k_flag_wait`.

E dá pra **rodar os kernels deles**: `hipModuleLoadData` engole o ELF carvado cru, sem re-empacotar.
`spike/rocm-custom-kernel/check_module_load.cpp` resolve `_Z6k_mean10MeanParams` pelo nome mangled,
dirige com um struct reconstruído e devolve `0.07641130` contra referência de CPU `0.07641130`. Três
dentes independentes, cada um verificado saindo 1: símbolo errado, arquitetura errada, `h`/`w`
trocados (o número anda pra 0.05603288).

**Então "só quem compilou o port poderia acrescentar" é falso como afirmação de API.** O que fecha o
caminho é outra coisa, e está na §5.

---

## 5. O quinto input de condicionamento existe — e por que mesmo assim não resolve

`k_pre_block` monta um vetor de 16 lanes fp16 por pixel **em LDS** e multiplica pela primeira camada
linear. Lanes 0–2 ruído gaussiano, lane 3 o bias, lanes 4–9 as duas imagens de cor, lanes 11–14 os
quatro controles que o addon já dirige, lane 15 zerada na unha pelo kernel.

**A lane 10 é um input vivo que o host prega em zero** — uma instrução de 10 bytes,
`mov dword ptr [rbp+238h], 0` em RVA `0x2D79B`, preenchendo `PreParams+48`.

O peso treinado dela é real: não-zero em **todas** as 16 linhas da projeção 16×16, rms ≈ 0,22–0,27,
**acima dos quatro controles que rodam** (0,091 / 0,066 / 0,168 / 0,165). E há controle negativo
embutido: a lane 15, que o kernel zera, tem coluna **exatamente zero** nas mesmas 16 linhas — o
treinador zera coluna não usada, então a lane 10 não é artefato.

**Logo, §3.1 "a rede recebe 4 controles e nada mais" está medidamente errado.**

### As três razões de isso não ter virado pixel

1. **Kernel custom não alcança.** O vetor de 16 lanes vive **só em LDS** — `ds_store` em
   `0xE080 + tid*32`, consumido por `ds_load` na cadeia `v_dot2_f32_f16`. Nunca vai pra memória
   global. Nenhum kernel ao lado, nenhum hook de IAT, nenhum buffer D3D12 compartilhado e nenhuma
   ordenação de stream toca nele. As três rungs de interop passam e são **estruturalmente incapazes**
   de carregar a tese. O que alcança a lane 10 é um store do host num struct — kernel nenhum.
2. **O struct medido está atrás de uma variável de ambiente.**
   ```
   18002d6ba  80 3d 1f a9 06 00 01   cmp byte ptr [rip+0x6a91f], 1   ; -> 0x97fe0
   18002d6cb  0f 85 57 01 00 00      jne 0x2d828                     ; pula [0x2d6d1, 0x2d828)
   18002d79b  c7 85 38 02 00 00 ...  mov dword ptr [rbp+238h], 0     ; o zero-write do +48
   18002d81c  48 8d 0d 25 6b 03 00   lea rcx, [0x64348]              ; stub do k_pre_block
   18002d9aa  48 8d 0d b7 e2 03 00   lea rcx, [0x6bc68]              ; stub do k_swin_var<32,true>
   ```
   `0x97fe0` vem de `getenv("DLSSNR_SLOW_PREPOST")`. Sem a variável, o `jne` é tomado e o
   preenchimento inteiro do `PreParams`, o zero-write **e** o launch do `k_pre_block` são pulados.
   **O caminho que embarca é `k_swin_var<32,true>`, com um `VarParams` de 168 bytes que ninguém
   mapeou.** `k_pre_block` é caminho de debug.
   *Registrando: a §2.3 do handoff anterior chamou `off_18006BC68` de kernel pre e **acertou** — é o
   stub que o caminho default carrega. A leitura que "corrigiu" ela é que estava errada.*
3. **Ninguém sabe o que a lane 10 é.** Provamos que é *um* input de condicionamento, nunca *qual*.
   Chamar de Style é a mesma adjacência que custou a §2. A §4 do handoff anterior tem um rival que
   ninguém considerou: **`UICorrection`** — default 0 na NVIDIA, RenoDX manda 1, "AMD não tem o
   campo". Uma lane pregada em `0.0f` encaixa melhor nisso do que em `style/128`, que com n=3
   clampado a 2 só produz {0, 0,0078, 0,0156}.

### Uma mina, se alguém reabrir a rota de módulo

`g_e4m3_lut` (512 bytes) vem **toda zero** na imagem; o runtime preenche a cópia do módulo dele no
init. Resolvendo todo par `s_getpc_b64`+`s_add_u32` nos objetos carvados: gfx1201 e gfx1200 têm
**zero** sites (o compilador inlinou tudo), mas `gfx11-generic` tem 82, dos quais **79 caem
exatamente nela**, incluindo 9 dentro do `k_swin_var<32,true>`. Em RDNA3 — a maior parte da base de
usuários — rodar os kernels deles a partir de um módulo nosso dequantiza todo peso fp8 por uma
tabela de zeros, com `hipSuccess` em tudo e nenhum erro. Conserto: um `hipModuleGetGlobal` + 512
bytes antes de qualquer launch.

---

## 6. Seis chaves de ambiente escondidas no runtime, duas medidas

O runtime lê seis variáveis de ambiente. Cada string tem exatamente um xref; quatro gravam um byte
em `.data` via `setne`. Mecanismo lido no stream de instruções, não deduzido do nome:

| Variável | Byte | Lida em | Mecanismo |
|---|---|---|---|
| `DLSSNR_SLOW_PREPOST` | `0x97fe0` | `0x2d6ba`, `0x2f606` | troca o `k_swin_var<32,true>` fundido pelos `k_pre_block`/`k_post_block` dedicados |
| `DLSSNR_NOBLEND` | `0x97fe8` | `0x2f559` | `jne` pula `movaps xmm6,xmm7` — o coeficiente de blend do pós fica 0 |
| `DLSSNR_NOPOSTHIST` | `0x97ff0` | `0x2f572` | `cmove rsi,rax` é pulado — o pós recebe ponteiro de history nulo |
| `DLSSNR_WBLOG` | `0x98000` | `0x32b28` | log do carregamento de pesos |
| `DLSSNR_STAGES` | (valor) | `0x190f6` | não é booleana, a string é parseada num valor |
| `DLSSNR_NO_REPACK` | (inline) | `0x20315` | booleana, ramifica inline |

Polaridade: `setne` depois do `getenv` → byte **1 significa que a variável estava presente**, ou
seja, **o amortecimento foi removido**.

### A medição

God of War 2 a partir de save state, raster de rede 653x330, `Scale=0.34`, `Passes=1`. Duas
baselines dão o piso de ruído: **±6% no resíduo médio, ±4% no ratio**. O `input mean` variou 1,1%
entre as quatro runs, o que prova que a cena reproduziu.

| run | resíduo médio | vs baseline | ratio | vs baseline |
|---|---|---|---|---|
| baseline A | 0,031424 | — | 0,211 | — |
| baseline B | 0,029653 | −5,6% | 0,203 | −3,8% |
| `DLSSNR_NOBLEND` | 0,052057 | **+66%** | 0,268 | +27% |
| `DLSSNR_NOPOSTHIST` | 0,041107 | **+31%** | 0,247 | +17% |

**Leitura:** o runtime gasta entre um terço e dois terços da força do efeito comprando estabilidade
temporal. As duas chaves compram essa força de volta — e as duas trazem flicker visível.

`DLSSNR_SLOW_PREPOST` é útil como ferramenta de diagnóstico: é o único jeito de rodar o caminho cujo
struct está inteiramente mapeado (§5), então uma pergunta sobre um input de condicionamento pode ser
feita primeiro no caminho lento e só depois repetida no que embarca.

---

## 7. O que foi construído e removido

Foram feitos dois checkboxes no overlay escrevendo `0x97fe8` e `0x97ff0` por frame, com chave de ini,
tooltip nos dois idiomas e os offsets no `runtime_offsets.h`. Compilou, os quatro checks do projeto
passaram, e foi medido.

**Rejeitado pelo usuário: as duas chaves geram mais ruído do que o ganho de força justifica.**

Removido por completo — não é "desabilitado", é ausente. `runtime_offsets.h` idêntico ao HEAD,
checker de volta a 32 offsets, build de volta a 600 576 bytes. Revertido **à mão**, porque
`git checkout src/neural/neural.cpp` levaria junto o trabalho não commitado das sessões anteriores.

Duas coisas do experimento valem mais que ele:

- **Os bytes são mesmo ao vivo.** Escrever `0x97fe8` por frame moveu o resíduo +31,5% contra um piso
  de ±6%. O runtime relê a cada avaliação; não precisa relançar.
- **Mas o byte write não reproduziu o caminho da variável de ambiente** (0,0413 contra 0,0521), e
  **ninguém descobriu por quê**. Se isso for reaberto, essa lacuna é o primeiro item, não o flicker.

---

## 8. Erros desta sessão, e como corrigir

Seção pedida explicitamente. Três dos cinco são da mesma família do "quinto slot".

### 8.1 sha256 errado propagado (corrigido)
Afirmei que o runtime era `8321cae7…`. **É `70af3fb757f83f71…`.** O `8321cae7` é o
`original_sha256` do `tools/runtime-patches.json`, ou seja a **entrada** do `patch_runtime.py`, não
a saída. Peguei do handoff sem conferir e propaguei pra cinco agentes.

Os 35 bytes de diferença são exatamente os três patches declarados, nenhum em `.hip_fat`, então os
kernels carvados são os do fabricante e todos os RVAs continuam valendo.

**Consequência que fica:** `patch_runtime.py` recusaria o binário que foi analisado, e qualquer plano
de patch de byte também esbarra no `RuntimeHashMatches` do próprio addon (`neural.cpp:756-800`,
`kRuntimeSha256` em `neural.cpp:725`).
**Correção de método:** hash vem de `sha256sum` no arquivo, nunca de prosa de handoff.

### 8.2 Duas leituras se contradizendo, e a mais confiante ganhou sem medição
Um agente reportou que `k_pre_block` estava atrás de `DLSSNR_SLOW_PREPOST`. Outro "corrigiu" isso
formalmente, dizendo que o evaluate lança `k_pre_block` direto. O segundo estava errado, e como era
o mais assertivo, virou premissa de toda a escada.

Isso **não** é a §2.5 (uma leitura confiante sem medição). É uma variante pior: **a contradição
estava visível e ninguém a tratou como pedido de medição.**
**Correção de método:** duas leituras discordando não se resolvem por argumento. Quem discorda roda
o teste. Aqui o teste eram sete bytes: `cmp byte [0x97fe0],1` / `jne`.

### 8.3 Existência de feature deduzida de chave de ini
Propus o experimento do "amortecimento duplo" porque o ini da bancada tem `PersistentResidual`,
`ResidualHold` e `ResidualMaxAge`. **Nenhuma delas existe no fonte, em commit nenhum, nem em
qualquer dos dois binários atuais.** Só existem num binário antigo de 1,2 MB que não é desta árvore.
O addon **não tem amortecedor temporal próprio**; as chaves são lixo de um build velho.
**Correção de método:** antes de construir em cima de uma feature, `grep` no fonte E no binário. Um
ini é um artefato de runtime, não um contrato.

### 8.4 Métrica do ratio incompleta (corrigida)
Dei a regra *"ratio mexeu → a rede está computando coisa diferente"*. **Errado.** Tirar um filtro
**temporal** aumenta a variação local da correção sozinho, sem a rede computar nada diferente — foi
o que as duas chaves fizeram.

**Regra corrigida:** o ratio separa um ganho chapado de uma mudança estruturada. **Não** separa
mudança temporal de mudança de condicionamento. Quem mostrou isso foi o olho do usuário vendo
flicker, não a métrica.

### 8.5 Automatizar um teste cujos modos de falha são visuais
Automatizei as runs de jogo. Três sessões se perderam em tela preta, TDR e save state incompatível,
e eu interpretei log depois do fato sem ver a tela. O custo caiu na máquina do usuário.
**Correção de método:** quando o modo de falha é visual, quem dirige é quem enxerga. Automatizar o
**arreio** (setar variável, arquivar log, tabular) está certo; automatizar o **julgamento**, não.

### 8.6 Generalização a partir de um caso
Disse "todos os save states são de versão antiga do PCSX2" depois de ver um erro. Só o do **GoW1**
(`SCUS-97399`) é antigo. `SCUS-97481` (GoW2) e `SLUS-21287` (PoP) estão carimbados v2.8.2 e são
válidos — dá pra ler o carimbo direto do `.p2s`, que é um zip com `PCSX2 Savestate Version.id`.

---

## 9. NÃO FAZER (medido, fechado)

- **Kernel custom pra levar condicionamento à rede.** O vetor vive só em LDS (§5.1). Nenhuma rota de
  fora alcança. As três rungs de interop passaram e não servem pra isso.
- **Reimplementar `k_pre_block`.** Não está no caminho que embarca (§5.2).
- **Embarcar hiprtc.** Funciona e se adapta à placa, mas custa **111,8 MiB** de DLL redistribuível pra
  economizar um compile de 60 ms que um `.co` de 5 KB em cache substitui.
- **Textura D3D12 compartilhada pra o kernel.** Não é endereçável linearmente, e a rota de surface
  object **zera a alocação em silêncio devolvendo `hipSuccess` nas cinco chamadas `hip*External*`**.
  Usar buffer compartilhado.
- **Reconstruir a ponte D3D12↔HIP.** O runtime já faz, com cópia de GPU de **0,04 ms/frame**, contra
  1,58 ms do round trip por RAM.
- **Os checkboxes de `NOBLEND`/`NOPOSTHIST`.** Construídos, medidos, rejeitados: mais ruído que ganho.
- **O "amortecimento duplo".** Não existe amortecedor do lado do addon (§8.3).
- **`Scale=1` com o efeito ligando no boot.** Rede a 1920x974 em modo inline tira o device do ar
  (`Failed to allocate 1024x1024 surface` com 13 GB de VRAM livre = assinatura de TDR, não de OOM).
- Continuam fechados do handoff anterior: NR Preset, ângulo de câmera, estilo entrando na rede na AMD
  pela rota atual.

---

## 10. O protocolo de medição que funcionou

Use este. Foi o que deu número confiável.

1. **Save state**, não câmera parada na mão. O emulador reproduz o mesmo frame; o `input mean` variou
   1,1% entre quatro runs. Confira o carimbo de versão antes (§8.6).
2. **`Scale` fixo** em todas as runs. Resíduo muda com o raster — as medições antigas a 630x372
   (ratio 0,282) e 1920x971 (ratio 0,132) não são comparáveis entre si.
3. **Duas baselines**, sempre, pra ter o piso de ruído. Sem isso um +17% não significa nada.
4. **Efeito nasce desligado** (`StartOn=0`) e liga por hotkey depois que o jogo e os shaders subiram.
   O ReShade leva segundos pra compilar o `DLSS5_Neural_Feed.fx`; sessão curta demais nunca fica
   pronta e o log diz `not installed, or its technique is not enabled`.
5. O addon mede sozinho no primeiro frame não-preto depois do 240, e **"Measure Residual Again"** no
   overlay re-arma. Log em `dlss5-neural.log`, truncado a cada sessão, com `fflush` por linha — matar
   o processo não perde medição.

`tools/knob_sweep.ps1` faz o arreio: seta a variável, lança, arquiva os dois logs e tabula com deltas.
`-Seconds` e `-ToggleAfter` rodam sem ninguém, `-Report` só re-tabula.

---

## 11. Ferramentas novas

| | |
|---|---|
| `tools/carve_amd_kernels.py` | tira o offload bundle do runtime, um `.co` por arquitetura. `--check` tem self-test (que pegou um bug meu) |
| `tools/read_amd_weights.py` | parseia `DLSSNRW1`, self-validante, e `--nvidia` compara os blobs contra a DLL da NVIDIA |
| `tools/knob_sweep.ps1` | o arreio de medição do §10 |
| `spike/rocm-custom-kernel/` | relatório completo + 5 checks runnable. `check_claims.py` sai 0 com nove asserções; `check_claims_teeth.py` tem que sair 1 |

Nenhum binário entra no repo — runtime, pesos e DLL da NVIDIA são do usuário.

---

## 12. Pendências, em ordem de valor

1. **Mapear `VarParams` no kernel que embarca** (`k_swin_var<32,true>`, 168 bytes, kernarg 424).
   Achar lá o equivalente da lane 10. **Até isso, nada da §5 é acionável.**
   `spike/rocm-custom-kernel/probe_preparams.cpp` é o molde — sondou `PreParams` campo a campo na GPU
   real; apontar pra `VarParams` é uma hora de trabalho.
2. **Decidir o que a lane 10 é antes de dirigi-la.** Style vs `UICorrection` (§5.3). Sem isso, mexer
   nela é repetir a §2.3 um nível mais fundo.
3. **Entender por que o byte write não igualou a variável de ambiente** (§7). É a única coisa que
   ficou sem explicação nesta sessão.
4. **Medir flicker direito.** O projeto já tem o instrumento: `Capture=base`, 120 frames com a câmera
   parada. A observação de flicker desta sessão é a olho, não medida.
5. Continuam do roadmap anterior: comparação A/B com a máquina NVIDIA (agora mais estreita, §3),
   RDR1 em D3D12, `DepthInverted` em jogo com depth real, `UICorrection`, pacote pro outro dev.

---

## 13. Estado da bancada PCSX2

Devolvida como estava. `dlss5-neural.ini` restaurado do backup (`StartOn=0`, `Scale=1`, `Passes=1`,
sem as chaves que eu tinha acrescentado), preset do ReShade intacto, nenhum processo preso, addon de
600 576 bytes instalado, logs das runs falhas removidos.

**Aviso:** o ini está com `Scale=1`, que é o estado original mas é a combinação que tirou o D3D11 do
ar quando o efeito ligou. A config que rodou liso nas quatro runs foi `Scale=0.34`.

Resultados arquivados em `spike/rocm-custom-kernel/knob-results-gow2/`.

---

## 14. Armadilhas confirmadas nesta sessão

- **Adjacência continua não sendo semântica**, agora um nível mais fundo: cinco lanes de um vetor de
  features não dizem qual delas é o quê. Seguir o consumidor prova que é *um* input, nunca *qual*.
- **Contradição entre duas leituras é pedido de medição**, não debate (§8.2).
- **Chave de ini não prova que a feature existe** (§8.3).
- **`hipSuccess` mente** em duas rotas diferentes: surface object sobre textura compartilhada zera a
  alocação em silêncio, e módulo carvado em RDNA3 dequantiza por uma LUT zerada.
- **Falha de alocação com VRAM sobrando é TDR**, não falta de memória.
- `hipcc` no Windows precisa de `INCLUDE`/`LIB` do MSVC setados à mão — ver `spike/rocm-custom-kernel/hipenv.sh`.
- Save state do PCSX2 carrega a versão no próprio arquivo (zip, `PCSX2 Savestate Version.id`); dá pra
  conferir sem abrir o emulador.
- Backtick em string de PowerShell com aspas duplas é escape: `"`ratio`"` vira carriage return.
