// ============================================================
// Metal warp — standalone GPU acceleration
// Creates its own Metal device/queue, copies CPU data to GPU,
// runs bilinear warp kernel, copies result back.
// No Resolve GPU integration required.
// ============================================================

#import <Metal/Metal.h>
#import <mutex>
#import <unordered_map>

static std::mutex s_PSOMutex;
static std::unordered_map<id<MTLDevice>, id<MTLComputePipelineState>> s_PSOCache;
static id<MTLDevice> s_Device = nil;
static id<MTLCommandQueue> s_Queue = nil;
static id<MTLBuffer> s_SrcBuf = nil;
static id<MTLBuffer> s_DstBuf = nil;
static size_t s_BufSize = 0;

static const char* kernelSrc = R"metal(
#include <metal_stdlib>
using namespace metal;

kernel void planar_warp(device const float* src  [[buffer(0)]],
                        device float*       dst  [[buffer(1)]],
                        constant float4&    p0   [[buffer(2)]], // dx, dy, zoom, cx
                        constant float&     cy   [[buffer(3)]],
                        constant int&       w    [[buffer(4)]],
                        constant int&       h    [[buffer(5)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= (uint)w || gid.y >= (uint)h) return;
    float dx = p0.x, dy = p0.y, zoom = p0.z, cx = p0.w;
    float invZ = 1.0 / zoom;
    float srcX = ((float)gid.x - cx) * invZ + cx + dx;
    float srcY = ((float)gid.y - cy) * invZ + cy + dy;
    if (srcX < 0.0 || srcX >= (float)(w-1) || srcY < 0.0 || srcY >= (float)(h-1)) {
        int oi = ((int)gid.y * w + (int)gid.x) * 4;
        dst[oi] = 0; dst[oi+1] = 0; dst[oi+2] = 0; dst[oi+3] = 1;
        return;
    }
    int x0 = (int)srcX, y0 = (int)srcY;
    int x1 = min(x0 + 1, w - 1), y1 = min(y0 + 1, h - 1);
    float fx = srcX - (float)x0, fy = srcY - (float)y0;
    int i00 = (y0 * w + x0) * 4, i10 = (y0 * w + x1) * 4;
    int i01 = (y1 * w + x0) * 4, i11 = (y1 * w + x1) * 4;
    float4 p00 = float4(src[i00], src[i00+1], src[i00+2], src[i00+3]);
    float4 p10 = float4(src[i10], src[i10+1], src[i10+2], src[i10+3]);
    float4 p01 = float4(src[i01], src[i01+1], src[i01+2], src[i01+3]);
    float4 p11 = float4(src[i11], src[i11+1], src[i11+2], src[i11+3]);
    float4 row0 = mix(p00, p10, fx);
    float4 row1 = mix(p01, p11, fx);
    float4 out = mix(row0, row1, fy);
    int oi = ((int)gid.y * w + (int)gid.x) * 4;
    dst[oi] = out.x; dst[oi+1] = out.y; dst[oi+2] = out.z; dst[oi+3] = out.w;
}
)metal";

extern "C" int TriggerMetalWarpFromCPU(float* dst, const float* src, int w, int h,
                                        float dx, float dy, float zoom,
                                        float centerX, float centerY) {
    @autoreleasepool {
        size_t bufSize = (size_t)w * h * 4 * sizeof(float);
        
        // Lazy-init Metal device and queue
        if (!s_Device) {
            s_Device = MTLCreateSystemDefaultDevice();
            if (!s_Device) return 1;
            s_Queue = [s_Device newCommandQueue];
            if (!s_Queue) return 2;
        }
        
        // Get or create cached PSO
        id<MTLComputePipelineState> pso = nil;
        {
            std::lock_guard<std::mutex> lock(s_PSOMutex);
            auto it = s_PSOCache.find(s_Device);
            if (it != s_PSOCache.end()) pso = it->second;
        }
        if (!pso) {
            NSError* err = nil;
            MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
            if (@available(macOS 15.0, *)) opts.mathMode = MTLMathModeFast;
            id<MTLLibrary> lib = [s_Device newLibraryWithSource:[NSString stringWithUTF8String:kernelSrc]
                                                         options:opts error:&err];
            if (!lib) { NSLog(@"MetalWarp: compile error %@", err); return 3; }
            id<MTLFunction> fn = [lib newFunctionWithName:@"planar_warp"];
            if (!fn) { return 4; }
            pso = [s_Device newComputePipelineStateWithFunction:fn error:&err];
            if (!pso) { return 5; }
            std::lock_guard<std::mutex> lock(s_PSOMutex);
            s_PSOCache[s_Device] = pso;
        }
        
        // Reuse or allocate GPU buffers
        if (!s_SrcBuf || s_BufSize < bufSize) {
            s_SrcBuf = [s_Device newBufferWithLength:bufSize options:MTLResourceStorageModeShared];
            s_DstBuf = [s_Device newBufferWithLength:bufSize options:MTLResourceStorageModeShared];
            s_BufSize = bufSize;
        }
        
        // Copy source data to GPU buffer
        memcpy([s_SrcBuf contents], src, bufSize);
        
        float cx = centerX;
        float cy = centerY;
        float params[4] = {dx, dy, zoom, cx};
        
        id<MTLCommandBuffer> cb = [s_Queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:s_SrcBuf offset:0 atIndex:0];
        [enc setBuffer:s_DstBuf offset:0 atIndex:1];
        [enc setBytes:params length:sizeof(float)*4 atIndex:2];
        [enc setBytes:&cy length:sizeof(float) atIndex:3];
        [enc setBytes:&w length:sizeof(int) atIndex:4];
        [enc setBytes:&h length:sizeof(int) atIndex:5];
        
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake((w+15)/16, (h+15)/16, 1);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        
        // Copy result back to destination
        memcpy(dst, [s_DstBuf contents], bufSize);
        return 0;
    }
}
