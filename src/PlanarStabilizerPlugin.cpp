// LoFi FX Planar Stabilizer — tracking overlay demo
// Stores per-frame tracked positions for stable replay
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#include <CoreFoundation/CoreFoundation.h>
extern "C" int TriggerMetalWarpFromCPU(float* dst, const float* src, int w, int h,
                                        float dx, float dy, float zoom,
                                        float centerX, float centerY);
#endif

#define PLUGIN_ID   "com.lofifx.planarstabilizer"
#define PLUGIN_LABEL "LoFi FX Planar Stabilizer"
#define PLUGIN_GROUP "LoFi FX"
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 0

static const OfxPropertySuiteV1*    gProp   = nullptr;
static const OfxImageEffectSuiteV1* gEffect = nullptr;
static const OfxParameterSuiteV1*   gParam  = nullptr;
static OfxHost* gHost = nullptr;

struct InstanceData {
    float* templ = nullptr;         // working template
    float* stableTempl = nullptr;   // stable template from startTracking
    int ts = 0, rPx = 0;           // template size, radius in pixels
    int refW = 0, refH = 0;         // reference frame dimensions
    float trackX = 0.5f;            // current tracked X (normalized)
    float trackY = 0.5f;            // current tracked Y (normalized)
    float trackAngle = 0.0f;        // current tracked rotation (radians)
    float anchorX = 0.5f;           // anchor X (stabilization target)
    float anchorY = 0.5f;           // anchor Y (stabilization target)
    bool tracking = false;          // is tracking active?
    bool hasTrackedBefore = false;  // has any tracking been done this session
    int lastTrackedFrame = -1;      // last frame we ran NCC on
    int lastGoodFrame = -1;         // last frame with NCC > 0.7
    
    // Per-frame tracked data: frame, x, y, angle (radians) interleaved
    std::vector<float> trackData;
    bool dataLoaded = false;
    
    float autoZoom = 1.0f;
    float autoZoomCenterX = 0.5f;
    float autoZoomCenterY = 0.5f;
    bool autoZoomComputed = false;
    
    InstanceData() {}
    ~InstanceData() { delete[] templ; delete[] stableTempl; }
};

// Serialize trackData and anchor to a string param value
// Format: "anchorX,anchorY;frame,x,y,angle;frame,x,y,angle;..."
// angle in radians, omitted if zero for backward compat
static void serializeTrackData(OfxParamSetHandle ps, const std::vector<float>& data,
                                 float anchorX, float anchorY) {
    if(data.empty()) return;
    std::string s;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f,%.6f;", anchorX, anchorY);
    s += buf;
    for(size_t i = 0; i + 3 < data.size(); i += 4) {
        int f = (int)data[i];
        float ang = data[i+3];
        if(fabsf(ang) < 0.001f)
            snprintf(buf, sizeof(buf), "%d,%.6f,%.6f;", f, data[i+1], data[i+2]);
        else
            snprintf(buf, sizeof(buf), "%d,%.6f,%.6f,%.6f;", f, data[i+1], data[i+2], ang);
        s += buf;
    }
    OfxParamHandle p;
    if(gParam->paramGetHandle(ps, "_trackData", &p, nullptr) == kOfxStatOK && p)
        gParam->paramSetValue(p, s.c_str());
}

// Deserialize a string param value into trackData vector and anchor
// Handles current format (angle), new format (no angle = 0), and old format (no anchor)
static void deserializeTrackData(const char* str, std::vector<float>& data,
                                  float& outAnchorX, float& outAnchorY) {
    data.clear();
    outAnchorX = 0.5f; outAnchorY = 0.5f;
    if(!str || !str[0]) return;
    const char* p = str;
    
    // Try old-format first (starts with int)
    int testF; float tx, ty;
    if(sscanf(p, "%d,%f,%f;", &testF, &tx, &ty) == 3) {
        data.push_back((float)testF); data.push_back(tx); data.push_back(ty); data.push_back(0);
        while(*p && *p != ';') p++; if(*p == ';') p++;
    } else {
        float ax, ay;
        if(sscanf(p, "%f,%f;", &ax, &ay) == 2) {
            outAnchorX = ax; outAnchorY = ay;
            while(*p && *p != ';') p++; if(*p == ';') p++;
        }
    }
    while(*p) {
        int f; float x, y, a = 0;
        if(sscanf(p, "%d,%f,%f,%f;", &f, &x, &y, &a) == 4) {
            data.push_back((float)f); data.push_back(x); data.push_back(y); data.push_back(a);
            while(*p && *p != ';') p++; if(*p == ';') p++;
        } else if(sscanf(p, "%d,%f,%f;", &f, &x, &y) == 3) {
            data.push_back((float)f); data.push_back(x); data.push_back(y); data.push_back(0);
            while(*p && *p != ';') p++; if(*p == ';') p++;
        } else break;
    }
}

// Look up tracked data for a given frame. Returns true if found.
static bool getTrackedPos(const std::vector<float>& data, int frame,
                           float& outX, float& outY, float& outAngle) {
    for(size_t i = 0; i + 3 < data.size(); i += 4) {
        if((int)data[i] == frame) {
            outX = data[i+1]; outY = data[i+2]; outAngle = data[i+3];
            return true;
        }
    }
    return false;
}

static InstanceData* getInst(OfxImageEffectHandle e) {
    OfxPropertySetHandle p; void* v = nullptr;
    gEffect->getPropertySet(e, &p);
    gProp->propGetPointer(p, kOfxPropInstanceData, 0, &v);
    return (InstanceData*)v;
}

