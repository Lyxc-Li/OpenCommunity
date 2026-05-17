#pragma once

#include "../../game/classes/AxisAlignedBB.h"

#include <jni.h>

#include <string>
#include <vector>

namespace BlockVisuals {
    struct ScreenPoint {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct WorldPoint {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct RenderMatrixSnapshot {
        std::vector<float> modelView;
        std::vector<float> projection;
        int viewportWidth = 0;
        int viewportHeight = 0;

        bool IsValid() const {
            return modelView.size() == 16 &&
                projection.size() == 16 &&
                viewportWidth > 0 &&
                viewportHeight > 0;
        }
    };

    struct BlockDescriptor {
        std::string displayName;
        std::string compactName;
        std::vector<std::string> aliases;
        bool isAir = false;
    };

    RenderMatrixSnapshot CaptureRenderMatrixSnapshot();
    bool TryProjectPoint(const WorldPoint& worldPoint, const RenderMatrixSnapshot& snapshot, ScreenPoint& screenPoint);
    void DrawWireBox(double minX, double minY, double minZ, double maxX, double maxY, double maxZ);
    void DrawWireBox(const AxisAlignedBB_t& bounds);

    jobject GetBlockObjectAt(JNIEnv* env, jobject worldObject, int x, int y, int z);
    BlockDescriptor DescribeBlock(JNIEnv* env, jobject blockObject);
    std::string NormalizeBlockQuery(const std::string& text);
    bool BlockMatchesQuery(const BlockDescriptor& descriptor, const std::string& normalizedQuery);
    bool IsExposedToAir(JNIEnv* env, jobject worldObject, int x, int y, int z);
}
