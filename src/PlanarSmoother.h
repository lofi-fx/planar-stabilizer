#ifndef PLANAR_SMOOTHER_H
#define PLANAR_SMOOTHER_H

#include "PlanarParams.h"
#include <vector>

// ============================================================
// PlanarSmoother — applies Gaussian low-pass filter to motion
// curves (X, Y, rotation, scale) extracted from tracking data.
//
// smoothness parameter (0..100):
//   0   = no smoothing (raw tracking)
//   50  = moderate smoothing (default)
//   100 = maximum smoothing (very slow motion changes)
//
// Each component curve is filtered independently with a
// 1D Gaussian kernel applied over the frame index axis.
// ============================================================
class PlanarSmoother {
public:
    PlanarSmoother();

    // Extract raw curves from tracking data
    void loadFromTrackingData(const TrackingData& data);

    // Apply Gaussian smoothing with given smoothness (0..100)
    void smooth(float smoothness);

    // Get smoothed transform for a specific frame
    FrameTransform getSmoothedTransform(int frameIndex) const;

    // Get the difference transform (for stabilization):
    // Returns the transform that, when applied to the raw frame,
    // produces the stabilized frame.
    // This is: smoothed - raw (i.e., the correction to apply)
    static FrameTransform computeCorrection(const FrameTransform& raw,
                                             const FrameTransform& smoothed);

    // Build the stabilization homography (3x3 row-major) from
    // the correction transform, respecting component toggles.
    static void buildStabilizationHomography(
        const FrameTransform& correction,
        float* homography3x3,  // out: 9 floats
        int w, int h,
        bool enableX, bool enableY, bool enableZ,
        bool enableRot, bool enableScale);

private:
    std::vector<float> mCurves[4];  // 0=tx, 1=ty, 2=rot, 3=scale
    std::vector<float> mSmoothed[4];
    int mFrameCount = 0;
};

#endif // PLANAR_SMOOTHER_H
