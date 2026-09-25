#pragma once
#include "bridge_ipc.h"
#include "../shared/history_keys.h"
#include <algorithm>
#include <cmath>
namespace x86bridge {
// Non-finite values reject the entire update; finite values clamp to the settings table's ranges,
// the same ones the 64-bit route clamps its ini and its panel to (Inline to 1 among them). The
// per-pass profiles follow Structure, Tone and Skin, which the table leaves unbounded, so a
// per-pass Skin keeps its -1 automatic.
inline bool NormalizeSettings(WireSettings& s){
#define X(type,name,key,def,low,high) if(!std::isfinite(static_cast<double>(s.name)))return false;
#include "settings_fields.inc"
#undef X
    for(unsigned i=0;i<3;++i)if(!std::isfinite(s.passStructure[i])||!std::isfinite(s.passTone[i])||!std::isfinite(s.passSkin[i]))return false;
#define X(type,name,key,def,low,high) s.name=(std::clamp)(s.name,static_cast<type>(low),static_cast<type>(high));
#include "settings_fields.inc"
#undef X
    for(unsigned i=0;i<3;++i)s.passOverride[i]=(std::min)(s.passOverride[i],1u);
    return true;
}
// base is the table's defaults, as the helper captured them before reading the ini: the same
// Factory Defaults as the 64-bit route (ui::KeepPreferences). Which optional controls have a widget
// is a panel preference, like the language and the hotkey, so it is left alone rather than
// emptying a panel somebody arranged.
inline WireSettings FactorySettings(WireSettings base,const WireSettings& current){
    base.enabled=current.enabled; base.startOn=current.startOn; base.toggleKey=current.toggleKey;
    base.toggleMods=current.toggleMods; base.disableOnAltTab=current.disableOnAltTab; base.language=current.language;
    base.optional=current.optional;
    base.settings_revision=current.settings_revision+1; return base;
}
inline bool NewRevision(uint64_t incoming,uint64_t current){return incoming>current;}
inline bool NewCommand(const WireCommand& c,uint64_t last){return c.id>last&&(c.code==CommandCode::MeasureResidualAgain||c.code==CommandCode::FactoryDefaults||c.code==CommandCode::LiftScaleCap)&&c.reserved==0;}
}
