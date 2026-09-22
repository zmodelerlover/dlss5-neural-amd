#include "control_state.h"
#include <limits>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>
int main(){
    using namespace x86bridge;
    Header h;h.bytes=sizeof(Hello);assert(ValidHeader(h));h.version++;assert(!ValidHeader(h));
    h=Header{};h.kind=static_cast<Kind>(99);h.bytes=UINT32_MAX;assert(!ValidHeader(h));
    Build b;b.generation=1;b.colour={1,1920,1080,28,0x123456789abcdef0ULL};b.output=b.colour;b.output.handle++;
    assert(ValidBuild(b));Build copy;std::memcpy(&copy,&b,sizeof(b));assert(copy.colour.handle==b.colour.handle);
    b.depth.valid=2;assert(!ValidBuild(b));b.depth={};b.motion={1,1920,1080,34,42};assert(ValidBuild(b));
    Frame f;f.id=3;f.generation=4;Ack a;a.header.kind=Kind::Frame;a.header.bytes=sizeof(Ack)-sizeof(Header);a.frame=f.id;a.generation=f.generation;
    for(auto status:{Result::Original,Result::Error,Result::Ready}){a.result=status;assert(!Confirmed(a,f,false));}
    a.result=Result::Neural;assert(Confirmed(a,f,false));a.frame--;assert(!Confirmed(a,f,false));a.frame=f.id;a.generation--;assert(!Confirmed(a,f,false));
    a.generation=f.generation;a.result=Result::Transport;assert(!Confirmed(a,f,false)&&Confirmed(a,f,true));
    h=Header{};h.bytes=sizeof(Hello);h.version=1;assert(!ValidHeader(h));h.version=2;assert(!ValidHeader(h));
    for(auto kind:{Kind::GetState,Kind::SetState,Kind::SaveSettings,Kind::ReloadSettings,Kind::Command,Kind::Status}){h=Header{};h.kind=kind;h.bytes=BodyBytes(kind);assert(ValidHeader(h));++h.bytes;assert(!ValidHeader(h));}
    WireSettings settings{};settings.settings_revision=0x123456789abcdef0ULL;
    settings.skin=-1;settings.structure=3;settings.language=1;settings.passOverride[2]=1;settings.passSkin[2]=2;
    WireSettings restored;std::memcpy(&restored,&settings,sizeof(settings));assert(std::memcmp(&settings,&restored,sizeof(settings))==0);
    WireStatus status{};status.processed=0xfedcba9876543210ULL;status.depthActive=1;status.depthMin=.25f;status.stillPct=72;
    WireStatus statusCopy;std::memcpy(&statusCopy,&status,sizeof(status));assert(std::memcmp(&status,&statusCopy,sizeof(status))==0);
    settings.scale=99;settings.toggleMods=99;settings.enabled=42;settings.tonemap=-90;settings.motionScale=-99;settings.inlineMode=0;
    assert(NormalizeSettings(settings));assert(settings.scale==2&&settings.toggleMods==7&&settings.enabled==1&&settings.tonemap==-1&&settings.motionScale==-2&&settings.inlineMode==1&&settings.skin==-1);
    settings.tone=std::numeric_limits<float>::quiet_NaN();assert(!NormalizeSettings(settings));
    settings.tone=1;settings.passSkin[0]=std::numeric_limits<float>::infinity();assert(!NormalizeSettings(settings));
    assert(NewRevision(2,1)&&!NewRevision(1,1)&&!NewRevision(0,1));
    WireCommand c;c.id=1;assert(NewCommand(c,0));uint64_t last=c.id;assert(!NewCommand(c,last));
    c.id=2;c.code=CommandCode::FactoryDefaults;assert(NewCommand(c,last));last=c.id;assert(!NewCommand(c,last));
    c.id=3;c.reserved=1;assert(!NewCommand(c,last));c.reserved=0;c.code=static_cast<CommandCode>(99);assert(!NewCommand(c,last));
    WireSettings base{};base.tone=1;base.scale=.5f;WireSettings current{};current.settings_revision=7;current.enabled=1;current.language=1;current.toggleKey=70;current.startOn=1;
    auto factory=FactorySettings(base,current);assert(factory.colourStrength==0.25f&&factory.structure==1&&factory.skin==-1&&factory.passes==1&&factory.inlineMode==1);
    assert(factory.tone==1&&factory.scale==.5f&&factory.enabled==1&&factory.language==1&&factory.toggleKey==70&&factory.startOn==1&&factory.settings_revision==8);
    printf("PASS protocol v3 WireSettings=224 WireStatus=112 StateSnapshot=336 WireCommand=16; roundtrips, clamps, NaN/Inf, old-version rejection, revisions, command dedup\n");
    printf("PASS fixed-width layouts Header=16 Hello=16 Texture=24 Build=104 Frame=32 Ack=48; malformed/version/old-frame/generation rejected; transport is explicit; pointer_bits=%zu\n",sizeof(void*)*8);
}
