# LoFi FX Planar Stabilizer — Project Status

## What This Plugin Does

A DaVinci Resolve OFX plugin for planar tracking stabilization. The user selects a feature in the video, the plugin tracks that feature across frames, and stabilizes the shot so the feature stays locked in place.

## Current State (July 2026)

### What Works

- **Plugin loads in Resolve** without crashing (took many iterations to fix: missing param defaults, Metal framework linkage issues, architecture mismatch, stale code signatures)
- **Green circle overlay** drawn on the output image at the tracking region position. Circle reads live slider values and updates in real-time.
- **Param sliders**: Center X, Center Y, Radius, Show Overlay
- **Metal passthrough warp** (copies source to destination via GPU, no transform yet)
- **Template extraction**: can extract a circular template from a reference frame

### What's Partially Working

- **NCC tracker**: uses normalized cross-correlation (via Accelerate vDSP) to match a template against each frame. The tracker runs during render and updates the tracked position. The circle is supposed to move to follow the feature.
- **Tracking confidence**: NCC values are often low (<0.2), meaning the template doesn't match well. The single-template approach is fragile — it fails on lighting changes, rotation, scale changes, and low-texture regions.

### What We've Tried (and Why It Failed)

| Approach | Problem |
|----------|---------|
| Single NCC template matching | Not robust — fails on real footage with lighting/motion changes |
| Auto-zoom to hide borders | Zoom factor computed from max displacement blew up from outlier tracking data |
| OFX Interact API (V1 + V2) for clickable overlays | Resolve doesn't call the interact handlers for Filter context plugins |
| OfxDrawSuiteV1 for overlay drawing | Same issue — Resolve doesn't fire the draw action |
| OpenGL overlay drawing | Deprecated on macOS + Resolve doesn't support it for OFX plugins |
| Metal warp via buffer wrapping | Crashed because Resolve's data pointers aren't MTLBuffer objects |
| Metal warp via Resolve texture pointers | Works (identity passthrough) but stabilization logic not yet correct |
| Concurrent frame tracking | Better than reference tracking but NCC still too fragile |

### Architecture

```
PlanarStabilizerPlugin.cpp  — main OFX plugin (all-in-one file, ~450 lines)
MetalWarp.mm               — Metal compute kernel for GPU warp
CMakeLists.txt             — Build system, fetches OpenFX SDK 1.5.1
Info.plist                 — Bundle metadata
build_and_install.py       — Build + install script
```

Dependencies: Metal.framework, Accelerate.framework, CoreFoundation.framework

### Key Lessons Learned

1. **Resolve doesn't call OFX Interact handlers** — no clickable overlays in the viewer. All interaction must be through parameter sliders.
2. **Metal textures must come from Resolve** — use `kOfxImageEffectPropMetalTexturePointer` property, don't create textures from raw data pointers.
3. **String params must have defaults** — missing `kOfxParamPropDefault` on string params causes Resolve to crash during instance creation.
4. **Don't read param values during createInstance** — params aren't initialized yet. Read them lazily during first render.
5. **Universal binary is essential** — `CMAKE_OSX_ARCHITECTURES` must be `arm64;x86_64`. Set it on the cmake command line, not just in CMakeLists.txt.
6. **Clear build directory between attempts** — stale `_CodeSignature` directories from previous code-signing cause mysterious crashes.
7. **Single NCC template is not enough** — real footage needs multi-feature tracking with RANSAC.

## What We're Trying to Build

### Core Goal

A planar tracking stabilizer that:
1. User draws/positions a circle on a feature
2. Plugin tracks that feature across all frames
3. Plugin warps each frame so the feature stays at its original position (lock mode)
4. User can toggle X/Y/Rotation/Scale components independently
5. Auto-crop hides black borders from the warp

### Planned Tracking Approach

The NCC single-template approach needs to be replaced with **multi-feature tracking** (like vid.stab):

1. **Detect multiple feature points** within the circular region (Shi-Tomasi corners — use smallest eigenvalue of structure tensor)
2. **Extract small patches** (15×15) around each feature point
3. **Track each patch** to the next frame using NCC or SSD within a small search window (±5px)
4. **Estimate dominant motion** using histogram voting or RANSAC on the tracked point displacements
5. **Accumulate** per-frame motions to get the camera path
6. **Smooth** the path with a Gaussian/butterworth filter
7. **Warp** each frame: `correction = smoothed_path - raw_path`

### Stabilization Pipeline

1. CPU: Feature detection + tracking (consecutive frames, not reference frame)
2. CPU: Path accumulation + smoothing
3. GPU: Warp via Metal compute shader (using Resolve's texture pointers)
4. GPU: Auto-crop zoom to fill black borders

### Interactive Overlay

Since Resolve doesn't support OFX Interact API:
- Overlay drawn on the output image (already works)
- User positions the circle via sliders (Center X, Center Y, Radius)
- During tracking, the circle follows the feature (proving tracking works)
- "Show Overlay" toggle to hide for clean output

### Reference Implementations to Study

- **vid.stab** (https://github.com/georgmartius/vid.stab) — the gold standard for open-source video stabilization. Uses multi-measurement-field with contrast selection, RANSAC motion estimation, Gaussian path smoothing.
- **FFmpeg deshake filter** — similar approach to vid.stab, single-pass.
- **Blender's tracking** — uses KLT (Kanade-Lucas-Tomasi) feature tracker with pyramid optical flow.
- **OpenCV videostab** — comprehensive stabilization module with multiple motion models.

### Future Considerations

- **Multi-resolution pyramid** for tracking: coarse-to-fine search enables larger motions
- **Kalman filter** for real-time smoothing (vs. Gaussian which needs future frames)
- **Rolling shutter correction** — estimate and correct rolling shutter from inter-frame motion
- **GPU-accelerated tracking** — the NCC computations could run on GPU for speed
- **Perspective warp** — 4-point homography instead of just translation
- **Gyroscope data** — use camera IMU data for stabilization when available (like GYROflow)

## File Structure (as of July 2026)

```
lofi-fx-planar-stabilizer/
├── CMakeLists.txt
├── Info.plist
├── build_and_install.py
├── PROJECT_STATUS.md
├── src/
│   ├── PlanarStabilizerPlugin.cpp    — Main plugin (all tracking + render + overlay)
│   └── MetalWarp.mm                  — Metal warp kernel wrapper
├── build/                            — Build output
│   └── LofiFxPlanarStabilizer.ofx.bundle/
└── (old files kept but not compiled)
    ├── PlanarParams.h
    ├── PlanarTracker.h/.cpp
    ├── PlanarSmoother.h/.cpp
    ├── MetalKernels.h/.mm
    ├── PlanarKernelCore.inc
    ├── PlanarKernelSource.h
    ├── PlanarKernelCoreText.h.in
    └── CudaKernels.h/.cpp
```

## Current Parameter Set

| Param | Type | Default | Range |
|-------|------|---------|-------|
| center_x | Double | 0.5 | 0.0–1.0 |
| center_y | Double | 0.5 | 0.0–1.0 |
| track_r | Double | 0.02 | 0.001–0.5 |
| show_overlay | Boolean | true | — |
| _status | String (secret) | "Ready" | — |
| track_btn | PushButton | — | — |

## Build & Install

```bash
cd lofi-fx-planar-stabilizer
python3 build_and_install.py
```

Requires: CMake 3.20+, macOS 11.0+, Xcode CommandLineTools
Installs to: `/Library/OFX/Plugins/LofiFxPlanarStabilizer.ofx.bundle`
