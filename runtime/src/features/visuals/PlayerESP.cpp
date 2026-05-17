#include "pch.h"
#include "PlayerESP.h"

#include "../../core/RenderHook.h"
#include "../../game/classes/Minecraft.h"
#include "../../game/classes/Player.h"
#include "../../game/classes/Timer.h"
#include "../../game/classes/World.h"
#include "../../game/jni/GameInstance.h"

#include <gl/GL.h>
#include <vector>

namespace {
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

    struct PlayerRenderEntry {
        jobject entity = nullptr;
        double renderX = 0.0;
        double renderY = 0.0;
        double renderZ = 0.0;
    };

    RenderMatrixSnapshot CaptureRenderMatrixSnapshot() {
        std::lock_guard<std::mutex> lock(RenderCache::mtx);
        return {
            RenderCache::modelView,
            RenderCache::projection,
            RenderCache::viewportW,
            RenderCache::viewportH
        };
    }

    void DrawWireBox(double minX, double minY, double minZ, double maxX, double maxY, double maxZ) {
        glBegin(GL_LINES);

        glVertex3d(minX, minY, minZ); glVertex3d(maxX, minY, minZ);
        glVertex3d(maxX, minY, minZ); glVertex3d(maxX, minY, maxZ);
        glVertex3d(maxX, minY, maxZ); glVertex3d(minX, minY, maxZ);
        glVertex3d(minX, minY, maxZ); glVertex3d(minX, minY, minZ);

        glVertex3d(minX, maxY, minZ); glVertex3d(maxX, maxY, minZ);
        glVertex3d(maxX, maxY, minZ); glVertex3d(maxX, maxY, maxZ);
        glVertex3d(maxX, maxY, maxZ); glVertex3d(minX, maxY, maxZ);
        glVertex3d(minX, maxY, maxZ); glVertex3d(minX, maxY, minZ);

        glVertex3d(minX, minY, minZ); glVertex3d(minX, maxY, minZ);
        glVertex3d(maxX, minY, minZ); glVertex3d(maxX, maxY, minZ);
        glVertex3d(maxX, minY, maxZ); glVertex3d(maxX, maxY, maxZ);
        glVertex3d(minX, minY, maxZ); glVertex3d(minX, maxY, maxZ);

        glEnd();
    }
}

void PlayerESP::RenderOverlay(ImDrawList* drawList, float screenW, float screenH) {
    (void)drawList;
    (void)screenW;
    (void)screenH;

    if (!IsEnabled() || !g_Game || !g_Game->IsInitialized()) {
        return;
    }

    JNIEnv* env = g_Game->GetCurrentEnv();
    if (!env || env->PushLocalFrame(768) != 0) {
        return;
    }

    if (Minecraft::GetCurrentScreen(env)) {
        env->PopLocalFrame(nullptr);
        return;
    }

    jobject localPlayerObject = Minecraft::GetThePlayer(env);
    jobject worldObject = Minecraft::GetTheWorld(env);
    jobject timerObject = Minecraft::GetTimer(env);
    if (!localPlayerObject || !worldObject || !timerObject) {
        env->PopLocalFrame(nullptr);
        return;
    }

    const RenderMatrixSnapshot snapshot = CaptureRenderMatrixSnapshot();
    if (!snapshot.IsValid()) {
        env->PopLocalFrame(nullptr);
        return;
    }

    const float partialTicks = reinterpret_cast<Timer*>(timerObject)->GetRenderPartialTicks(env);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->PopLocalFrame(nullptr);
        return;
    }

    std::vector<PlayerRenderEntry> renderEntries;
    const auto players = reinterpret_cast<World*>(worldObject)->GetPlayerEntities(env);
    renderEntries.reserve(players.size());

    for (auto* player : players) {
        if (!player) {
            continue;
        }

        jobject playerObject = reinterpret_cast<jobject>(player);
        if (env->IsSameObject(playerObject, localPlayerObject)) {
            env->DeleteLocalRef(playerObject);
            continue;
        }

        if (player->GetHealth(env) <= 0.0f || player->HasZeroedBoundingBox(env)) {
            env->DeleteLocalRef(playerObject);
            continue;
        }

        const Vec3D currentPos = player->GetPos(env);
        const Vec3D previousPos = player->GetLastTickPos(env);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(playerObject);
            continue;
        }

        renderEntries.push_back({
            playerObject,
            previousPos.x + ((currentPos.x - previousPos.x) * partialTicks),
            previousPos.y + ((currentPos.y - previousPos.y) * partialTicks),
            previousPos.z + ((currentPos.z - previousPos.z) * partialTicks)
        });
    }

    if (renderEntries.empty()) {
        env->PopLocalFrame(nullptr);
        return;
    }

    const float* color = GetColor();

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_LINE_BIT | GL_POLYGON_BIT | GL_TEXTURE_BIT | GL_LIGHTING_BIT);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(snapshot.projection.data());

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(snapshot.modelView.data());

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glLineWidth(1.6f);
    glColor4f(color[0], color[1], color[2], color[3]);

    for (const auto& entry : renderEntries) {
        auto* player = reinterpret_cast<Player*>(entry.entity);
        float width = player->GetWidth(env);
        float height = player->GetHeight(env);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            continue;
        }

        if (width <= 0.05f) {
            width = 0.6f;
        }
        if (height <= 0.05f) {
            height = 1.8f;
        }

        const double halfWidth = static_cast<double>(width) * 0.5;
        DrawWireBox(
            entry.renderX - halfWidth,
            entry.renderY,
            entry.renderZ - halfWidth,
            entry.renderX + halfWidth,
            entry.renderY + static_cast<double>(height),
            entry.renderZ + halfWidth);
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glLineWidth(1.0f);
    glDisable(GL_LINE_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);
    glDisable(GL_BLEND);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();

    for (const auto& entry : renderEntries) {
        if (entry.entity) {
            env->DeleteLocalRef(entry.entity);
        }
    }

    MarkInUse(120);
    env->PopLocalFrame(nullptr);
}
