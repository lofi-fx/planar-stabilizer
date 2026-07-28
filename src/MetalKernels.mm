#ifdef __APPLE__

#import <Metal/Metal.h>
#include <string>
#include <cstring>
#include "MetalKernels.h"
#include "PlanarKernelSource.h"

// ============================================================
// Module-level Metal state
// ============================================================
static id<MTLDevice>                s_device   = nil;
static id<MTLCommandQueue>          s_queue    = nil;
static id<MTLLibrary>               s_lib      = nil;
static id<MTLComputePipelineState>  s_psoPyramid = nil;
static id<MTLComputePipelineState>  s_psoTrack   = nil;
static id<MTLComputePipelineState>  s_psoWarp    = nil;

// ============================================================
// Init / Release
// ============================================================
bool metalInit(void* cmdQueuePtr) {
    @autoreleasepool {
        if (s_lib && s_psoPyramid && s_psoTrack && s_psoWarp) return true;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)cmdQueuePtr;
        if (!queue) { NSLog(@"PlanarStab: nil queue"); return false; }
        s_queue = queue;
        s_device = queue.device;
        if (!s_device) { NSLog(@"PlanarStab: nil device"); return false; }

        NSError* err = nil;
        if (!s_lib) {
            MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
            if (@available(macOS 15.0, *)) {
                opts.mathMode = MTLMathModeFast;
            } else {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                opts.fastMathEnabled = YES;
    #pragma clang diagnostic pop
            }
            const std::string& src = getPlanarMetalKernelSource();
            s_lib = [s_device newLibraryWithSource:
                       [NSString stringWithUTF8String:src.c_str()]
                     options:opts error:&err];
            if (!s_lib) {
                NSLog(@"PlanarStab: shader compile failed: %@", err);
                return false;
            }
            NSLog(@"PlanarStab: Metal library compiled successfully");
        }

        auto makePso = [&](NSString* name, id<MTLComputePipelineState>* out) {
            if (*out) return;
            id<MTLFunction> fn = [s_lib newFunctionWithName:name];
            if (!fn) { NSLog(@"PlanarStab: missing kernel %@", name); return; }
            NSError* e = nil;
            *out = [s_device newComputePipelineStateWithFunction:fn error:&e];
            if (!*out) NSLog(@"PlanarStab: pipeline %@ failed: %@", name, e);
        };
        makePso(@"planarPyramidKernel", &s_psoPyramid);
        makePso(@"planarTrackKernel",   &s_psoTrack);
        makePso(@"planarWarpKernel",    &s_psoWarp);

        return s_psoPyramid && s_psoTrack && s_psoWarp;
    }
}

void metalRelease() {
    @autoreleasepool {
        s_psoPyramid = nil;
        s_psoTrack   = nil;
        s_psoWarp    = nil;
        s_lib        = nil;
        s_queue      = nil;
        s_device     = nil;
    }
}

// ============================================================
// Template upload — template as float4 array, retained MTLBuffer
// Caller owns the returned handle and must metalFreeTemplate() it.
// ============================================================
void* metalUploadTemplate(const float* data, int count, const float* stats3) {
    @autoreleasepool {
        if (!data || count <= 0 || !s_device) return nullptr;
        size_t bytes = (size_t)count * 4 * sizeof(float);
        id<MTLBuffer> buf = [s_device newBufferWithBytes:data length:bytes
                                       options:MTLResourceStorageModeShared];
        // Return with +1 retain (newBuffer already +1, CFBridgingRetain makes it +2,
        // but ARC releases the local id<MTLBuffer> at end of scope = -1, net +1)
        return (void*)CFBridgingRetain(buf);
    }
}

void metalFreeTemplate(void* handle) {
    // Release the retained MTLBuffer
    if (handle) CFBridgingRelease(handle);
}

// ============================================================
// Create a sampler-ready texture from a Resolve-provided buffer.
// Returns a retained id<MTLTexture> that caller must release.
// ============================================================
static id<MTLTexture> createTextureFromBuffer(id<MTLBuffer> buf, int w, int h, int rowBytes) {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;  // matches Resolve's buffer
    id<MTLTexture> tex = [buf newTextureWithDescriptor:desc
                                                offset:0
                                           bytesPerRow:(NSUInteger)rowBytes];
    return tex;  // +1 retain
}

// Create a private (GPU-only) writeable texture for pyramid intermediates.
static id<MTLTexture> createPrivateTexture(int w, int h) {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [s_device newTextureWithDescriptor:desc];
    return tex;  // +1 retain
}

