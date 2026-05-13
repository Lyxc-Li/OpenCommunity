#pragma once

#include <jni.h>

struct AxisAlignedBB_t {
    double minX = 0.0;
    double minY = 0.0;
    double minZ = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double maxZ = 0.0;
};

class AxisAlignedBB {
public:
    AxisAlignedBB_t GetNativeBoundingBox(JNIEnv* env);
    void SetNativeBoundingBox(AxisAlignedBB_t buffer, JNIEnv* env);
};
