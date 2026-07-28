// ============================================================
// LoFi FX Planar Stabilizer — shared GPU kernel source
//
// Three kernels:
//   1. pyramidGenKernel  — 2x downscale box filter
//   2. trackSearchKernel — NCC template match at given offset
//   3. warpKernel        — homography perspective warp
// ============================================================

// ============================================================
// Grayscale luminance (Rec.601)
// ============================================================
DEVFN float lum(float3 c) {
    return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}

// ============================================================
// Sample a READ_TEX at pixel (x,y) with clamp-to-edge, return luma
// ============================================================
DEVFN float sampleLum(READ_TEX tex, float x, float y, int w, int h) {
    float u = clamp((x + 0.5f) / max((float)w, 1.0f), 0.0f, 1.0f);
    float v = clamp((y + 0.5f) / max((float)h, 1.0f), 0.0f, 1.0f);
    return lum(TEX_SAMPLE(tex, u, v).rgb);
}

// ============================================================
// KERNEL: pyramidGenKernel
// 2x downscale via separable 4-tap box filter.
// One thread per output pixel.
// Input: READ_TEX (source, can sample)
// Output: WRITE_TEX (destination, at half resolution)
// ============================================================
DEVFN void pyramidGenBody(
    int x, int y,
    READ_TEX  srcTex,
    WRITE_TEX dstTex,
    int sw, int sh)
{
    if (x >= sw || y >= sh) return;  // sw,sh are already half-res
    float sx = (float)x * 2.0f;
    float sy = (float)y * 2.0f;
    int fw = sw * 2;  // full width
    int fh = sh * 2;
    float l = 0.0f;
    l += sampleLum(srcTex, sx,       sy,       fw, fh);
    l += sampleLum(srcTex, sx + 1.0f, sy,       fw, fh);
    l += sampleLum(srcTex, sx,       sy + 1.0f, fw, fh);
    l += sampleLum(srcTex, sx + 1.0f, sy + 1.0f, fw, fh);
    TEX_WRITE(dstTex, x, y, MAKE_FLOAT4(l * 0.25f, 0, 0, 0));
}

