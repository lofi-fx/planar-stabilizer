#ifndef PLANAR_PARAMS_H
#define PLANAR_PARAMS_H

#include <vector>
#include <string>

#define PLUGIN_ID             "com.lofifx.planarstabilizer"
#define PLUGIN_LABEL          "LoFi FX Planar Stabilizer"
#define PLUGIN_GROUP          "LoFi FX"
#define PLUGIN_VERSION_MAJOR  1
#define PLUGIN_VERSION_MINOR  0

struct FrameTransform {
    float tx = 0;
    float ty = 0;
    float rotation = 0;
    float scale = 1.0f;
    float confidence = 0;
};

struct TrackingData {
    int frameCount = 0;
    int width = 0;
    int height = 0;
    float centerX = 0.5f;
    float centerY = 0.5f;
    float radius = 0.1f;
    std::vector<FrameTransform> frames;

};

#endif