static OfxPropertySetHandle defP(OfxParamSetHandle ps, const char* type, const char* name, const char* label, const char* grp) {
    OfxPropertySetHandle pp; gParam->paramDefine(ps, type, name, &pp);
    gProp->propSetString(pp, kOfxPropLabel, 0, label);
    if(grp) gProp->propSetString(pp, kOfxParamPropParent, 0, grp); return pp;
}
static void defG(OfxParamSetHandle ps, const char* n, const char* l, bool o) {
    OfxPropertySetHandle pp; gParam->paramDefine(ps, kOfxParamTypeGroup, n, &pp);
    gProp->propSetString(pp, kOfxPropLabel, 0, l);
    gProp->propSetInt(pp, kOfxParamPropGroupOpen, 0, o?1:0);
}
static void defB(OfxParamSetHandle ps, const char* n, const char* l, const char* g, bool d) {
    OfxPropertySetHandle pp = defP(ps, kOfxParamTypeBoolean, n, l, g);
    gProp->propSetInt(pp, kOfxParamPropDefault, 0, d?1:0);
}
static void defD(OfxParamSetHandle ps, const char* n, const char* l, const char* g, double d, double mn, double mx) {
    OfxPropertySetHandle pp = defP(ps, kOfxParamTypeDouble, n, l, g);
    gProp->propSetDouble(pp, kOfxParamPropDefault, 0, d);
    gProp->propSetDouble(pp, kOfxParamPropMin, 0, mn);
    gProp->propSetDouble(pp, kOfxParamPropMax, 0, mx);
    gProp->propSetDouble(pp, kOfxParamPropDisplayMin, 0, mn);
    gProp->propSetDouble(pp, kOfxParamPropDisplayMax, 0, mx);
}
static double gD(OfxParamSetHandle ps, const char* n, double t) {
    OfxParamHandle p; double v=0;
    if(gParam->paramGetHandle(ps,n,&p,nullptr)==kOfxStatOK&&p) gParam->paramGetValueAtTime(p,t,&v);
    return v;
}
static int gI(OfxParamSetHandle ps, const char* n) {
    OfxParamHandle p; int v=0;
    if(gParam->paramGetHandle(ps,n,&p,nullptr)==kOfxStatOK&&p) gParam->paramGetValue(p,&v);
    return v;
}
static bool gB(OfxParamSetHandle ps, const char* n) { return gI(ps,n)!=0; }
static void sS(OfxParamSetHandle ps, const char* n, const char* v) {
    OfxParamHandle p; if(gParam->paramGetHandle(ps,n,&p,nullptr)==kOfxStatOK&&p) gParam->paramSetValue(p,v);
}

// ============= NCC with vDSP =============
static float computeNCC(const float* frame, int fw, int fh, int rb,
                         const float* templ, int ts, int rPx,
                         int cx, int cy) {
    static thread_local std::vector<float> fB, tB;
    int maxP=ts*ts; if((int)fB.size()<maxP){fB.resize(maxP);tB.resize(maxP);}
    float r2=(float)(rPx*rPx), hf=(float)(ts-1)*0.5f; int hh=(int)(hf+0.5f), n=0;
    for(int ty=0;ty<ts;ty++) for(int tx=0;tx<ts;tx++){
        float tdx=tx-hf,tdy=ty-hf;
        if(tdx*tdx+tdy*tdy>r2) continue;
        int fx=cx+tx-hh,fy=cy+ty-hh;
        if(fx<0||fx>=fw||fy<0||fy>=fh) continue;
        const float*p=(const float*)((const char*)frame+fy*rb)+fx*4;
        fB[n]=0.299f*p[0]+0.587f*p[1]+0.114f*p[2];
        tB[n]=templ[ty*ts+tx]; n++;
    }
    if(n<4) return 0;
    float sI,sT,sI2,sT2,sIT;
#ifdef __APPLE__
    vDSP_sve(fB.data(),1,&sI,n); vDSP_sve(tB.data(),1,&sT,n);
    vDSP_dotpr(fB.data(),1,fB.data(),1,&sI2,n);
    vDSP_dotpr(tB.data(),1,tB.data(),1,&sT2,n);
    vDSP_dotpr(fB.data(),1,tB.data(),1,&sIT,n);
#else
    double sI=0,sI2=0,sIT=0,sT=0,sT2=0;
    for(int i=0;i<n;i++){float fl=fB[i],tl=tB[i];sI+=fl;sI2+=fl*fl;sIT+=fl*tl;sT+=tl;sT2+=tl*tl;}
    sI2=(float)sI2;sT2=(float)sT2;sIT=(float)sIT;
#endif
    float fn=(float)n, num=fn*sIT-sT*sI;
    float den=sqrtf(fmaxf(0,(fn*sT2-sT*sT)*(fn*sI2-sI*sI)));
    return den>1e-10f?num/den:0;
}

// 3-pass search: coarse → medium → fine
// searches around (cx, cy) for best template match
static float search(const float* frame, int fw, int fh, int rb,
                    const float* templ, int ts, int rPx,
                    int cx, int cy, float* outDx, float* outDy) {
    float bestNcc=-2;
    int bestDx=0, bestDy=0;
    int ranges[3]={24,8,3}, steps[3]={4,2,1};
    for(int p=0;p<3;p++){
        int scx=cx+bestDx, scy=cy+bestDy;
        int relDx=0, relDy=0;
        float bestRelNcc=-2;
        for(int dy=-ranges[p];dy<=ranges[p];dy+=steps[p])
            for(int dx=-ranges[p];dx<=ranges[p];dx+=steps[p]){
                float ncc=computeNCC(frame,fw,fh,rb,templ,ts,rPx,scx+dx,scy+dy);
                if(ncc>bestRelNcc){bestRelNcc=ncc;relDx=dx;relDy=dy;}
            }
        if(bestRelNcc>bestNcc){
            bestNcc=bestRelNcc;
            bestDx+=relDx; bestDy+=relDy;
        }
    }
    *outDx=(float)bestDx;*outDy=(float)bestDy;
    return bestNcc;
}

