# dlss5-neural-amd — como instalar

Versão curta em português. O [README](README.md) tem tudo, em inglês.

É um add-on de ReShade que roda a rede do DLSS-NR numa placa AMD. Só foi testado no PCSX2, numa
RX 9070 XT. Em qualquer outra coisa eu não sei o que acontece.

**Só quer rodar?** [Antes de começar](#antes-de-começar) e os passos abaixo. Não precisa de
compilador.
**Quebrou?** [Quando não funciona](#quando-não-funciona), organizado pelo que aparece na tela.
**Quer mexer no código?** [Compilar por conta própria](#compilar-por-conta-própria-opcional).

## Antes de começar

| | |
|---|---|
| **Placa** | AMD **RDNA3 ou RDNA4** com o runtime **HIP 7** (`amdhip64_7.dll` no path). HIP 6 não serve. Driver Adrenalin atual já traz. Em NVIDIA ou Intel não faz nada. |
| **Renderizador** | O jogo tem que estar em **Direct3D 12**. Em D3D11, Vulkan ou OpenGL o add-on carrega e fica parado. |
| **ReShade** | A build **com add-on**, 6.x. A normal não carrega add-on nenhum. Testado na 6.8.0. |
| **Disco** | Uns 150 MB, por causa dos pesos da rede. |

Testado: RX 9070 XT, ReShade 6.8.0, PCSX2 2.3.14 e 2.8.2, God of War 1. Mais nada foi testado
por mim.

**Onde ficam os arquivos? Sempre do lado do `pcsx2-qt.exe`.** Vale tanto pra PCSX2 portable em
HD externo quanto pra instalação normal — o add-on só olha a pasta do `.exe` que está rodando.
A diferença portable/instalador só importa pras *configurações* do PCSX2, e isso aparece uma vez
só, na parte do renderizador.

## Três arquivos na pasta do `pcsx2-qt.exe`

### 1. `dlss5-neural.addon64`

Baixe dos **[Releases](https://github.com/zmodelerlover/dlss5-neural-amd/releases/latest)**.

Pronto. **Não precisa compilar nada** — o arquivo do release é compilado deste mesmo
repositório. Compilar só serve se você quiser mexer no código, e tem
[seção própria](#compilar-por-conta-própria-opcional) no fim.

### 2 e 3. `dlssnr_amd_pass1.dll` e `dlssnr_on_amd_weights.bin`

**Esses dois não estão no repositório e não vão estar.** Os pesos são derivados da NVIDIA e o
runtime vem de um projeto de terceiro que não declara licença. Não sou eu que vou redistribuir.

> **Os dois estão no canal `files` do discord → https://discord.gg/wYhvS3JSHM**

A `.dll` de lá já está reconstruída, é só copiar. Confira o que baixou contra
`tools/SHA256SUMS.txt`:

```powershell
Get-FileHash dlssnr_amd_pass1.dll, dlssnr_on_amd_weights.bin -Algorithm SHA256
```

Tem que ser exatamente aquela build. O add-on confere o hash e recusa qualquer outra, porque a
coisa toda é offset fixo dentro de um binário específico e apontar para outro trava o jogo.

### E o `dlssnr_on_amd.ini`?

Esse aparece sozinho na primeira vez que roda, voce nao copia. **Não apague.** Ver [abaixo](#o-ini-do-engine).

## ReShade e PCSX2

Instale o ReShade (build **com add-on**) no `pcsx2-qt.exe`, opção **Direct3D 10/11/12**. Pode
pular o download de shaders, isso aqui não usa nenhum.

No PCSX2: Configurações → Gráficos → Renderizador → **Direct3D 12**.

**Cuidado com configuração por jogo.** Um renderizador fixado num jogo **ganha** da configuração
global, calado — isso me custou uma noite. O jeito mais fácil de conferir, não importa onde o
PCSX2 esteja instalado: **clique com o botão direito no jogo na lista → Propriedades →
Gráficos**, e veja se o Renderizador está em *Direct3D 12* ou na configuração global.

Se preferir olhar o arquivo, ele é `gamesettings\<SERIAL>.ini` — mas *qual pasta* depende de como
o PCSX2 foi instalado:

* **Portable** — você extraiu o `.7z`/`.zip`, ou existe um `portable.ini` do lado do exe. É o
  caso comum de PCSX2 em HD externo. Aí fica tudo junto do `pcsx2-qt.exe`: `gamesettings\`,
  `inis\`, `memcards\`, `cache\`. Não existe `Documents\PCSX2` nenhum.
* **Instalador** — `Documents\PCSX2\gamesettings\`.

**Você quer `Renderer = 15`.** 15 é Direct3D 12, que é a única coisa em que este add-on funciona.
`Renderer = 3` é Direct3D 11 — se você achar essa linha, ela é o problema, não a solução. Apagar
a linha também resolve: aí volta pra configuração global.

## Rodando

Abra o jogo, **Home** para o overlay do ReShade → aba **Add-ons** → **DLSS Neural Rendering
(AMD)**. A linha de status diz se está rodando de verdade. Tem também o `dlss5-neural.log` do
lado do exe.

Os padrões já são a configuração que funciona (Encoding sRGB, Resolution Scale 0.50, Pass
Count 1). Não precisa mexer em nada.

## Quando não funciona

| O que aparece | O que é |
|---|---|
| O add-on não aparece na aba Add-ons | O `ReShade.ini` tem `DisabledAddons=dlss5 neural@dlss5-neural.addon64` em `[ADDON]`. O ReShade escreve essa linha se você desmarcar o add-on uma vez, e aí ele nunca mais carrega, sem erro nenhum. Apague a linha. |
| Status diz que a API está errada | O PCSX2 não está em D3D12. Confira também a configuração por jogo, não só a global: botão direito no jogo, Propriedades, Gráficos. |
| `HIP: amdhip64_7.dll failed to load` | HIP 7 não está instalado. HIP 6 não vale. |
| `hash mismatch; refused` | `dlssnr_amd_pass1.dll` errada. Confira contra `tools/SHA256SUMS.txt`. A recusa é de propósito — a alternativa é travar. |
| O jogo morre com `887A0005` | `DXGI_ERROR_DEVICE_REMOVED`, TDR do Windows. Ver [o ini do engine](#o-ini-do-engine). |
| Roda mas "só muda um pouco a cor" | Encoding errado. Em back buffer SDR de 8 bits tem que ser **sRGB**. `scRGB-nl` lineariza uma imagem que já é sRGB e ainda escala por 203/branco, então a rede recebe uma imagem quase preta e não faz nada. |
| `imgui.h` ou `reshade.hpp` não encontrado ao compilar | Você apagou a pasta `external/`. Ela está no repositório; `git checkout external` traz de volta. |

## Compilar por conta própria (opcional)

**Pule isso se você não vai mexer no código.** O `.addon64` dos
[Releases](https://github.com/zmodelerlover/dlss5-neural-amd/releases/latest) é compilado deste
repositório e é o mesmo arquivo que sairia aqui.

**O que instalar antes.** Uma coisa só: o compilador C++ da Microsoft. Não precisa do Visual
Studio completo — o **Build Tools for Visual Studio** é grátis e basta. Pegue em
<https://visualstudio.microsoft.com/pt-br/downloads/>, em *Ferramentas para Visual Studio* →
*Build Tools para Visual Studio*. No instalador dele marque só o workload **"Desenvolvimento
para desktop com C++"** e instale. Esse workload já traz o SDK do Windows junto, que é a outra
metade do que o build precisa. Se você já tem Visual Studio com C++, já tem tudo.

**Depois, no PowerShell:**

```powershell
git clone https://github.com/zmodelerlover/dlss5-neural-amd
cd dlss5-neural-amd
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target neural
```

Repare no `-ExecutionPolicy Bypass`. O Windows se recusa a rodar `.ps1` baixado por padrão, então
`.\build.ps1` sozinho normalmente falha com *"não pode ser carregado porque a execução de scripts
foi desabilitada neste sistema"*. Essa mensagem é do Windows, não deste projeto. A linha acima
contorna só naquele comando, sem mudar nada na sua máquina.

**É esse o build inteiro.** Não precisa baixar nada antes, não tem submódulo, nem `vcpkg`, nem
CMake, nem `.sln` pra abrir. Os headers do ReShade e do ImGui já estão em `external/reshade/`
(antes não estavam, e era por isso que não compilava pra ninguém). O `build.ps1` acha o
compilador e o SDK sozinho; se pegar o errado, passe `-VsPath` ou `-SdkPath`.

**Deu certo se** as duas últimas linhas forem assim, e o `build\dlss5-neural.addon64` existir:

```
OK: ...\build\dlss5-neural.addon64
dlss5-neural.addon64  73728  ...
```

**Se falhar:**

| Mensagem | O que fazer |
|---|---|
| `a execução de scripts foi desabilitada` / `running scripts is disabled` | Você esqueceu o `powershell -ExecutionPolicy Bypass -File` na frente. |
| `vswhere.exe not found` / `No Visual Studio install with the C++ tools` | O workload de C++ não está instalado. Rode o instalador do Build Tools e marque *Desenvolvimento para desktop com C++*. |
| `Windows 10/11 SDK not found in the registry` | Mesmo instalador, mesmo workload — ele inclui o SDK. Ou passe `-SdkPath`. |
| `fatal error C1083: 'imgui.h'` | Você apagou a pasta `external/`. `git checkout external` traz de volta. |
| `git` não é reconhecido | Instale o Git for Windows, ou simplesmente use o build do release. |

Os outros dois alvos compilam igual: `-Target probe` (mostra o que um jogo expõe) e
`-Target session`.

### Se for reportar um problema

Print não ajuda muito aqui — flicker é frame alternando, e uma imagem parada congela um deles,
então sempre parece normal. Duas coisas resolvem quase tudo:

1. **A linha de status.** Overlay do ReShade → Add-ons → DLSS Neural Rendering (AMD). Ela diz
   `Running: X processed, Y skipped (Z%)` mais o tamanho do back buffer e da rede. Cole essa linha.
2. **Os logs**, os dois do lado do `pcsx2-qt.exe` — a mesma pasta onde você pôs o add-on, seja
   portable ou não:
   * `dlss5-neural.log` — o add-on: o que ele detectou, tamanho e formato do back buffer, e a
     medição de resíduo que ele faz no frame 240.
   * `dlssnr_on_amd.log` — o runtime: formatos, tempo por job, timeouts, faults.

A medição de resíduo no primeiro é a parte útil. `mean 0.000000` quer dizer que a rede devolveu
a entrada intacta, o que é um problema completamente diferente de um resíduo diferente de zero
que sai errado na tela. Do sofá, os dois são idênticos.

### O ini do engine

O runtime lê o `dlssnr_on_amd.ini` da pasta do jogo quando carrega. **O padrão interno dele para
o watchdog é 600 ms**, e um job travado por esse tempo dispara o TDR do Windows, que remove o
dispositivo D3D12 e leva o emulador junto. O sintoma é um monte de `887A0005` no `emulog.txt` e
o PCSX2 morto, sem nada apontando para este add-on.

Por isso o add-on escreve o arquivo sozinho quando ele não existe, com `InlineWaitMs=100`. Em
escala 0.50 a rede leva uns 16 ms, então 100 é folga larga, e passando disso o frame sai sem o
efeito em vez de congelar. Se apagar, ele é escrito de novo; se editar, a sua versão fica. Não
aumente o `InlineWaitMs` sem motivo.