// ============================================================
// KERNEL: trackSearchKernel
// One thread per (dx,dy) offset in a 2D search window.
// Computes Pearson NCC between template buffer and frame region.
//
// templateBuf: GLOBAL_RO(float4) — [luminance,0,0,mask] per pixel
// refStats:    GLOBAL_RO(float)  — [sumT, sumT2, countT]
// corrOut:     GLOBAL_RW(float)  — searchW*searchH output values
//
// Template layout: float4[ts*ts], row-major, pixel (tx,ty) at [ty*ts+tx]
// ============================================================
DEVFN void trackSearchBody(
    int gx, int gy,
    READ_TEX        frameTex,
    GLOBAL_RO(float4) templBuf,   // template pixel data
    GLOBAL_RO(float)  refStats,    // [sumT, sumT2, countT]
    GLOBAL_RW(float)  corrOut,     // output correlation buffer
    int ts, int half,              // template size + half-size
    int cx, int cy,                // search center in frame coords
    int fw, int fh,                // frame dimensions
    int searchR,                   // search radius
    float cosA, float sinA,        // rotation
    float invScale)                // inverse scale
{
    int searchW = 2 * searchR + 1;
    if (gx < 0 || gx >= searchW || gy < 0 || gy >= searchW) return;

    int dx = gx - searchR;
    int dy = gy - searchR;

    float sumT  = refStats[0];
    float sumT2 = refStats[1];
    float cntT  = refStats[2];

    float sumI = 0.0f, sumI2 = 0.0f, sumIT = 0.0f;
    float cnt = 0.0f;
    float tcx = (float)(ts - 1) * 0.5f;
    float tcy = tcx;

    for (int ty = 0; ty < ts; ty++) {
        for (int tx = 0; tx < ts; tx++) {
            // Rotated + scaled template coordinate
            float rtx = ((float)tx - tcx) * cosA * invScale -
                        ((float)ty - tcy) * sinA * invScale + tcx;
            float rty = ((float)tx - tcx) * sinA * invScale +
                        ((float)ty - tcy) * cosA * invScale + tcy;

            if (rtx < -0.5f || rtx >= (float)ts + 0.5f ||
                rty < -0.5f || rty >= (float)ts + 0.5f) continue;

            int itx = (int)(rtx + 0.5f);
            int ity = (int)(rty + 0.5f);
            if (itx < 0 || itx >= ts || ity < 0 || ity >= ts) continue;

            float4 t = templBuf[ity * ts + itx];
            float mask = t.w;
            if (mask < 0.5f) continue;                      // outside circle

            // Frame pixel at (dx,dy) offset
            int fx = cx + tx - half + dx;
            int fy = cy + ty - half + dy;
            if (fx < 0 || fx >= fw || fy < 0 || fy >= fh) continue;

            float fl = sampleLum(frameTex, (float)fx, (float)fy, fw, fh);
            float tl = t.x;

            sumI  += fl;
            sumI2 += fl * fl;
            sumIT += fl * tl;
            cnt   += 1.0f;
        }
    }

    // Pearson correlation coefficient
    float ncc = 0.0f;
    if (cnt > 2.0f && cntT > 2.0f) {
        float s = cnt / max(cntT, 1e-10f);
        float st  = sumT * s;
        float st2 = sumT2 * s;

        float num = cnt * sumIT - st * sumI;
        float den = sqrt(max(0.0f, (cnt * st2 - st * st) * (cnt * sumI2 - sumI * sumI)));
        if (den > 1e-10f) ncc = clamp(num / den, -1.0f, 1.0f);
    }

    corrOut[gy * searchW + gx] = ncc;
}

// ============================================================
// KERNEL: warpKernel
// Homography perspective warp with auto-zoom.
// One thread per output pixel.
// ============================================================
DEVFN void warpBody(
    int x, int y,
    READ_TEX        srcTex,
    GLOBAL_RW(float) dstBuf,
    float h0, float h1, float h2,
    float h3, float h4, float h5,
    float h6, float h7, float h8,
    int w, int h,
    int cropMode, float autoZoom,
    int dstRowBytes)
{
    if (x >= w || y >= h) return;

    // Inverse homography: output pixel → source coordinate
    float sx = h0 * (float)x + h1 * (float)y + h2;
    float sy = h3 * (float)x + h4 * (float)y + h5;
    float sw = h6 * (float)x + h7 * (float)y + h8;
    float invW = 1.0f / max(sw, 1e-15f);
    sx *= invW;
    sy *= invW;

    // Auto-zoom: expand to fill frame, removing black borders
    if (cropMode == 1) {
        float cx = (float)w * 0.5f;
        float cy = (float)h * 0.5f;
        sx = (sx - cx) * autoZoom + cx;
        sy = (sy - cy) * autoZoom + cy;
    }

    // Bounds check
    if (sx < -0.5f || sx >= (float)w + 0.5f ||
        sy < -0.5f || sy >= (float)h + 0.5f) {
        int off = y * (dstRowBytes / (int)sizeof(float)) + x * 4;
        dstBuf[off + 0] = 0.0f;
        dstBuf[off + 1] = 0.0f;
        dstBuf[off + 2] = 0.0f;
        dstBuf[off + 3] = 0.0f;
        return;
    }

    float4 c = TEX_SAMPLE(srcTex,
                          (sx + 0.5f) / (float)w,
                          (sy + 0.5f) / (float)h);
    int off = y * (dstRowBytes / (int)sizeof(float)) + x * 4;
    dstBuf[off + 0] = c.r;
    dstBuf[off + 1] = c.g;
    dstBuf[off + 2] = c.b;
    dstBuf[off + 3] = c.a;
}