// ============= Plugin Actions =============
static OfxStatus actionLoad() {
    gProp=(const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host,kOfxPropertySuite,1);
    gEffect=(const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host,kOfxImageEffectSuite,1);
    gParam=(const OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host,kOfxParameterSuite,1);
    return (gProp&&gEffect&&gParam)?kOfxStatOK:kOfxStatErrMissingHostFeature;
}

static OfxStatus actionDescribe(OfxImageEffectHandle e) {
    OfxPropertySetHandle p; gEffect->getPropertySet(e,&p);
    gProp->propSetString(p,kOfxPropLabel,0,PLUGIN_LABEL);
    gProp->propSetString(p,kOfxImageEffectPluginPropGrouping,0,PLUGIN_GROUP);
    gProp->propSetString(p,kOfxImageEffectPropSupportedContexts,0,kOfxImageEffectContextFilter);
    gProp->propSetString(p,kOfxImageEffectPropSupportedPixelDepths,0,kOfxBitDepthFloat);
    gProp->propSetInt(p,kOfxImageEffectFrameVarying,0,1);
    return kOfxStatOK;
}

static OfxStatus actionDescribeInContext(OfxImageEffectHandle e,OfxPropertySetHandle) {
    OfxPropertySetHandle c;
    gEffect->clipDefine(e,kOfxImageEffectSimpleSourceClipName,&c);
    gProp->propSetString(c,kOfxImageEffectPropSupportedComponents,0,kOfxImageComponentRGBA);
    gEffect->clipDefine(e,kOfxImageEffectOutputClipName,&c);
    gProp->propSetString(c,kOfxImageEffectPropSupportedComponents,0,kOfxImageComponentRGBA);
    OfxParamSetHandle ps;gEffect->getParamSet(e,&ps);
    defG(ps,"grp_region","Tracking",true);
    defD(ps,"center_x","Center X","grp_region",0.5,0.0,1.0);
    defD(ps,"center_y","Center Y","grp_region",0.5,0.0,1.0);
    defD(ps,"track_r","Radius","grp_region",0.02,0.001,0.5);
    defB(ps,"show_overlay","Show tracking anchor","grp_region",true);
    defB(ps,"stab_x","Stabilize X","grp_region",true);
    defB(ps,"stab_y","Stabilize Y","grp_region",true);
    defB(ps,"stab_rot","Stabilize Rotation","grp_region",false);
    defD(ps,"stab_amount","Stabilization Amount","grp_region",1.0,0.0,1.0);
    defB(ps,"auto_zoom","Auto Zoom","grp_region",false);
    defD(ps,"crop_zoom","Crop Zoom","grp_region",1.0,1.0,2.0);
    // Push button for starting tracking
    defP(ps,kOfxParamTypePushButton,"track_btn","Start Tracking","grp_region");
    // Debugging group (hidden by default)
    defG(ps,"grp_debug","Debugging",false);
    OfxPropertySetHandle pp = defP(ps,kOfxParamTypeString,"_status","Status","grp_debug");
    gProp->propSetString(pp,kOfxParamPropDefault,0,"Ready");
    OfxPropertySetHandle td = defP(ps,kOfxParamTypeString,"_trackData","Track Data","grp_debug");
    gProp->propSetString(td,kOfxParamPropDefault,0,"");
    return kOfxStatOK;
}

static OfxStatus actionCreateInstance(OfxImageEffectHandle e) {
    InstanceData* d = new InstanceData();
    OfxPropertySetHandle p; gEffect->getPropertySet(e,&p);
    gProp->propSetPointer(p,kOfxPropInstanceData,0,d);
    OfxParamSetHandle ps; gEffect->getParamSet(e,&ps);
    OfxParamHandle ph;
    if(gParam->paramGetHandle(ps,"_trackData",&ph,nullptr)==kOfxStatOK&&ph){
        const char* val=nullptr;
        if(gParam->paramGetValue(ph,&val)==kOfxStatOK&&val){
            deserializeTrackData(val,d->trackData,d->anchorX,d->anchorY);
            if(!d->trackData.empty()) d->dataLoaded=true;
        }
    }
    return kOfxStatOK;
}

static OfxStatus actionDestroyInstance(OfxImageEffectHandle e) {
    InstanceData* d = getInst(e);
    if(d){
        // Do NOT access host suites during destroy — may be partially torn down.
        // Data was already serialized incrementally during tracking.
        delete d;
    }
    OfxPropertySetHandle p;
    if(gEffect && gEffect->getPropertySet(e, &p) == kOfxStatOK && p && gProp)
        gProp->propSetPointer(p, kOfxPropInstanceData, 0, nullptr);
    return kOfxStatOK;
}

static OfxStatus actionIsIdentity(OfxImageEffectHandle e,OfxPropertySetHandle in,OfxPropertySetHandle out) {
    // We draw overlays and run tracking — never a clean passthrough.
    // Return reply default so Resolve calls our render.
    (void)e; (void)in; (void)out;
    return kOfxStatReplyDefault;
}

// Extract a circular template from an RGBA float image buffer
// Returns a new float[ts*ts] array — caller must delete[]
static float* extractTemplate(const void* frame, int fw, int fh, int rb,
                               int cxPx, int cyPx, int rPx, int ts) {
    float* t = new float[ts*ts];
    float half=(float)(ts-1)*0.5f, r2=(float)(rPx*rPx);
    for(int ty=0;ty<ts;ty++) for(int tx=0;tx<ts;tx++){
        float tdx=tx-half, tdy=ty-half;
        if(tdx*tdx+tdy*tdy>r2){t[ty*ts+tx]=0;continue;}
        int px=cxPx+tx-rPx, py=cyPx+ty-rPx;
        if(px<0||px>=fw||py<0||py>=fh){t[ty*ts+tx]=0;continue;}
        const float* p=(const float*)((const char*)frame+py*rb)+px*4;
        t[ty*ts+tx]=0.299f*p[0]+0.587f*p[1]+0.114f*p[2];
    }
    return t;
}

