#pragma once
#include <cstdint>
#include <cstddef>
#include <type_traits>
namespace x86bridge {
// Version 3: the panel rebuild added five settings fields and the host now reports the
// scale cap, so a v2 peer would read this layout wrong rather than fail. Both sides are
// built together by build-x86bridge.ps1, and a mismatched pair is refused at the header.
constexpr uint32_t Magic=0x42313158, Version=3;
enum class Kind:uint32_t { Hello=1, Build=2, Frame=3, Drop=4, Quit=5, GetState=6, SetState=7, SaveSettings=8, ReloadSettings=9, Command=10, Status=11 };
enum class Result:uint32_t { Original=0, Neural=1, Error=2, Ready=3, Transport=4 };
#pragma pack(push,1)
struct Header { uint32_t magic=Magic,version=Version;Kind kind=Kind::Hello;uint32_t bytes=0; };
struct Hello { uint32_t pid=0,luidLow=0;int32_t luidHigh=0;uint32_t reserved=0; };
struct Texture { uint32_t valid=0,width=0,height=0,format=0;uint64_t handle=0; };
struct Build { uint64_t generation=0;Texture colour,output,depth,motion; };
struct Frame { uint64_t generation=0,id=0;uint32_t depthValid=0,motionValid=0,resetHistory=0,reserved=0; };
struct Ack { Header header;Result result=Result::Error;uint32_t error=0;uint64_t generation=0,frame=0;uint32_t luidLow=0;int32_t luidHigh=0; };

// These control messages are orthogonal to the unchanged FRAME/Ack contract.
enum class CommandCode:uint32_t { MeasureResidualAgain=1, FactoryDefaults=2 };
struct WireSettings {
    uint64_t settings_revision=0;
#define X(type,name,low,high) type name=0;
#include "settings_fields.inc"
#undef X
    uint32_t passOverride[3]{};
    float passStructure[3]{},passTone[3]{},passSkin[3]{};
};
struct WireCommand { uint64_t id=0;CommandCode code=CommandCode::MeasureResidualAgain;uint32_t reserved=0; };
struct WireStatus {
    uint64_t processed=0,skipped=0;
    uint32_t connected=0,engineReady=0,unavailable=0,failed=0,transportOnly=0;
    uint32_t outWidth=0,outHeight=0,netWidth=0,netHeight=0,loadedPasses=0,activePasses=0;
    uint32_t depthActive=0,motionActive=0,probeValid=0;
    float depthMin=0,depthMax=0,motionMean=0,motionMax=0;
    int32_t stillPct=-1,stage=0,events=0;
    uint32_t noBridge=0,noBackBuffer=0;
    // What the helper is actually running at when its own limit holds the scale below the
    // slider. 0 means no cap. Without it the panel would compare the raster against the
    // slider and report a working configuration as "not applied yet" for ever.
    float scaleCap=0;
};
struct StateSnapshot { WireSettings settings;WireStatus status; };
#pragma pack(pop)
static_assert(sizeof(Header)==16 && offsetof(Header,bytes)==12);
static_assert(sizeof(Hello)==16 && offsetof(Hello,luidHigh)==8);
static_assert(sizeof(Texture)==24 && offsetof(Texture,handle)==16);
static_assert(sizeof(Build)==104 && offsetof(Build,motion)==80);
static_assert(sizeof(Frame)==32 && offsetof(Frame,resetHistory)==24);
static_assert(sizeof(Ack)==48 && offsetof(Ack,generation)==24 && offsetof(Ack,luidHigh)==44);
static_assert(std::is_trivially_copyable_v<Build> && std::is_standard_layout_v<Frame>);
static_assert(sizeof(WireSettings)==224 && offsetof(WireSettings,passOverride)==176);
static_assert(sizeof(WireCommand)==16 && offsetof(WireCommand,code)==8);
static_assert(sizeof(WireStatus)==112 && offsetof(WireStatus,depthMin)==72);
static_assert(sizeof(StateSnapshot)==336 && offsetof(StateSnapshot,status)==224);
#define CHECK_WIRE(T) static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>);
CHECK_WIRE(Header) CHECK_WIRE(Hello) CHECK_WIRE(Texture) CHECK_WIRE(Build) CHECK_WIRE(Frame) CHECK_WIRE(Ack)
CHECK_WIRE(WireSettings) CHECK_WIRE(WireCommand) CHECK_WIRE(WireStatus) CHECK_WIRE(StateSnapshot)
#undef CHECK_WIRE
inline uint32_t BodyBytes(Kind k){switch(k){case Kind::Hello:return sizeof(Hello);case Kind::Build:return sizeof(Build);case Kind::Frame:return sizeof(Frame);case Kind::SetState:return sizeof(WireSettings);case Kind::Command:return sizeof(WireCommand);case Kind::GetState:case Kind::Status:case Kind::SaveSettings:case Kind::ReloadSettings:case Kind::Drop:case Kind::Quit:return 0;}return UINT32_MAX;}
inline bool ValidHeader(const Header& h){return BodyBytes(h.kind)!=UINT32_MAX&&h.magic==Magic&&h.version==Version&&h.bytes==BodyBytes(h.kind);}
inline bool ValidTexture(const Texture& t){return t.valid<=1 && (t.valid ? t.width&&t.height&&t.width<=16384&&t.height<=16384&&t.format&&t.handle : !t.handle&&!t.width&&!t.height&&!t.format);}
inline bool ValidBuild(const Build& b){return b.generation&&b.colour.valid&&b.output.valid&&ValidTexture(b.colour)&&ValidTexture(b.output)&&ValidTexture(b.depth)&&ValidTexture(b.motion)&&b.colour.width==b.output.width&&b.colour.height==b.output.height&&b.colour.format==b.output.format;}
inline bool Confirmed(const Ack& a,const Frame& f, bool transport){return a.header.magic==Magic&&a.header.version==Version&&a.header.kind==Kind::Frame&&a.header.bytes==sizeof(Ack)-sizeof(Header)&&a.generation==f.generation&&a.frame==f.id&&(a.result==Result::Neural||(transport&&a.result==Result::Transport));}
}
