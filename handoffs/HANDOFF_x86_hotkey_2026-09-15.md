# Handoff — o rebind de hotkey do bridge x86, e o formato que parava o host

> **Histórico. Tudo aqui foi entregue na v0.5.1**, em 15/09/2026 — as três causas do rebind, a
> leitura por `effect_runtime::is_key_down`, a escrita pelo shadow, a remoção do BOM do ini e o
> transporte de `X8R8G8B8` como `B8G8R8A8_UNORM`. Veja a entrada v0.5.1 no `CHANGELOG.md`.
>
> O que a §9 lista como pendente não é pendência: a branch `v0.5.1` e o worktree `D:\dlss5-hotkey`
> não existem mais, e a dúvida sobre o botão Save foi superada pela v0.5.2, em que cada controle
> escreve no ini assim que assenta. O valor deste arquivo é a §10 e o método das §§2-7: como cada
> causa foi medida em vez de adivinhada.

Branch `v0.5.1`, worktree `D:\dlss5-hotkey`, saindo de `origin/master` (`425ed4d`).
**Nada commitado, nada empurrado.** 15/09/2026.

---

## 0. Estado em uma linha

**Três causas encontradas, todas por medição, todas corrigidas.** A que faltava era a terceira: o
ReShade **zera o `GetAsyncKeyState`** enquanto o overlay está aberto, que é exatamente o tempo em
que a varredura roda. Some-se a isso um quarto defeito, achado no relatório de erro do Oblivion, que
não é de hotkey: o formato de transporte da rota D3D9 clássica **não tem UAV tipado nesta GPU**, e o
host parava sem desenhar nada.

Falta o teste em jogo. Build e testes passam; o par novo já está instalado no Half-Life 2.

---

## 1. Os sintomas

