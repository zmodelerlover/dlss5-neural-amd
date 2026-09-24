#pragma once
// Lab only (build.ps1 -Ffx): the FidelityFX SDK's optical flow and its DX12 backend, compiled into
// the target. Included at the top of the unity build, outside the anonymous namespace, because the
// SDK's entry points are extern "C".
#ifndef AMDNR_WITH_FFX
#define AMDNR_WITH_FFX 0
#endif
#if AMDNR_WITH_FFX
#include <ffx_dx12.h>
#include <ffx_opticalflow.h>
#include <ffx_message.h>
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
#include <cfloat>
#endif
