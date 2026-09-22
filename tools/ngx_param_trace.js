// Frida script: log every NGX parameter RenoDX hands to the DLSS-NR feature.
//
// Extended replacement for ngx_param_trace.js. The original hooks the exported C functions
// NVSDK_NGX_Parameter_SetUI/SetI/SetF/SetD/SetULL. Those exports DO NOT EXIST: _nvngx.dll
// exports 75 symbols and none of them is a parameter setter. They are inline helpers in
// nvsdk_ngx_helpers.h that call virtual methods on the NVSDK_NGX_Parameter object. The
// original also reads floats from this.context.xmm2, which Frida's x64 CpuContext does not
// expose, so its float path could never have printed a value either.
//
// What this one does instead:
//   1. hooks Create/Evaluate/Release Feature (D3D11 and D3D12) like the original;
//   2. takes the NVSDK_NGX_Parameter* those calls carry, reads its vtable, and attaches to
//      the eight Set() slots found there. Attach only -- arguments are never modified;
//   3. reads float/double values back out of the object through the Get() slots, because
//      they arrive in xmm2 where an attach cannot see them;
//   4. can dump a full snapshot of every DLSSNR.* parameter by calling Get() for each known
//      name, so parameters set before we attached are still recovered;
//   5. captures OutputDebugStringA/W and CreateFileW so the NGX "DLSSNR:" lines are caught
//      even if the driver logs to debug output rather than to a file.
//
// Driven by run_trace.py, not by the frida CLI (the CLI is a REPL and needs a live stdin).

'use strict';

/* ------------------------------------------------------------------ configuration */

// Parameters worth printing. Everything else is dropped unless LOG_ALL.
const LOG_ALL = false;
const NAME_RE = /^(DLSSNR\.|DLSS\.|Creation|Visibility)/;

// Controls: never folded away, however often they repeat.
const CONTROLS = [
  'DLSSNR.Style', 'DLSSNR.UseAutoMask', 'DLSSNR.Intensity',
  'DLSSNR.LocalToneStrength', 'DLSSNR.LocalStructureStrength',
  'DLSSNR.SkinStructureStrength', 'DLSSNR.GlobalToneStrength',
  'DLSSNR.Reset', 'DLSSNR.DepthInverted', 'DLSSNR.Enabled', 'DLSSNR.UICorrection',
  'DLSSNR.Hint.Render.Preset', 'DLSSNR.MVecScaleX', 'DLSSNR.MVecScaleY',
  'DLSSNR.Width', 'DLSSNR.Height', 'DLSSNR.InputWidth', 'DLSSNR.InputHeight',
  'DLSSNR.OutputWidth', 'DLSSNR.OutputHeight', 'DLSSNR.Output.Width', 'DLSSNR.Output.Height',
  'DLSSNR.ScalingRatio', 'DLSSNR.Scale', 'DLSSNR.Upscaling', 'DLSSNR.UIAlpha',
];

// Everything else stops printing after this many distinct values (resource pointers and
// jitter change every frame and would drown the file).
const CAP = 6;

// Names probed by snapshot(). Taken from the strings in renodx-dlss.addon64.
const PROBE = CONTROLS.concat([
  'DLSSNR.JitterOffsetX', 'DLSSNR.JitterOffsetY',
  'DLSSNR.Color', 'DLSSNR.MVec', 'DLSSNR.Depth', 'DLSSNR.Output',
  'DLSSNR.ControlMask', 'DLSSNR.UI', 'DLSSNR.Backbuffer',
  'DLSSNR.BidirectionalDistortionField',
  'CreationNodeMask', 'VisibilityNodeMask',
  'DLSS.Indicator.Invert.X.Axis', 'DLSS.Indicator.Invert.Y.Axis',
  'DLSS.Feature.Create.Flags',
  'DLSS.Render.Subrect.Dimensions.Width', 'DLSS.Render.Subrect.Dimensions.Height',
  'DLSS.Input.Depth.Subrect.Base.X', 'DLSS.Input.Depth.Subrect.Base.Y',
  'DLSS.Input.MV.Subrect.Base.X', 'DLSS.Input.MV.Subrect.Base.Y',
  'DLSS.Output.Subrect.Base.X', 'DLSS.Output.Subrect.Base.Y',
]);

