#include "sections.h"

#include "../i18n.h"
#include "../theme.h"
#include "../widgets/widgets.h"

namespace ui {

namespace {

void SaveReload(PanelActions &actions)
{
    if (ImGui::Button(T("Save", "Salvar")))
        actions.Add(PanelAction::Save);
    Help("Writes everything to amd-nr.ini next to the exe. You do not have to press it -- every "
         "control saves itself when you let go. This writes now, and logs that it happened.",
         "Escreve tudo no amd-nr.ini ao lado do exe. Você não precisa apertar -- cada controle se "
         "salva sozinho quando você solta. Isto escreve agora, e deixa registro no log.");
    ImGui::SameLine();
    if (ImGui::Button(T("Reload", "Recarregar")))
        actions.Add(PanelAction::Reload);
    Help("Re-reads amd-nr.ini, discarding anything changed here since the last save. This is how "
         "a setting edited in the file is picked up without restarting the game.",
         "Relê o amd-nr.ini, descartando qualquer coisa mudada aqui desde o último salvamento. É "
         "assim que um ajuste editado no arquivo é aplicado sem reiniciar o jogo.");
}

// Asking somebody for a log means asking them to find the emulator's install directory first --
// and on the bridge there are two logs, in two directories. This puts the files that answer any
// question about a run, and the ini that produced them, in a dated folder on the desktop. On its
// own line: a thin panel does not hold four buttons on one.
void ExportLogs(const PanelStatus &status, PanelActions &actions)
{
    if (ImGui::Button(T("Export logs to desktop", "Exportar logs pra área de trabalho")))
        actions.Add(PanelAction::ExportLogs);
    if (status.helperProcess)
        Help("Copies amd-nr-x86.log, the helper's amd-nr-x86-host.log, the runtime's "
             "dlssnr_on_amd.log, ReShade.log and amd-nr.ini into a dated folder on your desktop. "
             "The settings go with them because a log without them cannot be compared against "
             "anything.\n\n"
             "The logs are truncated every time the game starts, so export before relaunching.",

             "Copia o amd-nr-x86.log, o amd-nr-x86-host.log do ajudante, o dlssnr_on_amd.log do "
             "runtime, o ReShade.log e o amd-nr.ini para uma pasta datada na sua área de "
             "trabalho. Os ajustes vão junto porque um log sem eles não dá para comparar com "
             "nada.\n\n"
             "Os logs são truncados toda vez que o jogo abre, então exporte antes de relançar.");
    else
        Help("Copies amd-nr.log, the runtime's dlssnr_on_amd.log, ReShade.log and amd-nr.ini "
             "into a dated folder on your desktop. The settings go with them because a log "
             "without them cannot be compared against anything.\n\n"
             "The logs are truncated every time the game starts, so export before relaunching.",

             "Copia o amd-nr.log, o dlssnr_on_amd.log do runtime, o ReShade.log e o amd-nr.ini "
             "para uma pasta datada na sua área de trabalho. Os ajustes vão junto porque um log "
             "sem eles não dá para comparar com nada.\n\n"
             "Os logs são truncados toda vez que o jogo abre, então exporte antes de relançar.");
    if (status.exportFailed)
        ImGui::TextColored(kDanger, "%s", T("Could not write to the desktop.",
                                            "Não deu para escrever na área de trabalho."));
    else if (!status.exportedPath.empty())
        ImGui::TextColored(kOk, T("Exported to %ls", "Exportado para %ls"),
                           status.exportedPath.c_str());
}

void FactoryDefaults(PanelActions &actions)
{
    ImGui::SameLine();
    if (ImGui::Button(T("Factory Defaults", "Padrões de Fábrica")))
        actions.Add(PanelAction::FactoryDefaults);
    Help("Restores the tuning to what it ships with. Enabled, startup, hotkey and language are "
         "preserved. It lands in the ini with everything else, on its own.",
         "Restaura os ajustes de fábrica. Ligado, inicialização, atalho e idioma são preservados. "
         "Vai para o ini junto com o resto, sozinho.");
}

} // namespace

void DrawFooter(const PanelStatus &status, PanelActions &actions)
{
    ImGui::Separator();
    SaveReload(actions);
    ExportLogs(status, actions);
    FactoryDefaults(actions);
    // The legend, which used to be six paragraphs and a four-entry tag table. One wrapped line:
    // the tags are gone, and red and amber are the only marks a control can carry now.
    ImGui::TextDisabled("%s", T("Red: risks the display driver at this value. Amber: past what was "
                                "measured here.",
                                "Vermelho: neste valor arrisca o driver de vídeo. Âmbar: além do "
                                "que foi medido aqui."));
}

} // namespace ui
