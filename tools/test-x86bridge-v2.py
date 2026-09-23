from pathlib import Path
import os,re,subprocess,tempfile,shutil
r=Path(__file__).resolve().parents[1];n=r/'src/x86bridge';u=r/'src/ui'
read=lambda path:path.read_text(encoding='utf-8-sig')
compiler=os.environ.get('CXX') or shutil.which('g++')
if not compiler:raise SystemExit('Set CXX to a C++20 compiler')
# The panel is one implementation in src/ui/, drawn by both routes. It needs no windows.h, so every
# file of it is compiled here against the real bundled imgui.h and ReShade's function table, with
# the warnings that the MSVC build does not turn into errors.
for f in sorted(u.rglob('*.cpp')):
 subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-fsyntax-only','-I'+str(r/'external/reshade'),str(f)],check=True)
print('PASS shared panel source syntax against real bundled imgui.h and reshade_overlay.hpp, every file of src/ui')

# The bridge hands the panel a copy of the wire settings and takes the edit back. Every field has
# to survive the trip, the revision has to stay the wire's own, and the one panel-only field --
# the companion effect -- must never pick up a value on this route.
roundtrip=r'''#include "panel_wire.h"
#include <cassert>
#include <cstring>
using namespace x86bridge;
int main(){WireSettings w;unsigned char* b=reinterpret_cast<unsigned char*>(&w);
 for(size_t i=0;i<sizeof(w);++i)b[i]=static_cast<unsigned char>(i*7+3);
 for(unsigned i=0;i<3;++i){w.passStructure[i]=1.5f+i;w.passTone[i]=.25f*i;w.passSkin[i]=-1.f+i;}
#define X(type,name,low,high) w.name=static_cast<type>(high);
#include "settings_fields.inc"
#undef X
 const ui::PanelSettings p=ToPanel(w);assert(p.useFeedEffect==0);
#define X(type,name,low,high) assert(p.name==w.name);
#include "settings_fields.inc"
#undef X
 WireSettings back=FromPanel(w,p);assert(std::memcmp(&back,&w,sizeof(w))==0);
 ui::PanelSettings q=p;q.scale=.5f;q.passTone[2]=2.f;back=FromPanel(w,q);
 assert(back.scale==.5f&&back.passTone[2]==2.f&&back.settings_revision==w.settings_revision);
}
'''
with tempfile.TemporaryDirectory(prefix='x86bridge-wire-') as d:
 p=Path(d);(p/'wire.cpp').write_text(roundtrip,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(n),str(p/'wire.cpp'),'-o',str(p/'wire')],check=True)
 subprocess.run([str(p/'wire')],check=True)
print('PASS panel/wire conversion: every X-macro field and per-pass value round-trips, revision kept, companion effect never set')

# Compile the actual host ExportSettings/ApplySettings bodies, with only original g atomics doubled.
h=read(n/'host64.cpp')
methods=h[h.index('    WireSettings ExportSettings()'):h.index('    WireStatus ExportStatus()')]
state=r'''#include "control_state.h"
#include <atomic>
#include <cassert>
#include <limits>
#include <cstring>
using namespace x86bridge;
struct Engine {
 struct {
#define X(type,name,low,high) std::atomic<type> name{};
#include "settings_fields.inc"
#undef X
 std::atomic<bool> passOverride[3]{};
 std::atomic<float> passStructure[3]{},passTone[3]{},passSkin[3]{};
 } settings;
 std::atomic<bool> historyValid{true};
}g;
struct Host{uint64_t settingsRevision=1;
'''+methods+r'''};
int main(){Host host;WireSettings s; s.settings_revision=2;s.skin=-1;s.useHistory=1;s.scale=.5f;s.structure=1;s.tone=1;
 g.settings.useHistory.store(1);assert(host.ApplySettings(s));assert(g.historyValid.load());assert(g.settings.skin.load()==-1);assert(g.settings.inlineMode.load()==1);
 auto before=host.ExportSettings();assert(!host.ApplySettings(s));auto after=host.ExportSettings();assert(std::memcmp(&before,&after,sizeof(before))==0);
 s.settings_revision=3;s.scale=std::numeric_limits<float>::infinity();assert(!host.ApplySettings(s));after=host.ExportSettings();assert(std::memcmp(&before,&after,sizeof(before))==0);
 s.scale=.5f;s.useHistory=0;assert(host.ApplySettings(s));assert(!g.historyValid.load());
 g.historyValid.store(true);s.settings_revision=4;s.structure=2;assert(host.ApplySettings(s));assert(g.historyValid.load());
 s.settings_revision=5;s.startOn=1;s.toggleKey=65;s.toggleMods=5;s.language=1;s.disableOnAltTab=1;assert(host.ApplySettings(s));
 after=host.ExportSettings();assert(after.startOn==1&&after.toggleKey==65&&after.toggleMods==5&&after.language==1&&after.disableOnAltTab==1);
}
'''
with tempfile.TemporaryDirectory(prefix='x86bridge-state-') as d:
 p=Path(d);(p/'state.cpp').write_text(state,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(n),str(p/'state.cpp'),'-o',str(p/'state')],check=True)
 subprocess.run([str(p/'state')],check=True)
