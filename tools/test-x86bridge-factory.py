"""Compile the actual additive host factory methods against captured upstream defaults."""
from pathlib import Path
import os,re,subprocess,tempfile,shutil
r=Path(__file__).resolve().parents[1];n=r/'src/x86bridge'
read=lambda path:path.read_text(encoding='utf-8-sig')
compiler=os.environ.get('CXX') or shutil.which('g++')
if not compiler:raise SystemExit('Set CXX to a C++20 compiler')
h=read(n/'host64.cpp');u=read(r/'src/neural/neural.cpp');ui=read(n/'overlay32.inc');front=read(n/'frontend32.cpp')
fields=re.findall(r'^X\((\w+), (\w+),', read(n/'settings_fields.inc'), re.M)
# Every test initial value comes from the original State declaration, not a second default table.
initial=[]
for typ,name in fields:
 m=re.search(r'std::atomic<[^>]+>\s+'+name+r'\s*\{([^}]+)\}',u);assert m,name
 initial.append(name+'.store('+m[1].strip()+');')
methods=h[h.index('    WireSettings ExportSettings()'):h.index('    WireStatus ExportStatus()')]
methods+=h[h.index('    void CaptureFactoryDefaults()'):h.index('    void EnsureX86Ini()')]
restore=h[h.index('    void RestoreFactoryDefaults()'):h.index('    void Init(')]
assert all(x not in restore for x in ['SaveSettings(', 'LoadSettings(', 'WritePrivateProfile', 'EnsureX86Ini('])
methods+=restore
source='''#include "control_state.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <iterator>
#include <cstring>
using namespace x86bridge;
constexpr int VK_END=35;
void Require(bool b,const char*){assert(b);}
struct Watch {bool value=true;unsigned writes=0;void store(bool v){value=v;++writes;}bool load(){return value;}};
struct Engine {
#define X(type,name,low,high) std::atomic<type> name{};
#include "settings_fields.inc"
#undef X
 Watch historyValid;
 std::atomic<bool> passOverride[3]{};
 std::atomic<float> passStructure[3]{},passTone[3]{},passSkin[3]{};
 Engine(){'''+''.join(initial)+'''}
}g;
struct Host{uint64_t settingsRevision=1;WireSettings factoryDefaults{};
'''+methods+'''};
std::string file(){std::ifstream f("amd-nr.ini",std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
int main(){
 std::ofstream("amd-nr.ini")<<"[amd-nr]\\nColourStrength=0.75\\nUserText=untouched\\n";auto original=file();
 Host host;host.CaptureFactoryDefaults();auto captured=host.factoryDefaults;
 auto custom=host.ExportSettings();custom.settings_revision=2;custom.colourStrength=.9f;custom.structure=2;custom.skin=2;custom.passes=3;custom.optional=7;custom.scale=1.5f;custom.tone=2;
 custom.enabled=1;custom.startOn=1;custom.toggleKey=65;custom.toggleMods=5;custom.language=1;custom.disableOnAltTab=1;custom.useHistory=0;custom.useDepth=0;
 assert(host.ApplySettings(custom));g.historyValid.value=true;g.historyValid.writes=0;
 host.RestoreFactoryDefaults();auto result=host.ExportSettings();
 assert(result.colourStrength==0.25f&&result.structure==1&&result.skin==-1&&result.passes==1&&result.inlineMode==1);
 auto expected=FactorySettings(captured,custom);assert(std::memcmp(&result,&expected,sizeof(result))==0);
 assert(result.enabled==1&&result.startOn==1&&result.toggleKey==65&&result.toggleMods==5&&result.language==1&&result.disableOnAltTab==1);
 assert(result.optional==7); // a panel arrangement is a preference, not tuning

 assert(!g.historyValid.load()&&g.historyValid.writes==1);assert(file()==original);
 StateSnapshot snapshot{result,{}};assert(snapshot.settings.colourStrength==0.25f&&snapshot.settings.settings_revision==3);
 WireCommand c;c.id=12;c.code=CommandCode::FactoryDefaults;assert(NewCommand(c,11)&&!NewCommand(c,12));
}
'''
with tempfile.TemporaryDirectory(prefix='factory-test-') as d:
 p=Path(d);(p/'test.cpp').write_text(source,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(n),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],cwd=p,check=True)
assert 'CaptureFactoryDefaults();EnsureX86Ini();LoadSettings();' in h
assert 'controls.factory=true' in ui and 'Kind::Command,&c,sizeof(c),true' in front
print('PASS factory: captured original defaults, x86 overrides, preferences preserved, history reset once, INI byte-identical, snapshot updated, replay rejected; actual host methods compiled/executed with engine atomics doubled')
