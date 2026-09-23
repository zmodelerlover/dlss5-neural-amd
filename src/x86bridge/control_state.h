#pragma once
#include "bridge_ipc.h"
#include <algorithm>
#include <cmath>
namespace x86bridge {
// Non-finite values reject the entire update; finite values clamp to original UI ranges.
inline bool NormalizeSettings(WireSettings& s){
#define X(type,name,low,high) if(!std::isfinite(static_cast<double>(s.name)))return false;
#include "settings_fields.inc"
#undef X
    for(unsigned i=0;i<3;++i)if(!std::isfinite(s.passStructure[i])||!std::isfinite(s.passTone[i])||!std::isfinite(s.passSkin[i]))return false;
#define X(type,name,low,high) s.name=(std::clamp)(s.name,static_cast<type>(low),static_cast<type>(high));
#include "settings_fields.inc"
#undef X
    for(unsigned i=0;i<3;++i){s.passOverride[i]=(std::min)(s.passOverride[i],1u);s.passStructure[i]=(std::clamp)(s.passStructure[i],0.f,3.f);s.passTone[i]=(std::clamp)(s.passTone[i],0.f,3.f);s.passSkin[i]=(std::clamp)(s.passSkin[i],0.f,3.f);}
    s.inlineMode=1;return true;
}
// Skin is -1, not 1. It is the value the engine boots with and it means "derive it from local
// structure" -- a mode, not a strength. Writing 1 here turned that automatic off on a fresh x86
// install, which is the same bug the 64-bit default had until the panel rebuild.
//
// Which optional controls have a widget is a panel preference, like the language and the hotkey,
// so factory defaults leaves it alone rather than emptying a panel somebody arranged.
inline WireSettings FactorySettings(WireSettings base,const WireSettings& current){
    base.colourStrength=0.25f; base.structure=1; base.skin=-1; base.passes=1; base.inlineMode=1;
    base.enabled=current.enabled; base.startOn=current.startOn; base.toggleKey=current.toggleKey;
    base.toggleMods=current.toggleMods; base.disableOnAltTab=current.disableOnAltTab; base.language=current.language;
    base.optional=current.optional;
    base.settings_revision=current.settings_revision+1; return base;
}
inline bool NewRevision(uint64_t incoming,uint64_t current){return incoming>current;}
inline bool NewCommand(const WireCommand& c,uint64_t last){return c.id>last&&(c.code==CommandCode::MeasureResidualAgain||c.code==CommandCode::FactoryDefaults||c.code==CommandCode::LiftScaleCap)&&c.reserved==0;}
}
