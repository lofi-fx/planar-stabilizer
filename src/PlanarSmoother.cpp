#include "PlanarSmoother.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// ============================================================
PlanarSmoother::PlanarSmoother() {}

// ============================================================
// Load raw curves from tracking data
// ============================================================
void PlanarSmoother::loadFromTrackingData(const TrackingData& data) {
    mFrameCount = (int)data.frames.size();
    for (int c = 0; c < 4; c++) {
        mCurves[c].resize(mFrameCount);
        mSmoothed[c].resize(mFrameCount);
    }

    for (int i = 0; i < mFrameCount; i++) {
        const auto& f = data.frames[i];
        mCurves[0][i] = f.tx;
        mCurves[1][i] = f.ty;
        mCurves[2][i] = f.rotation;
        mCurves[3][i] = f.scale;
        mSmoothed[0][i] = f.tx;
        mSmoothed[1][i] = f.ty;
        mSmoothed[2][i] = f.rotation;
        mSmoothed[3][i] = f.scale;
    }
}

// ============================================================
// Apply Gaussian smoothing
// smoothness: 0 = none, 100 = maximum
// ============================================================
void PlanarSmoother::smooth(float smoothness) {
    if (mFrameCount < 3) return;

    // Map smoothness to Gaussian sigma (kernel size)
    // smoothness 0 → sigma 0.5 (almost no smoothing)
    // smoothness 50 → sigma 5.0
    // smoothness 100 → sigma 30.0
    float sigma = 0.5f + smoothness * 0.295f;  // 0→0.5, 100→30
    int kernelRadius = (int)(sigma * 3.0f);  // 99.7% coverage
    if (kernelRadius < 1) kernelRadius = 1;

    // Build Gaussian kernel
    int kernelSize = 2 * kernelRadius + 1;
    std::vector<float> kernel(kernelSize);
    float sumK = 0;
    for (int i = -kernelRadius; i <= kernelRadius; i++) {
        float v = std::exp(-0.5f * (float)(i * i) / (sigma * sigma));
        kernel[i + kernelRadius] = v;
        sumK += v;
    }
    for (int i = 0; i < kernelSize; i++) kernel[i] /= sumK;

    // Apply 1D convolution to each curve
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < mFrameCount; i++) {
            double sum = 0;
            double wSum = 0;
            for (int k = -kernelRadius; k <= kernelRadius; k++) {
                int idx = i + k;
                if (idx < 0 || idx >= mFrameCount) continue;
                double w = kernel[k + kernelRadius];
                // For rotation (curve 2), handle angular wrap
                double val = mCurves[c][idx];
                if (c == 2) {
                    // Unwrap to prevent 360° wrapping
                    double ref = mSmoothed[c][i];
                    if (idx > i) ref = mCurves[c][idx - 1];
                    double diff = val - ref;
                    if (diff > 3.14159) val -= 2.0 * 3.14159;
                    else if (diff < -3.14159) val += 2.0 * 3.14159;
                }
                sum += w * val;
                wSum += w;
            }
            mSmoothed[c][i] = (float)(sum / wSum);
        }
    }

    // Normalize rotation back to [-π, π]
    for (int i = 0; i < mFrameCount; i++) {
        while (mSmoothed[2][i] > 3.14159f) mSmoothed[2][i] -= 2.0f * 3.14159f;
        while (mSmoothed[2][i] < -3.14159f) mSmoothed[2][i] += 2.0f * 3.14159f;
    }
}

// ============================================================
// Get smoothed transform for a frame
// ============================================================
FrameTransform PlanarSmoother::getSmoothedTransform(int frameIndex) const {
    FrameTransform ft = {0, 0, 0, 1.0f, 0};
    if (frameIndex >= 0 && frameIndex < mFrameCount) {
        ft.tx = mSmoothed[0][frameIndex];
        ft.ty = mSmoothed[1][frameIndex];
        ft.rotation = mSmoothed[2][frameIndex];
        ft.scale = mSmoothed[3][frameIndex];
    }
    return ft;
}

// ============================================================
// Compute correction transform
// correction = smoothed - raw
// For stabilization: we want to warp so raw becomes smoothed.
// The pixel shift is: -(raw - smoothed) = smoothed - raw
// ============================================================
FrameTransform PlanarSmoother::computeCorrection(const FrameTransform& raw,
                                                  const FrameTransform& smoothed) {
    FrameTransform corr;
    corr.tx = smoothed.tx - raw.tx;
    corr.ty = smoothed.ty - raw.ty;
    corr.rotation = smoothed.rotation - raw.rotation;
    corr.scale = smoothed.scale / std::max(raw.scale, 0.01f);
    corr.confidence = raw.confidence;
    return corr;
}

// ============================================================
// Build stabilization homography matrix from correction transform.
// The homography maps output (stabilized) pixels → source (input) pixels.
//
// H = T(-tx, -ty) * R(-rot) * S(1/scale)
//
// Applied as:
//   [x_src]   [H0 H1 H2] [x_dst]
//   [y_src] = [H3 H4 H5] [y_dst]
//   [1    ]   [H6 H7 H8] [1    ]
// ============================================================
void PlanarSmoother::buildStabilizationHomography(
    const FrameTransform& correction,
    float* H,  // out: 9 floats, row-major
    int w, int h,
    bool enableX, bool enableY, bool enableZ,
    bool enableRot, bool enableScale)
{
    // Start with identity
    memset(H, 0, 9 * sizeof(float));
    H[0] = H[4] = H[8] = 1.0f;

    float tx = enableX ? correction.tx : 0.0f;
    float ty = enableY ? correction.ty : 0.0f;
    float rot = enableRot ? correction.rotation : 0.0f;
    float scaleCorr = 1.0f;

    // Handle Z (uniform scale) and Scale toggles
    if (enableZ && enableScale)
        scaleCorr = correction.scale;
    else if (enableZ)
        scaleCorr = correction.scale;
    else if (enableScale)
        scaleCorr = correction.scale;
    else
        scaleCorr = 1.0f;

    float s = 1.0f / std::max(scaleCorr, 0.01f);
    float c = cosf(-rot);  // inverse rotation
    float sR = sinf(-rot);

    // Center of rotation/scale
    float cx = (float)w * 0.5f;
    float cy = (float)h * 0.5f;

    // Build homography: translate to origin, rotate/scale, translate back, then translate
    // H = T(cx,cy) * S(s) * R(rot) * T(-cx,-cy) * T(-tx,-ty)
    //
    // Actually for stabilization the warp maps output→input.
    // We want: for each output pixel, find the source pixel.
    //
    // source = inverse(correction) applied to dest
    // correction = raw→smoothed (for the tracked region)
    // inverse correction = smoothed→raw
    // = translate(-tx, -ty) * rotate(-rot) * scale(1/s) around center, then translate back
    //
    // Full matrix:
    // H = [s*c   -s*sR   cx - s*c*cx + s*sR*cy - tx]
    //     [s*sR   s*c    cy - s*sR*cx - s*c*cy - ty]
    //     [0      0                              1]

    H[0] = s * c;
    H[1] = -s * sR;
    H[2] = cx - s * c * cx + s * sR * cy - tx;

    H[3] = s * sR;
    H[4] = s * c;
    H[5] = cy - s * sR * cx - s * c * cy - ty;

    H[6] = 0.0f;
    H[7] = 0.0f;
    H[8] = 1.0f;
}
