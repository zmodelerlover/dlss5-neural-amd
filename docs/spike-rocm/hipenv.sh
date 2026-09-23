# source this before calling hipcc. hipcc's clang needs MSVC/SDK paths via INCLUDE/LIB.
export ROCM="C:/Program Files/AMD/ROCm/7.1"
MSVC="C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.50.35717"
SDK="D:/Windows Kits/10"; SDKV="10.0.26100.0"
export INCLUDE="$(cygpath -w "$MSVC/include");$(cygpath -w "$SDK/Include/$SDKV/ucrt");$(cygpath -w "$SDK/Include/$SDKV/um");$(cygpath -w "$SDK/Include/$SDKV/shared")"
export LIB="$(cygpath -w "$MSVC/lib/x64");$(cygpath -w "$SDK/Lib/$SDKV/ucrt/x64");$(cygpath -w "$SDK/Lib/$SDKV/um/x64")"
export PATH="$ROCM/bin:$PATH"
hipbuild() { "$ROCM/bin/hipcc.exe" --offload-arch=gfx1201 "$@"; }
