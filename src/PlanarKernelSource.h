#ifndef PLANAR_KERNEL_SOURCE_H
#define PLANAR_KERNEL_SOURCE_H

// ============================================================
// LoFi FX Planar Stabilizer — Metal runtime shader source
//
// Concatenates Metal prefix + shared kernel core (.inc) + suffix.
// The .inc text is embedded by CMake via configure_file.
// ============================================================

#include <string>
#include "PlanarKernelCoreText.h"   // generated: planarKernelCoreSource

namespace {

// ------------------------------------------------------------
// Metal prefix: macros mapping abstraction layer to MSL.
// ============================================================
static const char* kPlanarMetalPrefix = R"METAL_PREFIX(
#include <metal_stdlib>
using namespace metal;

#define DEVFN             inline
#define GLOBAL_RO(T)      device const T*
#define GLOBAL_RW(T)      device T*
#define CONSTANT_REF(T)   constant T&
#define READ_TEX          texture2d<float, access::sample>
#define WRITE_TEX         texture2d<float, access::write>
#define TEX_SAMPLE(t,u,v) (t).sample(planar_linear_sampler, float2((u),(v)))
#define TEX_WRITE(t,x,y,v) (t).write((v), uint2(uint(x), uint(y)))
#define MAKE_FLOAT2(a,b)     float2((a),(b))
#define MAKE_FLOAT3(a,b,c)   float3((a),(b),(c))
#define MAKE_FLOAT4(a,b,c,d) float4((a),(b),(c),(d))

// Linear sampler for frame textures, point sampler for template
constexpr sampler planar_linear_sampler(filter::linear, address::clamp_to_edge);
)METAL_PREFIX";

// ------------------------------------------------------------
// Metal suffix: kernel entry points forwarding to DEVFN bodies.
// ============================================================
static const char* kPlanarMetalSuffix = R"METAL_SUFFIX(
// ---- Pyramid generation ----
kernel void planarPyramidKernel(
    READ_TEX                 srcTex [[texture(0)]],
    WRITE_TEX                dstTex [[texture(1)]],
    constant int2&           dims   [[buffer(0)]],   // (dstW, dstH)
    uint2 gid [[thread_position_in_grid]])
{
    int x = int(gid.x), y = int(gid.y);
    if (x >= dims.x || y >= dims.y) return;
    pyramidGenBody(x, y, srcTex, dstTex, dims.x, dims.y);
}

// ---- NCC tracking search ----
kernel void planarTrackKernel(
    READ_TEX                 frameTex    [[texture(0)]],
    device const float4*     templBuf    [[buffer(0)]],
    constant float*          refStats    [[buffer(1)]],   // 3 floats: [sumT, sumT2, countT]
    device float*            corrOut     [[buffer(2)]],
    constant int2&           tsHalf      [[buffer(3)]],  // (ts, half)
    constant int2&           center      [[buffer(4)]],  // (cx, cy)
    constant int2&           frameDims   [[buffer(5)]],  // (fw, fh)
    constant int&            searchR     [[buffer(6)]],
    constant float2&         rotSc       [[buffer(7)]],  // (cosA, sinA)
    constant float&          invScale    [[buffer(8)]],
    uint2 gid [[thread_position_in_grid]])
{
    int gx = int(gid.x), gy = int(gid.y);
    trackSearchBody(gx, gy,
        frameTex,
        templBuf, refStats, corrOut,
        tsHalf.x, tsHalf.y,
        center.x, center.y,
        frameDims.x, frameDims.y,
        searchR,
        rotSc.x, rotSc.y, invScale);
}

// ---- Warp ----
// homography is 9 floats in ROW-MAJOR order [h00,h01,h02, h10,h11,h12, h20,h21,h22]
kernel void planarWarpKernel(
    READ_TEX                 srcTex      [[texture(0)]],
    device float*            dstBuf      [[buffer(0)]],
    constant float*          homography  [[buffer(1)]],   // 9 floats, row-major
    constant int3&           whCropZoom  [[buffer(2)]],
    constant float&          autoZoom    [[buffer(3)]],
    constant int&            dstRowBytes [[buffer(4)]],
    uint2 gid [[thread_position_in_grid]])
{
    int x = int(gid.x), y = int(gid.y);
    // C++ passes row-major; Metal float3x3 is column-major.
    // Transpose by constructing columns from source rows.
    float3x3 H = float3x3(homography[0], homography[3], homography[6],   // col 0 = row 0
                           homography[1], homography[4], homography[7],   // col 1 = row 1
                           homography[2], homography[5], homography[8]);  // col 2 = row 2
    warpBody(x, y, srcTex, dstBuf,
             H[0][0], H[0][1], H[0][2],
             H[1][0], H[1][1], H[1][2],
             H[2][0], H[2][1], H[2][2],
             whCropZoom.x, whCropZoom.y,
             whCropZoom.z, autoZoom,
             dstRowBytes);
}
)METAL_SUFFIX";

inline const std::string& getPlanarMetalKernelSource() {
    static const std::string s =
        std::string(kPlanarMetalPrefix) +
        std::string(planarKernelCoreSource) +
        std::string(kPlanarMetalSuffix);
    return s;
}

} // namespace

#endif // PLANAR_KERNEL_SOURCE_H
