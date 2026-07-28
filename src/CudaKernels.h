#ifndef PLANAR_CUDA_KERNELS_H
#define PLANAR_CUDA_KERNELS_H

// Stub CUDA dispatch — macOS/Metal only for v1.
// Linux/Windows builds compile but render via CPU fallback.

void cudaDispatchWarp(void* streamPtr, void* srcBufPtr, void* dstBufPtr,
                      const float* homography, int w, int h,
                      int cropMode, float autoZoom,
                      int srcRowBytes, int dstRowBytes);

#endif
