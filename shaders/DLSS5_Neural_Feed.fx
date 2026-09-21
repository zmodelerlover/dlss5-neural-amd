/*
    DLSS5_Neural_Feed.fx - companion effect for the DLSS 5 Neural Rendering (AMD) add-on.

    The add-on already estimates motion from the image when a game hands over nothing. That
    estimator is two levels of 3x3 block matching with a search radius of four, and it has to
    fit inside the frame budget next to the network. A dedicated optical-flow shader does not:
    iMMERSE Launchpad runs an eight-level pyramid with a filter between every level. This
    effect hands that field to the add-on, so the estimator becomes the fallback rather than
    the only answer, and it does the same for ReShade's depth buffer on the routes where the
    add-on cannot see the game's own.

      DLSS5N_MV     RG16F  motion vectors as delta UV, prev_uv = uv + mv -- the convention every
                           provider below uses, and the one the add-on's own estimator produces.
                           Vectors that fail validation are zeroed.
      DLSS5N_Depth  R32F   ReShade's depth buffer, with its RESHADE_DEPTH_INPUT_* fixes applied.
                           Used only where the add-on has no depth of its own; a game's own
                           buffer is always better and is preferred when it is there.

    PROVIDER -- set DLSS5N_MV_PROVIDER (ReShade overlay: this effect's "Preprocessor
    definitions") and enable that provider's technique ABOVE this one in the effect list:

      0  texMotionVectors    the shared texture qUINT_motionvectors, dh_uber_motion and
                             ReshadeMotionEstimation write. NOTE: DRME does not compile on
                             ReShade 6.8 and then silently writes nothing.
      1  Launchpad           iMMERSE Launchpad (MartysMods_LAUNCHPAD.fx)            [default]
      2  VORT                vort_Motion.fx
      3  LumeniteFX Kernel   lumenite_Kernel.fx, 1/8 resolution
      4  LumeniteFX QuantMotion

    Nothing of any provider is bundled or included. The selected provider's output texture is
    declared here exactly as the provider declares it, so ReShade binds the same resource --
    the mechanism dh_uber_rt and vort already use. Only the selected one is allocated.

    VALIDATION -- why it is here and not left to the network. Every provider above is optical
    flow: it matches pixels, so a lighting change answers with a vector pointing at whatever
    happened to match. Confidently wrong, and the network then pulls its history in from there.
    That is the warping around flames and the crawling under a flickering light. Each vector is
    reprojected into the previous frame and checked; one that fails is zeroed, which says "this
    surface did not move" -- the right answer for a lit wall.
*/

#include "ReShade.fxh"

#if __RENDERER__ < 0xA000
    #error "DLSS5_Neural_Feed needs D3D10 or newer. On ReShade's DirectX 9 backend the add-on could not be fed anyway."
#endif

#ifndef DLSS5N_MV_PROVIDER
    #define DLSS5N_MV_PROVIDER 1
#endif

// ------------------------------------------------------------------------------------------
// The selected provider's output, declared byte for byte the way the provider declares it.
// ------------------------------------------------------------------------------------------

#if DLSS5N_MV_PROVIDER == 1
    // iMMERSE Launchpad (MartysMods/mmx_deferred.fxh)
    namespace Deferred {
        texture MotionVectorsTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
        // Newer Launchpad builds only run the optical flow when a consumer asked for it during
        // the previous frame: it reads this 1x1 at the top of its technique and clears it at the
        // bottom, bit 4 meaning flow. Being below Launchpad in the list, our request lands for
        // the next frame. Builds without it simply never read what we write here.
        namespace IPC {
            texture2D PredicationBuffer { Format = RGBA8; };
        }
    }
    sampler sDLSS5N_Provider { Texture = Deferred::MotionVectorsTex; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };
    float4 DLSS5N_IpcVS(in uint id : SV_VertexID) : SV_Position { return float4(0.0, 0.0, 0.0, 1.0); }
    float4 DLSS5N_IpcPS(in float4 vpos : SV_Position) : SV_Target0 { return 1.0; }
    #define DLSS5N_PROVIDER_NAME "Launchpad (Deferred::MotionVectorsTex)"
    #define DLSS5N_REQUEST_PASS pass IpcRequestOpticalFlow { PrimitiveTopology = POINTLIST; VertexCount = 1; VertexShader = DLSS5N_IpcVS; PixelShader = DLSS5N_IpcPS; RenderTarget = Deferred::IPC::PredicationBuffer; RenderTargetWriteMask = 4; }