// ============================================================
// Pyramid generation: creates a half-res texture from source.
// Returns retained texture via outTexPtr (caller must release).
// ============================================================
void metalDispatchPyramid(void* srcTexPtr, int srcW, int srcH,
                          void** outTexPtr, int* outW, int* outH) {
    if (!s_psoPyramid || !srcTexPtr) { *outTexPtr = nullptr; *outW = 0; *outH = 0; return; }
    @autoreleasepool {
        id<MTLTexture> srcTex = (__bridge id<MTLTexture>)srcTexPtr;
        int dw = srcW / 2;
        int dh = srcH / 2;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        id<MTLTexture> dstTex = createPrivateTexture(dw, dh);  // +1
        if (!dstTex) { *outTexPtr = nullptr; *outW = 0; *outH = 0; return; }

        id<MTLCommandBuffer> cmd = [s_queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:s_psoPyramid];
        [enc setTexture:srcTex atIndex:0];
        [enc setTexture:dstTex atIndex:1];
        int dims[2] = { dw, dh };
        [enc setBytes:&dims length:sizeof(dims) atIndex:0];

        MTLSize grid = MTLSizeMake((NSUInteger)dw, (NSUInteger)dh, 1);
        NSUInteger tw = s_psoPyramid.threadExecutionWidth;
        NSUInteger th = s_psoPyramid.maxTotalThreadsPerThreadgroup / tw;
        [enc dispatchThreads:grid threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        // Transfer ownership to caller (+1 retain via CFBridgingRetain,
        // and ARC releases the local dstTex at scope end = net +1)
        *outTexPtr = (void*)CFBridgingRetain(dstTex);
        *outW = dw;
        *outH = dh;
    }
}

// ============================================================
// NCC tracking search
// ============================================================
void metalDispatchSearch(void* frameTexPtr,
                         void* templBufPtr,
                         const float* refStats,
                         float* correlationBuffer,
                         int ts, int half,
                         int cx, int cy,
                         int fw, int fh,
                         int searchR,
                         float cosA, float sinA,
                         float invScale) {
    if (!s_psoTrack) return;
    @autoreleasepool {
        id<MTLTexture> frameTex = (__bridge id<MTLTexture>)frameTexPtr;
        id<MTLBuffer> templBuf = (__bridge id<MTLBuffer>)templBufPtr;

        int searchW = 2 * searchR + 1;
        size_t corrBytes = (size_t)searchW * searchW * sizeof(float);

        // Shared buffer for CPU readback
        id<MTLBuffer> corrBuf = [s_device newBufferWithBytes:correlationBuffer
                                                       length:corrBytes
                                                      options:MTLResourceStorageModeShared];

        id<MTLCommandBuffer> cmd = [s_queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:s_psoTrack];
        [enc setTexture:frameTex atIndex:0];
        [enc setBuffer:templBuf offset:0 atIndex:0];
        [enc setBytes:refStats length:3*sizeof(float) atIndex:1];
        [enc setBuffer:corrBuf offset:0 atIndex:2];

        int tsHalf[2] = { ts, half };
        int center[2] = { cx, cy };
        int frameDims[2] = { fw, fh };
        [enc setBytes:&tsHalf length:sizeof(tsHalf) atIndex:3];
        [enc setBytes:&center length:sizeof(center) atIndex:4];
        [enc setBytes:&frameDims length:sizeof(frameDims) atIndex:5];
        [enc setBytes:&searchR length:sizeof(searchR) atIndex:6];
        float rotSc[2] = { cosA, sinA };
        [enc setBytes:&rotSc length:sizeof(rotSc) atIndex:7];
        [enc setBytes:&invScale length:sizeof(invScale) atIndex:8];

        MTLSize grid = MTLSizeMake((NSUInteger)searchW, (NSUInteger)searchW, 1);
        NSUInteger tw = s_psoTrack.threadExecutionWidth;
        NSUInteger th = s_psoTrack.maxTotalThreadsPerThreadgroup / tw;
        [enc dispatchThreads:grid threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        // Copy results back to caller
        memcpy(correlationBuffer, corrBuf.contents, corrBytes);
    }
}

// ============================================================
// Warp
// ============================================================
void metalDispatchWarp(void* srcBufPtr, int srcRowBytes,
                       int srcW, int srcH,
                       void* dstBufPtr, int dstRowBytes,
                       int dstW, int dstH,
                       const float* homography3x3,
                       int cropMode,
                       float autoZoom) {
    if (!s_psoWarp || !srcBufPtr || !dstBufPtr) return;
    @autoreleasepool {
        id<MTLBuffer> srcBuf = (__bridge id<MTLBuffer>)srcBufPtr;
        id<MTLTexture> srcTex = createTextureFromBuffer(srcBuf, srcW, srcH, srcRowBytes);  // +1
        if (!srcTex) return;

        id<MTLBuffer> dstBuf = (__bridge id<MTLBuffer>)dstBufPtr;

        id<MTLCommandBuffer> cmd = [s_queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:s_psoWarp];
        [enc setTexture:srcTex atIndex:0];
        [enc setBuffer:dstBuf offset:0 atIndex:0];
        // 9 floats in row-major order
        [enc setBytes:homography3x3 length:9*sizeof(float) atIndex:1];
        int whCrop[3] = { dstW, dstH, cropMode };
        [enc setBytes:&whCrop length:sizeof(whCrop) atIndex:2];
        [enc setBytes:&autoZoom length:sizeof(autoZoom) atIndex:3];
        [enc setBytes:&dstRowBytes length:sizeof(dstRowBytes) atIndex:4];

        MTLSize grid = MTLSizeMake((NSUInteger)dstW, (NSUInteger)dstH, 1);
        NSUInteger tw = s_psoWarp.threadExecutionWidth;
        NSUInteger th = s_psoWarp.maxTotalThreadsPerThreadgroup / tw;
        [enc dispatchThreads:grid threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
    }
}

// ============================================================
// Destroy textures created by helpers above
// ============================================================
void metalDestroyTexture(void* texPtr) {
    if (texPtr) CFBridgingRelease(texPtr);
}

#endif // __APPLE__