// ============= Start Tracking =============
static void startTracking(OfxImageEffectHandle e, InstanceData* d, double time) {
    OfxParamSetHandle ps; gEffect->getParamSet(e,&ps);
    
    // Save any existing track data and anchor before starting fresh
    if(!d->trackData.empty()) {
        serializeTrackData(ps, d->trackData, d->anchorX, d->anchorY);
        d->trackData.clear();
        d->dataLoaded = false;
    }
    
    double cx=gD(ps,"center_x",time), cy=gD(ps,"center_y",time), rad=gD(ps,"track_r",time);
    if(rad<0.01)rad=0.01;

    // Fetch reference frame at current time
    OfxImageClipHandle srcClip;
    gEffect->clipGetHandle(e,kOfxImageEffectSimpleSourceClipName,&srcClip,nullptr);
    if(!srcClip){sS(ps,"_status","Error: no clip");return;}

    sS(ps,"_status","Reading reference frame...");
    OfxPropertySetHandle refImg=nullptr;
    gEffect->clipGetImage(srcClip,time,nullptr,&refImg);
    if(!refImg){sS(ps,"_status","Can't read reference frame");return;}

    void* rp=nullptr;int rrb=0;OfxRectI b;
    gProp->propGetPointer(refImg,kOfxImagePropData,0,&rp);
    gProp->propGetInt(refImg,kOfxImagePropRowBytes,0,&rrb);
    gProp->propGetIntN(refImg,kOfxImagePropBounds,4,&b.x1);
    int fw=b.x2-b.x1,fh=b.y2-b.y1;
    if(!rp||fw<=0||fh<=0){gEffect->clipReleaseImage(refImg);sS(ps,"_status","Bad frame");return;}

    int rPx=(int)(rad*(float)(fw<fh?fw:fh)); if(rPx<4)rPx=4;
    int ts=2*rPx+1; int cxPx=(int)(cx*fw+0.5),cyPx=(int)(cy*fh+0.5);

    float* t = extractTemplate(rp, fw, fh, rrb, cxPx, cyPx, rPx, ts);
    float* s = extractTemplate(rp, fw, fh, rrb, cxPx, cyPx, rPx, ts);
    gEffect->clipReleaseImage(refImg);

    delete[] d->templ;
    delete[] d->stableTempl;
    d->templ=t; d->stableTempl=s; d->ts=ts; d->rPx=rPx; d->refW=fw; d->refH=fh;
    d->trackX=(float)cx; d->trackY=(float)cy;
    d->anchorX=(float)cx; d->anchorY=(float)cy;
    d->tracking=true;
    d->lastTrackedFrame=-1;
    d->autoZoomComputed = false;
    // Button label: first time → "Play clip to track", re-track → "Track again"
    const char* btnLabel = d->hasTrackedBefore ? "Track again" : "Play clip to track";
    d->hasTrackedBefore = true;
    OfxParamHandle btnH;
    if(gParam->paramGetHandle(ps,"track_btn",&btnH,nullptr)==kOfxStatOK&&btnH){
        OfxPropertySetHandle bp;
        if(gParam->paramGetPropertySet(btnH,&bp)==kOfxStatOK&&bp)
            gProp->propSetString(bp,kOfxPropLabel,0,btnLabel);
    }
    // Auto-hide the tracking anchor when tracking starts
    OfxParamHandle ovH;
    if(gParam->paramGetHandle(ps,"show_overlay",&ovH,nullptr)==kOfxStatOK&&ovH)
        gParam->paramSetValue(ovH,0);
    sS(ps,"_status","Tracking active — play through the clip");
}

// Fast translation warp: integer offset with sub-pixel edge interpolation
static void warpTranslate(float* dst, int dw, int dh, int dRB,
                          const float* src, int sw, int sh, int sRB,
                          float dx, float dy) {
    // Round to nearest integer offset for the fast path
    int idx = (int)(dx + (dx >= 0 ? 0.5f : -0.5f));
    int idy = (int)(dy + (dy >= 0 ? 0.5f : -0.5f));
    
    // Bail out if offset is so large there's no overlap
    if(idx <= -sw || idx >= sw || idy <= -sh || idy >= sh) {
        // Fill with black
        for(int y = 0; y < dh; y++) {
            float* dp = (float*)((char*)dst + (size_t)y*dRB);
            for(int x = 0; x < dw; x++) {
                dp[x*4+0] = 0; dp[x*4+1] = 0; dp[x*4+2] = 0; dp[x*4+3] = 1;
            }
        }
        return;
    }
    
    // Integer translation: dst[x, y] = src[x + dx, y + dy]
    // When dx > 0: read from src starting at dx, write to dst starting at 0 (image shifts left)
    // When dx < 0: read from src starting at 0, write to dst starting at -dx (image shifts right)
    int absDx = idx < 0 ? -idx : idx;
    int absDy = idy < 0 ? -idy : idy;
    int srcX0 = (idx > 0) ? idx : 0;
    int srcY0 = (idy > 0) ? idy : 0;
    int dstX0 = (idx < 0) ? absDx : 0;
    int dstY0 = (idy < 0) ? absDy : 0;
    int copyW = dw - absDx;
    int copyH = dh - absDy;
    if(copyW > sw) copyW = sw;
    if(copyH > sh) copyH = sh;
    if(copyW < 0) copyW = 0;
    if(copyH < 0) copyH = 0;
    
    // Fill destination with black first
    for(int y = 0; y < dh; y++) {
        float* dp = (float*)((char*)dst + (size_t)y*dRB);
        for(int x = 0; x < dw; x++) {
            dp[x*4+0] = 0; dp[x*4+1] = 0; dp[x*4+2] = 0; dp[x*4+3] = 1;
        }
    }
    
    // Copy the overlapping region (integer-offset, no interpolation needed when dx/dy are near-integer)
    if(copyW > 0 && copyH > 0) {
        for(int y = 0; y < copyH; y++) {
            const float* sp = (const float*)((const char*)src + (size_t)(srcY0 + y)*sRB) + srcX0*4;
            float* dp = (float*)((char*)dst + (size_t)(dstY0 + y)*dRB) + dstX0*4;
            memcpy(dp, sp, (size_t)copyW*4*sizeof(float));
        }
    }
}