print('PASS actual host Apply/Export: finite clamp, stale reject/no mutation, history change only, operational fields mirrored, forced inline')
# No I/O in the panel, and none in the bridge's side of it either. The only control transaction
# owner is OnPresent: the overlay callback turns clicks into requests and returns.
ui=''.join(read(f) for f in sorted(u.rglob('*')) if f.suffix in ('.h','.cpp'))
front=read(n/'frontend32.cpp');adapter=read(n/'panel32.cpp')
for call in ['Request(', 'ReadFile(', 'WriteFile(', 'WaitFor', 'FlushAndWait', 'StartHost(', 'StateRequest(', 'SyncControls(', 'SaveSettings(', 'LoadSettings(', 'x86bridge::', 'WireSettings']:
 assert call not in ui,call
for call in ['Request(', 'WaitFor', 'FlushAndWait', 'StartHost(', 'StateRequest(', 'SyncControls(']:
 assert call not in adapter,call
assert 'std::try_to_lock' in adapter and 'controls.save=true' in adapter and 'controls.reload=true' in adapter and 'controls.measure=true' in adapter
assert 'controls.exportLogs=true' in adapter and 'ExportBridgeLogs()' not in adapter
assert 'controls.liftCap=true' in adapter and 'CommandCode::LiftScaleCap' in front
assert front.index('if(controls.save)')<front.index('controls.exportedPath=ExportBridgeLogs()')
# The rebuilt panel: fifteen controls, the cascade that reveals the rest, and no
# MEASURED/TRACED/UNKNOWN/INERT tags. Feed.fx exists in the shared panel but is never drawn on this
# route -- the effect runtime is in this process and the network is in the helper -- so the bridge
# must never claim the capability.
for gone in ['void Tag(','enum Known','Tag(k']:
 assert gone not in ui,gone
assert 'hasFeedEffect' not in adapter and 'bool hasFeedEffect = false;' in ui
assert 'status.hasFeedEffect && Shown(s, kOptFeed)' in ui
for wanted in ['More settings','RightLine(','GroupShown(','EffectiveScale(','Export logs to desktop','kOptMeasure']:
 assert wanted in ui,wanted
# Autosave: armed only once a control has settled, and only against what is already on disk. Without
# the IsAnyItemActive guard a held slider is one whole-file rewrite per frame.
assert 'controls.shadow.settings_revision!=controls.savedRevision&&!ImGui::IsAnyItemActive()' in adapter
assert front.count('controls.savedRevision=controls.sentRevision')==3 # first sync, save, reload
assert front.count('SyncControls()')==2 # definition + OnPresent call
assert front.index('if(!SyncControls()')<front.index('Kind::Frame,&f')
for name in ['Silent Hill','Resident Evil','God of War','GTA V','NFS','PCSX2','ETS2','ProfileForThisProcess','kTargets']:
 assert name not in ui+front+h,name
# The Timing combo reads and writes the frontend's real flag rather than a shadow field, so the
# panel cannot drift from what the bridge is doing, and switching is live rather than a
# restart-only ini edit. The wire keeps the helper's own value, which is always same-frame.
assert 'panel.inlineMode=Pipelined()?0u:1u;' in adapter and 'SwitchPipelining(panel.inlineMode==0);' in adapter
assert adapter.index('panel.inlineMode=Pipelined()?0u:1u;')<adapter.index('ui::DrawPanel(')<adapter.index('SwitchPipelining(panel.inlineMode==0);')
assert 'panel.inlineMode=controls.shadow.inlineMode;' in adapter
assert adapter.index('SwitchPipelining(panel.inlineMode==0);')<adapter.index('x86bridge::FromPanel(')
# The port reaches the real flag and the real switch, not copies of them.
assert 'bool Pipelined(){return g.async;}' in front and 'void SwitchPipelining(bool on){SetAsync(on);}' in front
# The panel must not promise smearing the implementation cannot produce: the back buffer is
# replaced whole, so a pipelined frame is the previous one finished, never a mix of two.
assert 'It does not smear' in ui and 'smearing when the camera turns' not in ui
assert 'ImGui::IsItemDeactivated()' in ui and 'PanelAction::LiftScaleCap' in ui
# A route difference is data, never a preprocessor branch inside the panel.
assert re.findall(r'^\s*#\s*if\w*\s+(\w+)',ui,re.M)==['RESHADE_API_VERSION']
assert 'g.settings.inlineMode.store(true)' in h and 'LoadSettings();ForceInline();' in h
assert 'register_overlay("AMD Neural Rendering (32-bit)",OnOverlay32)' in front
assert h.index('SaveSettings();Snapshot')>h.index('case Kind::SaveSettings:')
fields=read(n/'settings_fields.inc')
assert all(re.fullmatch(r'X\((uint32_t|int32_t|float), [A-Za-z]+, [-.0-9]+, [-.0-9]+\)',line) for line in fields.splitlines() if not line.startswith('//'))
print('PASS panel has no IPC/waits; present-only controls; frontend-only shadow; original host Save/Reload; same-frame lock; generic source; route differences are data')