// NVSDK_NGX_Parameter vtable. Slots 0-7 Set, 8-15 Get, 16 Reset -- but NOT in the order
// nvsdk_ngx_params.h declares them: MSVC emits a set of overloaded virtuals into the vtable
// in REVERSE declaration order, so the header's (ULL, float, double, uint, int, ID3D11Res*,
// ID3D12Res*, void*) comes out as (void*, ID3D12Res*, ID3D11Res*, int, uint, double, float,
// ULL). Confirmed against _nvngx.dll by disassembly: slot 5 stores with movsd and tags the
// variant 5, slot 6 stores with movss and tags it 4, slots 3/4 consume r8d, slots 0/1/7
// consume r8, and slots 2 and 10 -- the two ID3D11Resource entries -- are stubs (a bare ret
// and a "mov eax, 0xBAD00010; ret"). Confirmed again at runtime: DLSSNR.Color, .MVec, .Depth,
// .Output, .UI and .UIAlpha all arrive at slot 1, the D3D12 resource setter.
const SET_SLOTS = [
  ['PTR', 'ptr'], ['R12', 'ptr'], ['R11', 'stub'], ['I', 'i32'],
  ['UI', 'u32'], ['D', 'f64'], ['F', 'f32'], ['ULL', 'u64'],
];
const SKIP_SET_SLOTS = [2];   // bare "ret" stub, too short to trampoline and never called
const GET_PTR = 8, GET_I = 11, GET_UI = 12, GET_D = 13, GET_F = 14, GET_ULL = 15;

// Where that vtable sits in this _nvngx.dll (610.62 / nvlti.inf_amd64_7566d6b2a7331e4e).
// Hooking it straight away matters: RenoDX installs its own inline hooks on the NGX exports
// ("utils::dlss::nvngx loaded-module hooks active 3/4") and calls the originals through its
// own trampolines, so waiting to catch a CreateFeature or EvaluateFeature at the export can
// miss every call. SLOT0_RVA is checked before use, so a different build just falls back to
// discovering the object from whatever call we do see.
const KNOWN_VTABLE_RVA = 0xB59B8;
const KNOWN_SLOT0_RVA = 0x37A0;

const NGX_SUCCESS = 1;

/* ------------------------------------------------------------------ plumbing */

const lastValue = new Map();   // name -> last rendered line body
const printCount = new Map();  // name -> how many times printed
const capped = new Set();
const hookedVTables = new Set();
const hookedAddrs = new Set();
const cstrCache = new Map();

let paramObj = null;           // most recent NVSDK_NGX_Parameter*
let getF = null, getD = null, getUI = null, getI = null, getULL = null, getPtr = null;
const scratch = Memory.alloc(16);   // out-parameter for the Get() read-backs
let evalCount = 0;

function p2(n) { return n < 10 ? '0' + n : '' + n; }
function p3(n) { return n < 10 ? '00' + n : (n < 100 ? '0' + n : '' + n); }
function stamp() {
  const d = new Date();   // local time, to line up with the operator's notes
  return p2(d.getHours()) + ':' + p2(d.getMinutes()) + ':' + p2(d.getSeconds()) + '.' + p3(d.getMilliseconds());
}
function out(s) { console.log(s); }

function cstr(s) {
  let p = cstrCache.get(s);
  if (!p) { p = Memory.allocUtf8String(s); cstrCache.set(s, p); }
  return p;
}

function globalExport(name) {
  try { if (typeof Module.getGlobalExportByName === 'function') return Module.getGlobalExportByName(name); } catch (e) {}
  try { if (typeof Module.findExportByName === 'function') return Module.findExportByName(null, name); } catch (e) {}
  return null;
}
function modExport(mod, name) {
  try { if (typeof mod.findExportByName === 'function') { const a = mod.findExportByName(name); if (a) return a; } } catch (e) {}
  try { if (typeof Module.findExportByName === 'function') return Module.findExportByName(mod.name, name); } catch (e) {}
  return null;
}
function findModule(name) {
  try { const m = Process.findModuleByName(name); if (m) return m; } catch (e) {}
  const want = name.toLowerCase();
  try {
    for (const m of Process.enumerateModules()) if (m.name.toLowerCase() === want) return m;
  } catch (e) {}
  return null;
}

/* ------------------------------------------------------------------ value read-back */