// Rotate a square luminance template by angle (radians) using bilinear interpolation
static void rotateTemplate(const float* src, int ts, float* dst, float angle) {
    float half = (float)(ts-1) * 0.5f;
    float cosA = cosf(angle), sinA = sinf(angle);
    for(int y = 0; y < ts; y++) {
        for(int x = 0; x < ts; x++) {
            float sx = (x - half) * cosA - (y - half) * sinA + half;
            float sy = (x - half) * sinA + (y - half) * cosA + half;
            if(sx < 0 || sx >= ts-1 || sy < 0 || sy >= ts-1) { dst[y*ts+x] = 0; continue; }
            int x0 = (int)sx, y0 = (int)sy;
            int x1 = x0+1 < ts ? x0+1 : ts-1, y1 = y0+1 < ts ? y0+1 : ts-1;
            float fx = sx - x0, fy = sy - y0;
            dst[y*ts+x] = src[y0*ts+x0]*(1-fx)*(1-fy) + src[y0*ts+x1]*fx*(1-fy)
                        + src[y1*ts+x0]*(1-fx)*fy + src[y1*ts+x1]*fx*fy;
        }
    }
}

// Search with rotation: tries the template at multiple angles, keeps best (ncc, dx, dy, angle)
static float searchRotate(const float* frame, int fw, int fh, int rb,
                           const float* templ, int ts, int rPx,
                           int cx, int cy, float* outDx, float* outDy, float* outAngle) {
    float bestNcc = -2, bestDx = 0, bestDy = 0, bestAngle = 0;
    // Search over ±3° in 1° steps
    static thread_local std::vector<float> rotatedTempl;
    if((int)rotatedTempl.size() < ts*ts) rotatedTempl.resize(ts*ts);
    for(int ai = -3; ai <= 3; ai++) {
        float angle = (float)ai * 0.0174533f; // degrees to radians
        rotateTemplate(templ, ts, rotatedTempl.data(), angle);
        float dx, dy;
        float ncc = search(frame, fw, fh, rb, rotatedTempl.data(), ts, rPx, cx, cy, &dx, &dy);
        if(ncc > bestNcc) { bestNcc = ncc; bestDx = dx; bestDy = dy; bestAngle = angle; }
    }
    *outDx = bestDx; *outDy = bestDy; *outAngle = bestAngle;
    return bestNcc;
}

// Combined rotation + translation + zoom warp with bilinear interpolation
// Rotates about anchor point, scales by zoom, then translates by (dx, dy)
static void warpTranslateRotate(float* dst, int dw, int dh, int dRB,
                                 const float* src, int sw, int sh, int sRB,
                                 float dx, float dy, float angle, float zoom,
                                 float anchorX, float anchorY) {
    float cx = anchorX * (float)dw;
    float cy = anchorY * (float)dh;
    float cosA = cosf(angle), sinA = sinf(angle);
    float invZ = 1.0f / zoom;
    for(int y = 0; y < dh; y++) {
        float* dp = (float*)((char*)dst + (size_t)y*dRB);
        float ny = ((float)y - cy) * invZ;
        for(int x = 0; x < dw; x++) {
            float nx = ((float)x - cx) * invZ;
            float sx = nx * cosA - ny * sinA + cx + dx;
            float sy = nx * sinA + ny * cosA + cy + dy;
            if(sx < 0 || sx >= sw-1 || sy < 0 || sy >= sh-1) {
                dp[x*4] = 0; dp[x*4+1] = 0; dp[x*4+2] = 0; dp[x*4+3] = 1; continue;
            }
            int x0 = (int)sx, y0 = (int)sy;
            int x1 = x0+1 < sw ? x0+1 : sw-1, y1 = y0+1 < sh ? y0+1 : sh-1;
            float fx = sx - x0, fy = sy - y0;
            const float* p00 = (const float*)((const char*)src + y0*sRB) + x0*4;
            const float* p10 = (const float*)((const char*)src + y0*sRB) + x1*4;
            const float* p01 = (const float*)((const char*)src + y1*sRB) + x0*4;
            const float* p11 = (const float*)((const char*)src + y1*sRB) + x1*4;
            for(int c = 0; c < 4; c++)
                dp[x*4+c] = (p00[c]*(1-fx)+p10[c]*fx)*(1-fy) + (p01[c]*(1-fx)+p11[c]*fx)*fy;
        }
    }
}

