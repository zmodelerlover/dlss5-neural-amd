# dlss5-neural-amd — como instalar

Versão curta em português. O [README](README.md) tem tudo, em inglês.

É um add-on de ReShade que roda a rede do DLSS-NR numa placa AMD. Só foi testado no PCSX2, numa
RX 9070 XT. Em qualquer outra coisa eu não sei o que acontece.

## O que precisa

* AMD **RDNA3 ou RDNA4** com o runtime **HIP 7** instalado (`amdhip64_7.dll`). HIP 6 não serve.
  Driver Adrenalin atual já traz.
* O jogo tem que estar em **Direct3D 12**.
* **ReShade com suporte a add-on** (o instalador "with full add-on support"), 6.x.

Testado: RX 9070 XT, ReShade 6.8.0, PCSX2 2.3.14 e 2.8.2, God of War 1.

## Quatro arquivos na pasta do `pcsx2-qt.exe`

### 1. `dlss5-neural.addon64`

Baixe dos [Releases](https://github.com/zmodelerlover/dlss5-neural-amd/releases), ou compile:

```powershell
git clone https://github.com/zmodelerlover/dlss5-neural-amd
cd dlss5-neural-amd
.\build.ps1 -Target neural
```

**É só isso.** Não precisa baixar header nenhum antes: os headers do ReShade e do ImGui estão
dentro do repositório, em `external/reshade/`. (Antes não estavam, e por isso não compilava para
ninguém. Agora estão.) Você precisa do Visual Studio com o workload de C++ e do SDK do
Windows 10/11 — o `build.ps1` acha os dois sozinho. Sai em `build\dlss5-neural.addon64`.

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

### 4. `dlssnr_on_amd.ini`

Esse aparece sozinho na primeira vez que roda. **Não apague.** Ver [abaixo](#o-ini-do-engine).

## ReShade e PCSX2

Instale o ReShade (build **com add-on**) no `pcsx2-qt.exe`, opção **Direct3D 10/11/12**. Pode
pular o download de shaders, isso aqui não usa nenhum.

No PCSX2: Configurações → Gráficos → Renderizador → **Direct3D 12**.

Cuidado com configuração por jogo: o PCSX2 guarda em
`Documents\PCSX2\gamesettings\<SERIAL>.ini`, e uma linha `Renderer = 3` ali **ganha** da
configuração global, calado. `15` é D3D12. Isso me custou uma noite.

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
| Status diz que a API está errada | O PCSX2 não está em D3D12. Confira o ini por jogo, não só o global. |
| `HIP: amdhip64_7.dll failed to load` | HIP 7 não está instalado. HIP 6 não vale. |
| `hash mismatch; refused` | `dlssnr_amd_pass1.dll` errada. Confira contra `tools/SHA256SUMS.txt`. A recusa é de propósito — a alternativa é travar. |
| O jogo morre com `887A0005` | `DXGI_ERROR_DEVICE_REMOVED`, TDR do Windows. Ver [o ini do engine](#o-ini-do-engine). |
| Roda mas "só muda um pouco a cor" | Encoding errado. Em back buffer SDR de 8 bits tem que ser **sRGB**. `scRGB-nl` lineariza uma imagem que já é sRGB e ainda escala por 203/branco, então a rede recebe uma imagem quase preta e não faz nada. |
| `imgui.h` ou `reshade.hpp` não encontrado ao compilar | Você apagou a pasta `external/`. Ela está no repositório; `git checkout external` traz de volta. |

### O ini do engine

O runtime lê o `dlssnr_on_amd.ini` da pasta do jogo quando carrega. **O padrão interno dele para
o watchdog é 600 ms**, e um job travado por esse tempo dispara o TDR do Windows, que remove o
dispositivo D3D12 e leva o emulador junto. O sintoma é um monte de `887A0005` no `emulog.txt` e
o PCSX2 morto, sem nada apontando para este add-on.

Por isso o add-on escreve o arquivo sozinho quando ele não existe, com `InlineWaitMs=100`. Em
escala 0.50 a rede leva uns 16 ms, então 100 é folga larga, e passando disso o frame sai sem o
efeito em vez de congelar. Se apagar, ele é escrito de novo; se editar, a sua versão fica. Não
aumente o `InlineWaitMs` sem motivo.