function getNum(fn, p, np, reader) {
  if (!fn) return null;
  try {
    scratch.writeU64(0);
    if (fn(p, np, scratch) !== NGX_SUCCESS) return null;
    return reader(scratch);
  } catch (e) { return null; }
}
function readBack(p, np) {
  return {
    f: getNum(getF, p, np, b => b.readFloat()),
    d: getNum(getD, p, np, b => b.readDouble()),
    i: getNum(getI, p, np, b => b.readS32()),
    ui: getNum(getUI, p, np, b => b.readU32()),
    ull: getNum(getULL, p, np, b => b.readU64()),
    ptr: getNum(getPtr, p, np, b => b.readPointer()),
  };
}
function fmtF(v) { return v === null ? '?' : v.toFixed(6); }

/* ------------------------------------------------------------------ printing */

function emit(kind, name, body) {
  if (!LOG_ALL && !NAME_RE.test(name)) return;
  if (lastValue.get(name) === body) return;
  lastValue.set(name, body);
  const n = (printCount.get(name) || 0) + 1;
  printCount.set(name, n);
  if (CONTROLS.indexOf(name) < 0 && n > CAP) {
    if (!capped.has(name)) { capped.add(name); out(`${stamp()}  ....  ${name}: changes past ${CAP} folded`); }
    return;
  }
  out(`${stamp()}  ${(kind + '     ').substr(0, 5)} ${name} = ${body}`);
}

function snapshot(tag) {
  if (!paramObj) { out(`${stamp()}  ==== snapshot ${tag}: no parameter object yet`); return; }
  out(`${stamp()}  ==== snapshot ${tag} (param=${paramObj}, evaluates=${evalCount})`);
  for (const name of PROBE) {
    const rb = readBack(paramObj, cstr(name));
    if (rb.f === null && rb.ui === null && rb.ull === null && rb.ptr === null) continue;
    const pad = (name + '                                         ').substr(0, 41);
    if (rb.ptr !== null && rb.f === null && rb.ui === null) { out(`          ${pad} res=${rb.ptr}`); continue; }
    out(`          ${pad} f=${fmtF(rb.f)}  ui=${rb.ui === null ? '?' : rb.ui}  i=${rb.i === null ? '?' : rb.i}  u64=${rb.ull === null ? '?' : rb.ull.toString()}`);
  }
  out(`${stamp()}  ==== end snapshot ${tag}`);
}

/* ------------------------------------------------------------------ vtable hooking */

function describeSlot(addr) {
  // Disassemble the head of the function and report whether it touches xmm registers.
  // Confirms which slot is the float/double setter without trusting the header layout.
  let xmm = false, insns = 0, mnem = [];
  try {
    let ip = addr;
    for (let i = 0; i < 24; i++) {
      const ins = Instruction.parse(ip);
      insns++;
      if (i < 4) mnem.push(ins.mnemonic);
      if (/xmm/.test(ins.opStr || '')) xmm = true;
      if (/^(ret|jmp)/.test(ins.mnemonic)) break;
      ip = ins.next;
    }
  } catch (e) {}
  return { xmm: xmm, insns: insns, head: mnem.join(' ') };
}