// ============= Render =============
static OfxStatus actionRender(OfxImageEffectHandle e,OfxPropertySetHandle in) {
    double time; gProp->propGetDouble(in,kOfxPropTime,0,&time);
    int curFrame=(int)floor(time+0.5);
    OfxParamSetHandle ps; gEffect->getParamSet(e,&ps);
    InstanceData* d=getInst(e);

    OfxImageClipHandle srcClip,dstClip;
    gEffect->clipGetHandle(e,kOfxImageEffectSimpleSourceClipName,&srcClip,nullptr);
    gEffect->clipGetHandle(e,kOfxImageEffectOutputClipName,&dstClip,nullptr);

    // ===== RESOLVE POSITION for this frame =====
    bool positionResolved = false;
    float ovX=0.5f, ovY=0.5f;  // overlay position
    
    // Priority 1: Use stored data if available (subsequent passes)
    if(d && !d->trackData.empty()) {
        float sx, sy, sa;
        if(getTrackedPos(d->trackData, curFrame, sx, sy, sa)) {
            d->trackX = sx; d->trackY = sy;
            ovX = sx; ovY = sy;
            positionResolved = true;
            char buf[128];
            snprintf(buf,128,"[TRACKED] F%d (%.4f,%.4f)", curFrame, sx, sy);
            sS(ps,"_status",buf);
        } else {
            char buf[128];
            snprintf(buf,128,"[MISS] F%d — no tracking data", curFrame);
            sS(ps,"_status",buf);
        }
    }
    
    // Priority 2: No stored data yet — run NCC and store result
    if(!positionResolved && d && d->tracking && d->templ && curFrame != d->lastTrackedFrame){
        OfxPropertySetHandle curImg=nullptr;
        gEffect->clipGetImage(srcClip,time,nullptr,&curImg);
        if(curImg){
            void* cp=nullptr;int crb=0;
            gProp->propGetPointer(curImg,kOfxImagePropData,0,&cp);
            gProp->propGetInt(curImg,kOfxImagePropRowBytes,0,&crb);
            if(cp){
                OfxRectI curB;
                gProp->propGetIntN(curImg,kOfxImagePropBounds,4,&curB.x1);
                int curW=curB.x2-curB.x1, curH=curB.y2-curB.y1;
                
                int searchCx=(int)(d->trackX*curW+0.5f);
                int searchCy=(int)(d->trackY*curH+0.5f);
                
                float dx=0, dy=0, rotAngle=0;
                bool doRot = d && gB(ps, "stab_rot");
                float ncc;
                if(doRot) {
                    ncc = searchRotate((const float*)cp,curW,curH,crb,
                                      d->templ,d->ts,d->rPx,
                                      searchCx,searchCy,&dx,&dy,&rotAngle);
                } else {
                    ncc = search((const float*)cp,curW,curH,crb,
                                d->templ,d->ts,d->rPx,
                                searchCx,searchCy,&dx,&dy);
                }
                
                if(ncc<0.15f && d->stableTempl){
                    float sdx=0,sdy=0, srot=0;
                    float sncc;
                    if(doRot) {
                        sncc = searchRotate((const float*)cp,curW,curH,crb,
                                           d->stableTempl,d->ts,d->rPx,
                                           searchCx,searchCy,&sdx,&sdy,&srot);
                    } else {
                        sncc = search((const float*)cp,curW,curH,crb,
                                     d->stableTempl,d->ts,d->rPx,
                                     searchCx,searchCy,&sdx,&sdy);
                    }
                    if(sncc>ncc){
                        ncc=sncc; dx=sdx; dy=sdy; rotAngle=srot;
                        delete[] d->templ;
                        float* t2 = extractTemplate(cp, curW, curH, crb,
                                                     searchCx+(int)(sdx+0.5f),
                                                     searchCy+(int)(sdy+0.5f),
                                                     d->rPx, d->ts);
                        d->templ = t2;
                    }
                }
                
                if(ncc>0.15f){
                    float newX = d->trackX + (float)dx/curW;
                    float newY = d->trackY + (float)dy/curH;
                    if(newX>=0&&newX<=1&&newY>=0&&newY<=1){
                        d->trackX=newX; d->trackY=newY;
                        d->trackAngle = rotAngle;
                        ovX=newX; ovY=newY;
                        positionResolved = true;
                        
                        // Store result in trackData (frame, x, y, angle)
                        d->trackData.push_back((float)curFrame);
                        d->trackData.push_back(newX);
                        d->trackData.push_back(newY);
                        d->trackData.push_back(rotAngle);
                        // Periodically serialize so data persists if Resolve crashes
                        if((d->trackData.size() % 120) < 4) // every ~30 frames
                            serializeTrackData(ps, d->trackData, d->anchorX, d->anchorY);
                        
                        if(ncc>0.7f && d->lastGoodFrame!=curFrame){
                            int newCxPx=(int)(d->trackX*curW+0.5f);
                            int newCyPx=(int)(d->trackY*curH+0.5f);
                            float* newT = extractTemplate(cp, curW, curH, crb,
                                                           newCxPx, newCyPx, d->rPx, d->ts);
                            delete[] d->templ;
                            d->templ = newT;
                            d->refW = curW; d->refH = curH;
                            d->lastGoodFrame=curFrame;
                        }
                    }
                }
                d->lastTrackedFrame=curFrame;
                char buf[256];
                if(ncc > 0.15f) {
                    snprintf(buf,256,"[TRACKED] F%d NCC=%.2f dx=%.0f dy=%.0f (%.4f,%.4f)",
                             curFrame,ncc,dx,dy,d->trackX,d->trackY);
                } else {
                    snprintf(buf,256,"[MISS] F%d NCC=%.2f – low confidence", curFrame,ncc);
                }
                sS(ps,"_status",buf);
            }
            gEffect->clipReleaseImage(curImg);
        }
    }
    
    // Priority 3: No stored data, tracking not active — use slider position
    if(!positionResolved) {
        ovX = (float)gD(ps, "center_x", time);
        ovY = (float)gD(ps, "center_y", time);
        char buf[128];
        snprintf(buf,128,"[STANDBY] F%d — no tracking data", curFrame);
        sS(ps,"_status",buf);
    }

    // ===== STABILIZATION: if enabled, compute warp offset =====
    bool doWarp = false;
    float warpDx=0, warpDy=0, warpAngle=0;
    bool warpRot=false;
    bool stabilizeNow = d && (gB(ps, "stab_x") || gB(ps, "stab_y") || gB(ps, "stab_rot"));
    bool hasTrackData = d && (!d->trackData.empty() || d->dataLoaded);
    if(stabilizeNow && hasTrackData && positionResolved) {
        float curX, curY;
        float curAngle;
        bool havePos = getTrackedPos(d->trackData, curFrame, curX, curY, curAngle);
        if(!havePos) {
            curX = d->trackX;
            curY = d->trackY;
            curAngle = d->trackAngle;
        }
        float amount = (float)gD(ps, "stab_amount", time);
        warpDx = (curX - d->anchorX) * amount;
        warpDy = (curY - d->anchorY) * amount;
        warpAngle = -curAngle * amount;
        if(!gB(ps,"stab_x")) warpDx = 0;
        if(!gB(ps,"stab_y")) warpDy = 0;
        if(!gB(ps,"stab_rot")) warpAngle = 0;
        warpRot = gB(ps,"stab_rot") && fabsf(warpAngle) > 0.001f;
        doWarp = (fabsf(warpDx) > 0.0001f || fabsf(warpDy) > 0.0001f || warpRot);
        // Overlay: interpolates between tracked and anchor when amount < 1.0
        ovX = gB(ps,"stab_x") ? d->anchorX * amount + d->trackX * (1.0f - amount) : d->trackX;
        ovY = gB(ps,"stab_y") ? d->anchorY * amount + d->trackY * (1.0f - amount) : d->trackY;
        
        char buf[256];
        bool isTracked = havePos || (d->trackData.size() >= 3);
        snprintf(buf,256,"%s F%d a(%.4f,%.4f) c(%.4f,%.4f) d(%.4f,%.4f) %s",
                 isTracked ? "[TRACKED]" : "[MISS]",
                 curFrame,
                 d->anchorX, d->anchorY,
                 curX, curY,
                 warpDx, warpDy,
                 doWarp ? "WARP" : "PASS");
    }

    // ===== RENDER: passthrough or stabilized warp =====
    OfxPropertySetHandle srcImg=nullptr,dstImg=nullptr;
    gEffect->clipGetImage(srcClip,time,nullptr,&srcImg);
    gEffect->clipGetImage(dstClip,time,nullptr,&dstImg);
    if(!srcImg||!dstImg){if(srcImg)gEffect->clipReleaseImage(srcImg);if(dstImg)gEffect->clipReleaseImage(dstImg);return kOfxStatFailed;}

    int w=0,h=0; OfxRectI b; gProp->propGetIntN(dstImg,kOfxImagePropBounds,4,&b.x1);
    w=b.x2-b.x1;h=b.y2-b.y1;

    void* sp=nullptr,*dp=nullptr;int rb=0,db=0;
    gProp->propGetPointer(srcImg,kOfxImagePropData,0,&sp);
    gProp->propGetPointer(dstImg,kOfxImagePropData,0,&dp);
    gProp->propGetInt(srcImg,kOfxImagePropRowBytes,0,&rb);
    gProp->propGetInt(dstImg,kOfxImagePropRowBytes,0,&db);
    
    if(sp&&dp) {
        // Compute zoom factor (auto or manual) — independent of stabilization
        float cropZoom = 1.0f;
        bool doAutoZoom = d && gB(ps, "auto_zoom") && d->trackData.size() >= 4;
        if(doAutoZoom) {
            if(!d->autoZoomComputed) {
                float maxPosDx = 0, maxNegDx = 0, maxPosDy = 0, maxNegDy = 0;
                for(size_t di = 0; di + 3 < d->trackData.size(); di += 4) {
                    float dx = d->trackData[di+1] - d->anchorX;
                    float dy = d->trackData[di+2] - d->anchorY;
                    if(dx > maxPosDx) maxPosDx = dx;
                    if(dx < maxNegDx) maxNegDx = dx;
                    if(dy > maxPosDy) maxPosDy = dy;
                    if(dy < maxNegDy) maxNegDy = dy;
                }
                int mpx = (int)(fmaxf(maxPosDx, -maxNegDx) * (float)w + 0.5f);
                int mpy = (int)(fmaxf(maxPosDy, -maxNegDy) * (float)h + 0.5f);
                int cw = w - mpx; if(cw < 1) cw = 1;
                int ch = h - mpy; if(ch < 1) ch = 1;
                d->autoZoom = fmaxf((float)w / (float)cw, (float)h / (float)ch);
                float peakDx = (maxPosDx > -maxNegDx) ? maxPosDx : maxNegDx;
                float peakDy = (maxPosDy > -maxNegDy) ? maxPosDy : maxNegDy;
                d->autoZoomCenterX = (peakDx >= 0) ? 0.0f : 1.0f;
                d->autoZoomCenterY = (peakDy >= 0) ? 0.0f : 1.0f;
                d->autoZoomComputed = true;
            }
            cropZoom = d->autoZoom;
        } else {
            cropZoom = (float)gD(ps, "crop_zoom", time);
        }
        
        bool doCrop = cropZoom > 1.01f;
        if(doWarp && doCrop) {
            // Combined stabilization + zoom via GPU
            float dx = warpDx * (float)w;
            float dy = warpDy * (float)h;
#ifdef __APPLE__
            if(!warpRot) {
                float czX = doAutoZoom ? d->autoZoomCenterX * (float)w : (float)w * 0.5f;
                float czY = doAutoZoom ? d->autoZoomCenterY * (float)h : (float)h * 0.5f;
                TriggerMetalWarpFromCPU((float*)dp, (const float*)sp, w, h, dx, dy, cropZoom, czX, czY);
            } else
#endif
            {
                warpTranslateRotate((float*)dp, w, h, db, (const float*)sp, w, h, rb,
                                    dx, dy, warpAngle, cropZoom, ovX, ovY);
            }
        } else if(doWarp) {
            warpTranslate((float*)dp, w, h, db,
                          (const float*)sp, w, h, rb,
                          warpDx * (float)w, warpDy * (float)h);
        } else if(doCrop) {
            // Zoom only (no stabilization) via GPU
#ifdef __APPLE__
            TriggerMetalWarpFromCPU((float*)dp, (const float*)sp, w, h, 0, 0, cropZoom, (float)w*0.5f, (float)h*0.5f);
#else
            for(int y=0;y<h;y++)
                memcpy((char*)dp+(size_t)y*db,(const char*)sp+(size_t)y*rb,(size_t)w*4*sizeof(float));
#endif
        } else {
            for(int y=0;y<h;y++)
                memcpy((char*)dp+(size_t)y*db,(const char*)sp+(size_t)y*rb,(size_t)w*4*sizeof(float));
        }
    }

    // ===== OVERLAY: draw circle at resolved position =====
    if(dp&&gB(ps,"show_overlay")){
        double or_=gD(ps,"track_r",time);
        int pcx=(int)(ovX*w),pcy=(int)(ovY*h),pr=(int)(or_*(w<h?w:h));
        if(pr<2)pr=2;
        for(int a=0;a<720;a++){
            float rad=(float)a*0.0087266f;
            for(int t=-1;t<=1;t++){
                int cx_=pcx+(int)((pr+t)*cosf(rad)),cy_=pcy+(int)((pr+t)*sinf(rad));
                if(cx_>=0&&cx_<w&&cy_>=0&&cy_<h){
                    float*px=(float*)((char*)dp+cy_*db)+cx_*4;
                    px[0]=0;px[1]=1;px[2]=0;px[3]=1;
                }
            }
        }
        // Draw crosshair slightly thicker
        int cs=pr/5;if(cs<4)cs=4;
        for(int i=-cs;i<=cs;i++){
            int x1=pcx+i,y1=pcy;if(x1>=0&&x1<w&&y1>=0&&y1<h){float*px=(float*)((char*)dp+y1*db)+x1*4;px[0]=0;px[1]=1;px[2]=0;px[3]=1;}
            int x2=pcx,y2=pcy+i;if(x2>=0&&x2<w&&y2>=0&&y2<h){float*px=(float*)((char*)dp+y2*db)+x2*4;px[0]=0;px[1]=1;px[2]=0;px[3]=1;}
        }
    }

    gEffect->clipReleaseImage(srcImg);gEffect->clipReleaseImage(dstImg);
    return kOfxStatOK;
}