1. "Não consigo trocar a bind por nada." Overlay aberto, botão clicável, combinação nunca mudava.
   Reproduzido no Half-Life 2 (rota `X86Dx9`, add-on em `bin\`).
2. O log dizia `ToggleKey=35` com o `dlss5-neural.ini` no disco dizendo `ToggleKey=120`.
3. (Relatado depois, no Oblivion GOG) o add-on instala, conecta, roda 9.720 quadros e **não muda
   nada na tela**.

---

## 2. Causa raiz nº 1 — duas velocidades

O laço que varria as teclas vivia dentro de `overlay32::Draw()`, chamado só quando o ReShade invoca
`OnOverlay32`. O temporizador que cancelava a captura vivia em `OnPresent`, que roda todo quadro.

**Medido:** `capture cancelled, overlay stamp 672 ms old`, e **nenhum** `capture scanning this
frame`. O cancelamento sempre ganhava; a varredura nunca rodou uma vez.

**Correção:** a varredura foi para `OnPresent`; o botão do overlay só arma a flag. A tolerância de
ociosidade subiu de 500 ms para `kCaptureIdleMs` (3 s).

---

## 3. Causa raiz nº 2 — captura a tecla que abriu o overlay

Com a nº 1 corrigida a captura disparou, e capturou `36` = `VK_HOME` — a tecla que abre o overlay do
ReShade, ainda segurada no instante em que a varredura começou. O atalho virava "Home sem
modificador", que liga e desliga o efeito ao abrir o painel.

**Correção:** espera por soltura. Nada é aceito enquanto qualquer não-modificador estiver
pressionado. É o comportamento padrão de captura de atalho.

---

## 4. Causa raiz nº 3 — o ReShade zera o `GetAsyncKeyState`

Esta é a que sobrava, e a que fazia o log terminar em `rebind armed` e mais nada, para sempre.

`reshade/source/input_windows.cpp`:

```cpp
extern "C" auto WINAPI HookGetAsyncKeyState(int vKey) -> SHORT
{
    if ((vKey & 0xF8) != 0)                               // toda tecla de 8 pra cima
    {
        if (reshade::input::is_blocking_any_keyboard_input())
            return 0;
    }
    ...
}
```

O ReShade engancha `GetAsyncKeyState`, `GetKeyState` e `GetKeyboardState` e responde **0 para todas
as teclas** enquanto o overlay bloqueia o teclado — que é o tempo inteiro em que o painel está na
tela. A varredura lia um teclado vazio, e por isso:

- não achava tecla nenhuma (nada para ligar);
- não desistia, porque `overlayAt` era recarimbado a cada quadro do painel aberto;
- não desarmava. `armed` e silêncio.

Também explica por que a captura *funcionou uma vez* e pegou Home: foi no primeiro quadro depois de
abrir o overlay, antes de o bloqueio valer. A correção da nº 2 fechou essa janela e o defeito ficou
100% reprodutível.

**Correção:** a captura passou a ler **`effect_runtime::is_key_down()`**, o estado de tecla do
próprio ReShade, que vem das mensagens de janela e não passa pelo gancho. É o que o próprio widget
de atalho do ReShade usa. O ponteiro do runtime é carimbado em `OnOverlay32` (único lugar onde o
ReShade entrega um) e zerado em `OnDestroy`.

---

## 5. Causa raiz nº 4 — a bind capturada nunca chegava à cópia sincronizada

Mesmo quando uma tecla era capturada, a captura escrevia em `g.toggleKey` e só. E:

- o painel lê `controls.shadow` (via `overlay32::Import`), então **o rótulo do botão não mudava**;
- `SyncControls` só empurra quando `shadow.settings_revision` sobe, então o host nunca sabia;
- `SaveSettings` é do host, então nada ia para o ini;
- qualquer `GetState`/`SetState`/`ReloadSettings` seguinte chama `OperationalSettings()`, que
  **copia o shadow por cima de `g`** — a tecla nova era desfeita.

Do ponto de vista de quem usa: "a bind não muda", mesmo nas vezes em que a tecla foi lida.

**Correção:** a captura escreve em `controls.shadow.toggleKey/toggleMods`, sobe
`settings_revision` e chama `OperationalSettings()`. O painel mostra, o host recebe, o Save
persiste.

---

## 6. O ini que era lido como se estivesse vazio (o `ToggleKey=35`)

`bin\dlss5-neural.ini` do Half-Life 2 começa com **EF BB BF** — BOM de UTF-8. `GetPrivateProfileInt`
lê o arquivo como bytes, então a primeira linha vira `<BOM>[dlss5]`, que não casa com seção nenhuma:
**todo o arquivo fica invisível e todos os valores caem no default**. `ToggleKey=35` é `VK_END`, o
default. Nenhum aviso, nada no log, e o arquivo parece certo em qualquer editor.

Quem põe o BOM é qualquer edição com `Set-Content -Encoding utf8` do Windows PowerShell — foi uma
edição manual do próprio ini durante a investigação anterior. O instalador escreve sem BOM
(`Encoding.UTF8.GetBytes`), então não é ele.

**Correção:** `src/neural/ini_text.h`, chamado antes de ler o ini nos dois lados (`LoadSettings` do
add-on e `Settings()` do bridge). Tira o BOM no lugar, uma vez, por temporário + `MoveFileEx`, e
registra no log que tirou. Instalações que já estão com BOM se consertam sozinhas ao abrir o jogo.

---

## 7. Causa raiz nº 5 — B8G8R8X8 não tem UAV tipado (o Oblivion)

Do relatório `amd-nr-report-20260915-014136.zip`, `dlss5-neural-x86-host.log`:

```
this GPU/driver has no typed UAV store for the back buffer format (DXGI 88, read as 88),
so there is no way to write the corrected image back. Stopping instead of drawing garbage.
```

DXGI 88 é `B8G8R8X8_UNORM`, para o qual `D3D9CpuFormat()` mapeava `D3DFMT_X8R8G8B8` — o back buffer
do Oblivion e da maior parte da geração dele. **Medido nesta máquina** (mesma GPU do relatório, RX
9070 XT, driver 32.0.31041), com uma sonda D3D12 de `CheckFeatureSupport`:

| Formato | texture2d | uav | typed_load | typed_store |
|---|---|---|---|---|
| `B8G8R8A8_UNORM` (87) | 1 | 1 | 1 | 1 |
| `B8G8R8X8_UNORM` (88) | 1 | **0** | **0** | **0** |
| `R8G8B8A8_UNORM` (28) | 1 | 1 | 1 | 1 |

**Correção:** `D3DFMT_X8R8G8B8` passa a ser transportado como `B8G8R8A8_UNORM`, os mesmos quatro
bytes por pixel com um canal que o jogo não lê. O par `X8B8G8R8` já era carregado como
`R8G8B8A8_UNORM` exatamente assim — o par BGRA é que era a exceção. Só a rota CPU muda: na rota
compartilhada o formato vem da textura do D3D9 e os dois lados do `CopyResource` continuam iguais.

Sem risco de alfa: `compose` escreve `float4(v, c.a)` e o destino final é uma superfície X8, que
ignora alfa. Nenhum caminho multiplica por alfa.

**O Half-Life 2 ia bater nisto também** assim que a bind funcionasse: mesmo back buffer X8R8G8B8.

---

## 8. O que mudou

```
src/neural/hotkey_capture.h     novo   a máquina de estados da captura, compartilhada
src/neural/ini_text.h           novo   remoção do BOM do ini
src/x86bridge/capture_test.cpp  novo   teste da captura, roda no build x86 e x64
src/x86bridge/frontend32.cpp           captura em OnPresent com is_key_down, shadow, formato
src/x86bridge/overlay32.inc            o botão só arma; carimba o runtime
src/neural/neural.cpp                  mesma captura no add-on 64-bit, e o BOM
build-x86bridge.ps1                    compila e roda o capture_test nas duas arquiteturas
```

O add-on 64-bit tinha a **mesma** causa nº 3 e a mesma nº 2, expostas (lá não há bridge): a captura
dele também lia `GetAsyncKeyState` dentro do overlay. Agora as duas pontas usam o mesmo cabeçalho.

**Build:** `.\build-x86bridge.ps1` passa — x86 e x64, testes de protocolo, IPC, captura, PE e
imports, mais o addon64 integrado.

---

## 9. O que falta

1. **Testar em jogo.** O par novo já está em
   `D:\SteamLibrary\steamapps\common\Half-Life 2\bin\` (os antigos viraram `.bak`). O roteiro:
   abrir o overlay, clicar no botão do atalho, **soltar tudo**, apertar F9. Esperado no
   `dlss5-neural-x86.log`:

   ```
   x86bridge: removed a UTF-8 byte-order mark from dlss5-neural.ini; ...
   x86bridge native x86 ... ToggleKey=120 ToggleMods=1      <- 120, não 35
   x86bridge: rebind armed (overlay stamp NN ms old)
   x86bridge: toggle bound to key NNN mods N
   ```

   Depois fechar o overlay e apertar a combinação nova: `x86bridge enabled=1`, e agora a imagem tem
   de mudar (§7).
2. **Conferir o Save.** Nunca se viu `settings saved to dlss5-neural.ini`. Com o rebind funcionando
   dá para testar se o botão Save persiste. Pode ser um defeito a mais.
3. **Commitar e mesclar.** A branch não tem commit. Base é `origin/master`.
4. **Restaurar o binário oficial** se abandonar: `%LOCALAPPDATA%\Temp\hl2-bin-backup` tem o par da
   release v0.5.0, e o `.bak` ao lado de cada arquivo é o build local anterior.

---

## 10. Armadilhas pagas (as antigas continuam valendo)

- **Nunca leia tecla com `GetAsyncKeyState` dentro de um add-on de ReShade** quando o overlay pode
  estar aberto. Use `effect_runtime::is_key_down`. Vale para qualquer add-on, não só para este.
- **BOM em ini é invisível e apaga o arquivo inteiro** para a API de perfil do Windows.
  `Set-Content -Encoding utf8` do PowerShell 5.1 põe BOM; `Out-File -Encoding utf8` também.
- **`B8G8R8X8_UNORM` não é um formato de UAV.** Se algo precisa escrever no back buffer, o par X8
  tem de virar A8.
- **Não testar com F10.** No Windows ela gera `WM_SYSKEYDOWN`, não `WM_KEYDOWN`. Use F9 (120) ou
  Insert (45).
- **Os arquivos deste repositório são CRLF.** Patch com padrão em LF não casa e falha em silêncio.
- **`\x64` em string Python não-raw** vira o caractere `d`.
- **`ImGui::IsItemClicked()` depois de `ImGui::Button()`** não dispara no mesmo quadro.
- **O clone em `D:\dlss5` está 48 commits atrás e sem upstream configurado.** Não foi tocado.
