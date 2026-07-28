#ifndef PLANAR_METAL_KERNELS_H
#define PLANAR_METAL_KERNELS_H

#include <cstdint>
#include <vector>

#ifdef __APPLE__

// ---- Lifecycle ----
bool metalInit(void* cmdQueuePtr);
void metalRelease();

// ---- Template management ----
// Upload template as float4 array [lum, 0, 0, mask] with stats.
// Returns handle (MTLBuffer* as void*).
void* metalUploadTemplate(const float* data, int count, const float* stats3);

void metalFreeTemplate(void* handle);

// ---- Pyramid generation ----
// Create a downscaled (half-res) WRITE_TEX texture from a READ_TEX source texture.
// On return, outTexId is set to a retained texture id (caller must release).
void metalDispatchPyramid(void* srcTexPtr, int srcW, int srcH,
                          void** outTexPtr, int* outW, int* outH);

// ---- NCC tracking search ----
// Run trackSearchKernel: dispatches [searchW, searchH, 1] threads.
// Each thread computes NCC at one (dx,dy) offset.
// correlationBuffer must be searchW*searchH floats.
void metalDispatchSearch(void* frameTexPtr,
                         void* templBufPtr,
                         const float* refStats,
                         float* correlationBuffer,
                         int ts, int half,
                         int cx, int cy,
                         int fw, int fh,
                         int searchR,
                         float cosA, float sinA,
                         float invScale);

// ---- Warp ----
// Apply homography warp to source, writing to dst buffer.
void metalDispatchWarp(void* srcBufPtr, int srcRowBytes,
                       int srcW, int srcH,
                       void* dstBufPtr, int dstRowBytes,
                       int dstW, int dstH,
                       const float* homography3x3,
                       int cropMode,
                       float autoZoom);

// ---- Texture creation helpers ----
// Create a texture wrapping an MTLBuffer (shared storage) for use as READ_TEX.
void* metalCreateTextureFromBuffer(void* bufPtr, int w, int h, int rowBytes);

// Create a private (GPU-only) R32Float texture for pyramid level.
void* metalCreatePrivateTexture(int w, int h, int usageReadWrite);

// Destroy a texture created by the above.
void metalDestroyTexture(void* texPtr);

#endif // __APPLE__
#endif // PLANAR_METAL_KERNELS_H