// ============= Instance Changed =============
static OfxStatus actionInstanceChanged(OfxImageEffectHandle e,OfxPropertySetHandle in) {
    char*type=nullptr,*name=nullptr;
    gProp->propGetString(in,kOfxPropType,0,&type);
    gProp->propGetString(in,kOfxPropName,0,&name);
    if(!type||!name||strcmp(type,kOfxTypeParameter)!=0) return kOfxStatReplyDefault;
    if(!strcmp(name,"track_btn")){
        InstanceData*d=getInst(e);
        double time=0;
        gProp->propGetDouble(in,kOfxPropTime,0,&time);
        if(d)startTracking(e,d,time);
        return kOfxStatOK;
    }
    if(!strcmp(name,"auto_zoom")){
        InstanceData*d=getInst(e);
        if(d) d->autoZoomComputed = false;
        return kOfxStatOK;
    }
    if(!strcmp(name,"center_x")||!strcmp(name,"center_y")){
        InstanceData*d=getInst(e);
        if(d){
            OfxParamSetHandle ps; gEffect->getParamSet(e,&ps);
            double time=0;
            gProp->propGetDouble(in,kOfxPropTime,0,&time);
            double cx=gD(ps,"center_x",time), cy=gD(ps,"center_y",time);
            // Update anchor position — user is repositioning the feature target
            d->anchorX=(float)cx; d->anchorY=(float)cy;
            // Also update the current frame's tracked position so the circle follows
            if(!d->trackData.empty() && ps) {
                int curF = (int)floor(time+0.5);
                for(size_t i=0;i+3<d->trackData.size();i+=4){
                    if((int)d->trackData[i]==curF){
                        d->trackData[i+1]=(float)cx;
                        d->trackData[i+2]=(float)cy;
                        break;
                    }
                }
                serializeTrackData(ps,d->trackData,d->anchorX,d->anchorY);
            }
            char buf[128];
            snprintf(buf,128,"Anchor moved to (%.4f,%.4f)",(float)cx,(float)cy);
            sS(ps,"_status",buf);
        }
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

static OfxStatus pluginMain(const char* action,const void* handle,OfxPropertySetHandle in,OfxPropertySetHandle out) {
    OfxImageEffectHandle e=(OfxImageEffectHandle)handle;
    if(!strcmp(action,kOfxActionLoad)) return actionLoad();
    if(!strcmp(action,kOfxActionUnload)) return kOfxStatOK;
    if(!strcmp(action,kOfxActionDescribe)) return actionDescribe(e);
    if(!strcmp(action,kOfxImageEffectActionDescribeInContext)) return actionDescribeInContext(e,in);
    if(!strcmp(action,kOfxActionCreateInstance)) return actionCreateInstance(e);
    if(!strcmp(action,kOfxActionDestroyInstance)) return actionDestroyInstance(e);
    if(!strcmp(action,kOfxActionInstanceChanged)) return actionInstanceChanged(e,in);
    if(!strcmp(action,kOfxImageEffectActionIsIdentity)) return actionIsIdentity(e,in,out);
    if(!strcmp(action,kOfxImageEffectActionRender)) return actionRender(e,in);
    return kOfxStatReplyDefault;
}

static void setHost(OfxHost*h){gHost=h;}
static OfxPlugin pluginStruct={kOfxImageEffectPluginApi,1,PLUGIN_ID,PLUGIN_VERSION_MAJOR,PLUGIN_VERSION_MINOR,setHost,pluginMain};
extern "C"{int OfxGetNumberOfPlugins(){return 1;}OfxPlugin* OfxGetPlugin(int n){return n==0?&pluginStruct:nullptr;}}
