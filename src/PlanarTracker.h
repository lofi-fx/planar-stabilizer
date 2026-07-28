#ifndef PLANAR_TRACKER_H
#define PLANAR_TRACKER_H

#include "PlanarParams.h"
#include <vector>
#include <string>

// ============================================================
// PlanarTracker — CPU-side template extraction, result storage,
// and per-frame tracking orchestration.
//
// GPU dispatch is done via MetalKernels.h; the tracker manages
// template data, NCC peak finding, sub-pixel refinement, and
// serialization. The main plugin calls trackFrameGPU() in a
// loop, passing GPU texture handles obtained from MetalKernels.
// ============================================================
class PlanarTracker {
public:
    PlanarTracker();
    ~PlanarTracker();

    // ---- Template (CPU) ----
    // Extract circular template from RGBA float frame data.
    // Returns false if template area is empty/invalid.
    bool extractTemplate(const float* frameData, int w, int h,
                         float centerX, float centerY, float radius,
                         int rowBytes);

    // Access template data for GPU upload
    const float* getTemplateBuffer() const { return mTemplateBuffer; }
    int getTemplateCount() const { return mTemplateCount; }
    const float* getRefStats() const { return mRefStats; }

    // Derived template geometry
    int getTemplateSize() const {
        return mTemplateCount > 0 ? (int)(std::sqrt((float)mTemplateCount) + 0.5f) : 0;
    }
    int getTemplateRadius() const {
        return mTemplateCount > 0 ? getTemplateSize() / 2 : 0;
    }
    int getTemplateHalf() const { int s = getTemplateSize(); return (s - 1) / 2; }

    // ---- Tracking (GPU) ----
    // Track a single frame given a GPU texture handle.
    // frameTexPtr:  void* handle to READ_TEX texture for current frame at full res
    //               (caller must create via metalCreateTextureFromBuffer)
    // accTx, accTy, accRot, accScale: accumulated transform from frame 0
    // gpuResources: GPU access for pyramid generation + search dispatches
    // returns the incremental transform for this frame
    FrameTransform trackFrameGPU(void* frameTexPtr,
                                  int w, int h,
                                  int frameWidth, int frameHeight,
                                  float accTx, float accTy,
                                  float accRot, float accScale);

    // ---- Results ----
    const TrackingData& getTrackingData() const { return mData; }
    TrackingData& getTrackingDataMut() { return mData; }

    // Access computed AutoZoom factor
    float computeAutoZoom() const;

    // ---- Serialization ----
    std::string serializeToJson() const;
    bool deserializeFromJson(const std::string& json);

    bool isTracked() const { return mData.frameCount > 0 && !mData.frames.empty(); }
    int frameCount() const { return mData.frameCount; }

    void clearFrames() {
        mData.frames.clear();
        mData.frameCount = 0;
    }

    void reserveFrames(int n) { mData.frames.reserve(n); }
    void addFrame(const FrameTransform& ft) { mData.frames.push_back(ft); }
    void setFrameCount(int n) { mData.frameCount = n; }

private:
    struct PeakResult { int dx, dy; float ncc; };
    PeakResult findPeak(const float* corr, int searchW);
    float subPixelFitX(const float* corr, int searchW, int cx, int cy);
    float subPixelFitY(const float* corr, int searchW, int cx, int cy);

    TrackingData mData;
    float* mTemplateBuffer = nullptr;
    int    mTemplateCount = 0;
    float  mRefStats[3] = {};
};

#endif // PLANAR_TRACKER_H