#elif DLSS5N_MV_PROVIDER == 2
    // VORT (Includes/vort_MotionUtils.fxh, V_MV_MODE 1)
    texture2D MotVectTexVort { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
    sampler sDLSS5N_Provider { Texture = MotVectTexVort; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };
    #define DLSS5N_PROVIDER_NAME "VORT (MotVectTexVort)"
#elif DLSS5N_MV_PROVIDER == 3
    // LumeniteFX Kernel (lumenite_Kernel.fx), as lumenite_RTAO re-declares it. 1/8 resolution.
    namespace Kernel {
        texture2D tFlow { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = RG16F; };
    }
    sampler sDLSS5N_Provider { Texture = Kernel::tFlow; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Linear; MagFilter = Linear; };
    #define DLSS5N_PROVIDER_NAME "LumeniteFX Kernel (Kernel::tFlow, 1/8 res)"
#elif DLSS5N_MV_PROVIDER == 4
    // LumeniteFX QuantMotion (lumenite_QuantMotion.fx), as lumenite_QuantAO re-declares it.
    namespace QuantMotion {
        texture2D tFlow { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = RG16F; };
    }
    sampler sDLSS5N_Provider { Texture = QuantMotion::tFlow; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Linear; MagFilter = Linear; };
    #define DLSS5N_PROVIDER_NAME "LumeniteFX QuantMotion (QuantMotion::tFlow, 1/8 res)"
#else
    texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
    sampler sDLSS5N_Provider { Texture = texMotionVectors; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };
    #define DLSS5N_PROVIDER_NAME "texMotionVectors (qUINT, dh_uber_motion, DRME)"
#endif

#ifndef DLSS5N_REQUEST_PASS
    #define DLSS5N_REQUEST_PASS
#endif

// ------------------------------------------------------------------------------------------

uniform int PROVIDER_INFO <
    ui_type = "radio";
    ui_label = " ";
    ui_text = "Motion vector provider: " DLSS5N_PROVIDER_NAME "\n"
              "Change it with the DLSS5N_MV_PROVIDER preprocessor definition:\n"
              "  0 texMotionVectors   1 Launchpad   2 VORT\n"
              "  3 LumeniteFX Kernel  4 LumeniteFX QuantMotion\n"
              "Enable that provider's technique ABOVE this one.";
>;

uniform bool VALIDATE <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_label = "Validate the vectors against the previous frame";
    ui_tooltip = "Optical flow answers a lighting change with a vector that points at whatever happened\n"
                 "to match. Reprojecting and checking catches those, and the vector is zeroed so the\n"
                 "network treats the surface as static -- which is what it is.";
> = true;

uniform bool VALIDATE_STATIC <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_label = "Static-hypothesis test";
    ui_tooltip = "For each pixel, asks which explains it better: 'did not move', or the provider's vector.\n"
                 "Both are scored on illumination-normalised 3x3 structure, with the local mean removed,\n"
                 "so a flickering light is not mistaken for motion. This is the test for a flickering wall.";
> = true;

uniform bool STATIC_HYSTERESIS <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_label = "Static test: require two frames in a row";
    ui_tooltip = "The test has no memory of its own: on a low-contrast surface under a slow pan it can win\n"
                 "one frame and lose the next, so the vector alternates between the provider's and zero and\n"
                 "the network alternately reprojects and does not -- a judder no later pass can smooth out.\n"
                 "With this on, a vector is only zeroed where the test won on this frame and the last.";
> = true;

uniform float STATIC_BIAS <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Static bias";
    ui_tooltip = "How much worse, relatively, the static explanation may score and still win.\n"
                 "0 means the vector must strictly beat 'did not move'. Higher favours zero vectors.";
> = 0.15;

uniform float STATIC_MIN_CONTRAST <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.1; ui_step = 0.001;
    ui_label = "Static test: minimum patch contrast";
    ui_tooltip = "Below this 3x3 contrast a patch has no structure to judge motion by and the test abstains,\n"
                 "leaving the provider's vector. Raise it if flat surfaces trail while moving; lower it if a\n"
                 "flickering wall stops being caught.";
> = 0.012;

uniform bool VALIDATE_DEPTH <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_label = "Depth test (disocclusion)";
    ui_tooltip = "The reprojected previous depth must match the current one. A mismatch means the vector\n"
                 "points at a different surface, so it is zeroed. Sky is exempt.";
> = true;

uniform float DEPTH_TOLERANCE <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.005;
    ui_label = "Depth tolerance";
    ui_tooltip = "Allowed relative difference between the reprojected previous depth and the current one.";
> = 0.10;

uniform bool VALIDATE_MV <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_label = "Consistency test";
    ui_tooltip = "This frame's vector must resemble the previous frame's vector where it points.\n"
                 "Real motion is smooth frame to frame; flow over fire, smoke or a flickering wall is not.";
> = true;

uniform float MV_CONSISTENCY <
    ui_category = "Validation (flicker, flames, disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 16.0; ui_step = 0.1;
    ui_label = "Vector consistency (px)";
    ui_tooltip = "Allowed change in pixels between this frame's vector and the previous frame's vector at\n"
                 "the reprojected spot, plus half the vector's length.";
> = 1.4;

uniform float2 MV_SIGN <
    ui_type = "drag"; ui_min = -1.0; ui_max = 1.0; ui_step = 2.0;
    ui_label = "Motion vector sign (x, y)";
    ui_tooltip = "Flip a component if the picture smears in that direction while moving. The default matches\n"
                 "the convention every provider above uses, prev_uv = uv + mv.";
> = float2(1.0, 1.0);

// Outputs the add-on reads.
texture DLSS5N_MV    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
texture DLSS5N_Depth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F;  };
sampler sDLSS5N_MV   { Texture = DLSS5N_MV; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Previous-frame state the validation needs. Luma may be interpolated, being a smooth quantity;
// depth and vectors must not be, because bilinear across an object edge mixes two surfaces and
// then fails the test on every edge in motion.
texture DLSS5N_PrevLuma  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F;  };
texture DLSS5N_PrevDepth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F;  };
texture DLSS5N_PrevMV    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
sampler sDLSS5N_PrevLuma  { Texture = DLSS5N_PrevLuma;  AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5N_PrevDepth { Texture = DLSS5N_PrevDepth; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; };
sampler sDLSS5N_PrevMV    { Texture = DLSS5N_PrevMV;    AddressU = Clamp; AddressV = Clamp; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; };

// The static decision, this frame and last. A texture cannot be sampled and written in one
// pass, which is why the hysteresis needs two of them.
texture DLSS5N_StaticNow  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
texture DLSS5N_PrevStatic { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
sampler sDLSS5N_StaticNow  { Texture = DLSS5N_StaticNow;  AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5N_PrevStatic { Texture = DLSS5N_PrevStatic; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// ------------------------------------------------------------------------------------------

float2 ProviderMV(float2 uv)
{
    return tex2Dlod(sDLSS5N_Provider, float4(uv, 0.0, 0.0)).xy * MV_SIGN;
}

float Luma(float2 uv)
{
    return dot(tex2Dlod(ReShade::BackBuffer, float4(uv, 0.0, 0.0)).rgb, float3(0.299, 0.587, 0.114));
}

// Illumination-normalised 3x3 structure difference between the current frame at uv_cur and the
// previous frame at uv_prev. Each patch has its own mean removed first, so a brightness change
// contributes nothing and only the pattern is compared. Also returns the current patch's
// contrast: a patch with no structure cannot decide anything and the caller must not pretend
// that it can.
float PatchError(float2 uv_cur, float2 uv_prev, out float contrast)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    float c[9], p[9];
    float mc = 0.0, mp = 0.0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        const float2 o = float2(i % 3 - 1, i / 3 - 1) * px;
        c[i] = Luma(uv_cur + o);
        p[i] = tex2Dlod(sDLSS5N_PrevLuma, float4(uv_prev + o, 0.0, 0.0)).x;
        mc += c[i]; mp += p[i];
    }
    mc /= 9.0; mp /= 9.0;
    float err = 0.0;
    contrast = 0.0;
    [unroll] for (int j = 0; j < 9; ++j)
    {
        err      += abs((c[j] - mc) - (p[j] - mp));
        contrast += abs(c[j] - mc);
    }
    contrast /= 9.0;
    return err / 9.0;
}

// ------------------------------------------------------------------------------------------

void PS_Guides(float4 vpos : SV_Position, float2 uv : TEXCOORD,
               out float2 mv : SV_Target0, out float depth : SV_Target1, out float stat : SV_Target2)
{
    depth = ReShade::GetLinearizedDepth(uv);
    mv    = ProviderMV(uv);
    stat  = 0.0;

    const float2 puv = uv + mv;
    // Reprojecting off the screen leaves nothing to compare against. The vector stands; the
    // network's own history handling owns that case.
    if (!VALIDATE || any(puv < 0.0) || any(puv > 1.0))
        return;

    bool zero = false;

    // Static hypothesis. Skipped under half a pixel, where there is nothing to decide.
    if (VALIDATE_STATIC && length(mv * BUFFER_SCREEN_SIZE) > 0.5)
    {
        float sc, unused;
        const float es = PatchError(uv, uv, sc);
        const float ef = PatchError(uv, puv, unused);
        // Only a patch with structure can tell the two apart. Below the contrast floor the
        // scores tie for lack of evidence, and a tie goes to the provider: its flow there is
        // propagated from textured neighbours, which is the right guess for a moving flat wall.
        if (sc >= STATIC_MIN_CONTRAST && es + 0.25 * sc <= ef * (1.0 + STATIC_BIAS))
        {
            stat = 1.0;
            // With hysteresis the first win only records itself. The vector is zeroed once the
            // test has won twice running, which is what stops the alternation.
            zero = zero || !STATIC_HYSTERESIS || tex2Dlod(sDLSS5N_PrevStatic, float4(uv, 0.0, 0.0)).x > 0.5;
        }
    }

    // Disocclusion: the vector points at a different surface than the one it came from.
    if (VALIDATE_DEPTH && depth < 0.999)
    {
        const float dp  = tex2Dlod(sDLSS5N_PrevDepth, float4(puv, 0.0, 0.0)).x;
        const float tol = DEPTH_TOLERANCE * max(depth, 1e-3);
        zero = zero || abs(dp - depth) > tol;
    }

    // Consistency: real motion is smooth frame to frame, flow over fire is not.
    if (VALIDATE_MV && MV_CONSISTENCY > 0.0)
    {
        const float2 pmv   = tex2Dlod(sDLSS5N_PrevMV, float4(puv, 0.0, 0.0)).xy;
        const float  diff  = length((mv - pmv) * BUFFER_SCREEN_SIZE);
        const float  allow = MV_CONSISTENCY + 0.5 * length(mv * BUFFER_SCREEN_SIZE);
        zero = zero || diff > allow;
    }

    if (zero)
        mv = float2(0.0, 0.0);
}

void PS_History(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                out float luma : SV_Target0, out float depth : SV_Target1,
                out float2 mv : SV_Target2, out float stat : SV_Target3)
{
    luma  = Luma(uv);
    depth = ReShade::GetLinearizedDepth(uv);
    mv    = tex2Dlod(sDLSS5N_MV, float4(uv, 0.0, 0.0)).xy;
    stat  = tex2Dlod(sDLSS5N_StaticNow, float4(uv, 0.0, 0.0)).x;
}

technique DLSS5_Neural_Feed
<
    ui_label = "DLSS 5 Neural Feed (put this ABOVE nothing; the add-on reads it at present)";
    ui_tooltip = "Hands the add-on a real optical-flow field and ReShade's depth buffer.\n"
                 "Enable the motion-vector provider's own technique ABOVE this one.";
>
{
    DLSS5N_REQUEST_PASS
    pass Guides
    {
        VertexShader = PostProcessVS; PixelShader = PS_Guides;
        RenderTarget0 = DLSS5N_MV; RenderTarget1 = DLSS5N_Depth; RenderTarget2 = DLSS5N_StaticNow;
    }
    pass History
    {
        VertexShader = PostProcessVS; PixelShader = PS_History;
        RenderTarget0 = DLSS5N_PrevLuma; RenderTarget1 = DLSS5N_PrevDepth;
        RenderTarget2 = DLSS5N_PrevMV;   RenderTarget3 = DLSS5N_PrevStatic;
    }
}