function hookVTable(vt) {
  const key = vt.toString();
  if (hookedVTables.has(key)) return;
  hookedVTables.add(key);

  let slots;
  try { slots = []; for (let i = 0; i < 17; i++) slots.push(vt.add(i * 8).readPointer()); }
  catch (e) { out(`${stamp()}  !!!! vtable ${vt} unreadable: ${e}`); return; }

  out(`${stamp()}  ---- NVSDK_NGX_Parameter vtable ${vt}`);
  for (let i = 0; i < 8; i++) {
    const d = describeSlot(slots[i]);
    const m = Process.findModuleByAddress ? Process.findModuleByAddress(slots[i]) : null;
    out(`          slot ${i} ${SET_SLOTS[i][0].padEnd(4)} ${slots[i]}  xmm=${d.xmm ? 'YES' : 'no '}  ${m ? m.name : '?'}!+0x${(m ? slots[i].sub(m.base) : ptr(0)).toString(16)}  [${d.head}]`);
  }
  out(`          expect xmm=YES on slots 5 (double) and 6 (float) only, per the reverse-order`);
  out(`          layout above; anything else means this build's NVSDK_NGX_Parameter differs again.`);

  try {
    const G = a => new NativeFunction(a, 'uint32', ['pointer', 'pointer', 'pointer'], 'win64');
    getPtr = G(slots[GET_PTR]);
    getI = G(slots[GET_I]);
    getUI = G(slots[GET_UI]);
    getD = G(slots[GET_D]);
    getF = G(slots[GET_F]);
    getULL = G(slots[GET_ULL]);
  } catch (e) { out(`${stamp()}  !!!! could not bind Get slots: ${e}`); }

  // Identical Set overloads can be folded together by the linker (/OPT:ICF), so several
  // slots may share one address. Attach once per address and label it with every slot.
  const byAddr = new Map();
  for (let i = 0; i < 8; i++) {
    if (SKIP_SET_SLOTS.indexOf(i) >= 0) continue;
    const a = slots[i].toString();
    if (!byAddr.has(a)) byAddr.set(a, { addr: slots[i], idx: [] });
    byAddr.get(a).idx.push(i);
  }

  let n = 0;
  for (const entry of byAddr.values()) {
    const a = entry.addr.toString();
    if (hookedAddrs.has(a)) continue;
    const idx = entry.idx;
    try {
    const kind = idx.map(i => SET_SLOTS[i][0]).join('/');
    const isDouble = idx.indexOf(5) >= 0;                      // Set(double)
    const wantsFloat = isDouble || idx.indexOf(6) >= 0;        // Set(double) / Set(float): value is in xmm2
    const isPtr = idx.indexOf(0) >= 0 || idx.indexOf(1) >= 0;  // Set(void*) / Set(ID3D12Resource*)
    const is64 = idx.indexOf(7) >= 0;                          // Set(unsigned long long)
    Interceptor.attach(entry.addr, {
      onEnter(args) {
        this.self = args[0];
        this.np = args[1];
        this.raw = args[2];
        try { this.name = args[1].readCString(); } catch (e) { this.name = null; }
      },
      onLeave() {
        if (!this.name) return;
        paramObj = this.self;
        let body;
        if (wantsFloat) {
          // the value arrived in xmm2, which an attach cannot read; take it back out of the object
          const rb = readBack(this.self, this.np);
          const v = isDouble ? (rb.d === null ? '?' : rb.d.toFixed(6)) : fmtF(rb.f);
          body = `${v}   {rb ui=${rb.ui === null ? '?' : rb.ui}}`;
        } else if (isPtr) {
          body = `${this.raw}`;
        } else if (is64) {
          body = `${this.raw.toString()}  (0x${this.raw.toString(16)})`;
        } else {
          const u = this.raw.toUInt32();
          const s = this.raw.toInt32();
          const rb = readBack(this.self, this.np);
          body = `${u}${s !== u ? ' (i32 ' + s + ')' : ''}   {rb f=${fmtF(rb.f)}}`;
        }
        emit(kind, this.name, body);
      }
    });
    hookedAddrs.add(a);
    n++;
    } catch (e) {
      // a one-instruction stub has no room for a trampoline; skipping it must not abort
      // the loop, or every later slot -- which is where all the scalars are -- stays unhooked
      out(`${stamp()}  !!!! slot(s) ${idx.join(',')} at ${a} not hookable: ${e}`);
    }
  }
  out(`${stamp()}  ---- attached to ${n} distinct Set implementation(s)`);
  snapshot('vtable-attach');
}

function noteParam(p, where) {
  if (p === null || p.isNull()) return;
  paramObj = p;
  let vt;
  try { vt = p.readPointer(); } catch (e) { return; }
  if (vt.isNull()) return;
  if (!hookedVTables.has(vt.toString())) {
    out(`${stamp()}  ---- parameter object ${p} seen at ${where}`);
    hookVTable(vt);
  }
}

/* ------------------------------------------------------------------ feature hooks */

