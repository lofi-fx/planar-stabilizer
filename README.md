# Planar Stabilizer

A planar tracking stabilizer plugin for DaVinci Resolve (OFX). Locks a tracked feature in place across frames using GPU-accelerated warp — designed for locked-off or parallax-free movement when a tripod isn't an option. This plugin works inside the color page, allowing you to achieve planar stabilization without the hassle of Fusion comps.

Position a circular tracking region on a feature, hit start, and play through the clip. The plugin tracks the feature frame-by-frame using Normalized Cross-Correlation and warps the output to hold it steady. Track data persists with the clip so re-opens and re-renders stay consistent.

## Features

- **Single-feature tracking** — position a circular tracking region, NCC matching against the reference frame with multi-pass search for sub-pixel accuracy
- **Translation and rotation stabilization** — stabilize X/Y position and optionally rotation, with adjustable stabilization amount
- **Auto zoom** — automatically computes the minimum crop zoom to hide stabilized edges
- **On-screen overlay** — green tracking-region circle and crosshair rendered live on the output image

## Download

[Download v0.9 beta (macOS)](https://github.com/lofi-fx/planar-stabilizer/releases/tag/v0.9-beta)

## Installing the plugin

The plugin is unsigned, so macOS will block it the normal way. To install:

1. **Extract the zip** — double-click the downloaded file. You'll see `LofiFxPlanarStabilizer.ofx.bundle`.
2. **Move it into place** — in Finder, press `Cmd+Shift+G`, type `/Library/OFX/Plugins/`, and hit Go. Drag `LofiFxPlanarStabilizer.ofx.bundle` into that folder.
3. **Bypass Gatekeeper** — open **System Settings > Privacy & Security**. Scroll down. Next to *"LofiFxPlanarStabilizer was blocked from loading"* click **Allow Anyway**.
4. If you don't see the message, open **Terminal** (from Applications/Utilities), paste this, and press Enter:
   ```
   sudo xattr -rd com.apple.quarantine /Library/OFX/Plugins/LofiFxPlanarStabilizer.ofx.bundle
   ```
   You'll be asked for your password. Type it (nothing will show as you type) and press Enter.
5. Restart DaVinci Resolve. The plugin appears under **LoFi FX Planar Stabilizer** in the OFX effects list.

## Building from source
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

Requires: CMake 3.20+, macOS 11+ (Metal GPU), Xcode Command Line Tools. Internet connection on first build (fetches OpenFX SDK headers).

Output: `build/LofiFxPlanarStabilizer.ofx.bundle`

## Install from source

```
sudo cmake --build build --target install_local
```

Or copy `build/LofiFxPlanarStabilizer.ofx.bundle` to `/Library/OFX/Plugins/`. Restart Resolve — the plugin appears under **LoFi FX Planar Stabilizer** in the OFX effects list.

## Status

Active development — see [PROJECT_STATUS.md](PROJECT_STATUS.md) for current capabilities and roadmap.

## License

AGPL-3.0-or-later.