function hookFeatures(mod) {
  let n = 0;
  for (const api of ['D3D11', 'D3D12', 'CUDA', 'VULKAN']) {
    for (const verb of ['AllocateParameters', 'GetCapabilityParameters', 'GetParameters']) {
      const sym = `NVSDK_NGX_${api}_${verb}`;
      const addr = modExport(mod, sym);
      if (!addr) continue;
      n++;
      Interceptor.attach(addr, {
        onEnter(args) { this.o = args[0]; },
        onLeave() { try { noteParam(this.o.readPointer(), sym); } catch (e) {} }
      });
    }
    for (const verb of ['CreateFeature', 'CreateFeature1']) {
      const sym = `NVSDK_NGX_${api}_${verb}`;
      const addr = modExport(mod, sym);
      if (!addr) continue;
      n++;
      Interceptor.attach(addr, {
        onEnter(args) {
          const id = args[1].toInt32();
          out(`\n${stamp()}  ---- ${sym} feature ${id}${id === 18 ? ' (DLSS-NR / Reserved18)' : ''}`);
          noteParam(args[2], sym);
          snapshot(`${sym}(${id})`);
        }
      });
    }
    const ev = modExport(mod, `NVSDK_NGX_${api}_EvaluateFeature`);
    if (ev) {
      n++;
      Interceptor.attach(ev, {
        onEnter(args) {
          evalCount++;
          if (evalCount === 1 || evalCount % 2000 === 0) out(`${stamp()}  ---- EvaluateFeature #${evalCount}`);
          noteParam(args[2], 'EvaluateFeature');
        }
      });
    }
    const rl = modExport(mod, `NVSDK_NGX_${api}_ReleaseFeature`);
    if (rl) {
      n++;
      Interceptor.attach(rl, { onEnter() { out(`${stamp()}  ---- NVSDK_NGX_${api}_ReleaseFeature`); } });
    }
  }
  out(`[trace] hooked ${n} feature entry points in ${mod.name} (${mod.path})`);

  // Hook the parameter vtable straight away at the RVA this build keeps it at, instead of
  // waiting to see it carried into a call we might never intercept. Verified before use.
  if (mod.name.toLowerCase() === '_nvngx.dll') {
    try {
      const vt = mod.base.add(KNOWN_VTABLE_RVA);
      const s0 = vt.readPointer();
      if (s0.equals(mod.base.add(KNOWN_SLOT0_RVA))) {
        out(`[trace] parameter vtable at known RVA 0x${KNOWN_VTABLE_RVA.toString(16)} (base ${mod.base})`);
        hookVTable(vt);
      } else {
        out(`[trace] RVA 0x${KNOWN_VTABLE_RVA.toString(16)} is not the vtable in this build (slot0=${s0}, expected ${mod.base.add(KNOWN_SLOT0_RVA)}); falling back to discovery`);
      }
    } catch (e) { out(`[trace] direct vtable hook failed: ${e}`); }
  }
}

/* ------------------------------------------------------------------ NGX log capture */

function hookLogging() {
  for (const sym of ['OutputDebugStringA', 'OutputDebugStringW']) {
    const a = globalExport(sym);
    if (!a) continue;
    const wide = sym.endsWith('W');
    Interceptor.attach(a, {
      onEnter(args) {
        let s = null;
        try { s = wide ? args[0].readUtf16String() : args[0].readCString(); } catch (e) {}
        if (!s) return;
        if (/DLSSNR|NGX|preset|config\(s\)|temporal history/i.test(s)) {
          out(`${stamp()}  DBG   ${s.replace(/[\r\n]+$/, '')}`);
        }
      }
    });
  }
  const cf = globalExport('CreateFileW');
  if (cf) {
    const seen = new Set();
    Interceptor.attach(cf, {
      onEnter(args) {
        let s = null;
        try { s = args[0].readUtf16String(); } catch (e) {}
        if (!s || seen.has(s)) return;
        if (/\.(log|txt)$/i.test(s) && /ngx|dlss|nvidia/i.test(s)) {
          seen.add(s);
          out(`${stamp()}  FILE  CreateFileW ${s}  (access=0x${args[1].toUInt32().toString(16)})`);
        }
      }
    });
  }
  out('[trace] logging capture installed (OutputDebugString + CreateFileW)');
}

/* ------------------------------------------------------------------ bring-up */

function tryHook() {
  let any = false;
  for (const name of ['_nvngx.dll', 'nvngx.dll']) {
    const mod = findModule(name);
    if (mod) { hookFeatures(mod); any = true; }
  }
  return any;
}

rpc.exports = {
  mark(text) {
    out(`\n${stamp()}  ================ ${text}`);
    snapshot(text);
    return true;
  },
  snap() { snapshot('manual'); return true; },
  status() {
    return {
      vtables: hookedVTables.size,
      setImpls: hookedAddrs.size,
      evaluates: evalCount,
      param: paramObj ? paramObj.toString() : null,
      names: Array.from(lastValue.keys()).sort(),
    };
  },
};

out(`[trace] frida ${Frida.version}, pid ${Process.id}, ${Process.arch}`);
hookLogging();
if (!tryHook()) {
  out('[trace] NGX core not loaded yet; waiting for it');
  const timer = setInterval(() => { if (tryHook()) clearInterval(timer); }, 500);
}
