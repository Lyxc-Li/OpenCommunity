#include "pch.h"
#include "PlayerChams.h"

#include "../../core/RenderHook.h"
#include "../../game/classes/Minecraft.h"
#include "../../game/classes/Player.h"
#include "../../game/classes/RenderManager.h"
#include "../../game/classes/Timer.h"
#include "../../game/classes/World.h"
#include "../../game/jni/Class.h"
#include "../../game/jni/GameInstance.h"
#include "../../game/jni/Method.h"
#include "../../game/mapping/Mapper.h"

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
        float entityYaw = 0.0f;
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

    jobject GetEntityRenderObject(JNIEnv* env, jobject renderManagerObject, jobject entityObject) {
        if (!env || !renderManagerObject || !entityObject || !g_Game || !g_Game->IsInitialized()) {
            return nullptr;
        }

        const std::string entitySignature = Mapper::Get("net/minecraft/entity/Entity", 2);
        const std::string renderSignature = Mapper::Get("net/minecraft/client/renderer/entity/Render", 2);
        const std::string methodName = Mapper::Get("getEntityRenderObject");
        if (entitySignature.empty() || renderSignature.empty() || methodName.empty()) {
            return nullptr;
        }

        auto* renderManagerClass = reinterpret_cast<Class*>(env->GetObjectClass(renderManagerObject));
        if (!renderManagerClass) {
            return nullptr;
        }

        Method* method = renderManagerClass->GetMethod(
            env,
            methodName.c_str(),
            ("(" + entitySignature + ")" + renderSignature).c_str());
        jobject renderObject = method ? method->CallObjectMethod(env, renderManagerObject, false, entityObject) : nullptr;
        env->DeleteLocalRef(reinterpret_cast<jclass>(renderManagerClass));
        return renderObject;
    }

    void RenderEntityWithGameRenderer(
        JNIEnv* env,
        jobject renderObject,
        jobject entityObject,
        double renderX,
        double renderY,
        double renderZ,
        float entityYaw,
        float partialTicks) {
        if (!env || !renderObject || !entityObject) {
            return;
        }

        const std::string entitySignature = Mapper::Get("net/minecraft/entity/Entity", 2);
        const std::string methodName = Mapper::Get("doRender");
        if (entitySignature.empty() || methodName.empty()) {
            return;
        }

        auto* renderClass = reinterpret_cast<Class*>(env->GetObjectClass(renderObject));
        if (!renderClass) {
            return;
        }

        Method* method = renderClass->GetMethod(
            env,
            methodName.c_str(),
            ("(" + entitySignature + "DDDFF)V").c_str());
        if (method) {
            method->CallVoidMethod(
                env,
                renderObject,
                false,
                entityObject,
                renderX,
                renderY,
                renderZ,
                entityYaw,
                partialTicks);
        }

        env->DeleteLocalRef(reinterpret_cast<jclass>(renderClass));
    }

    void ApplySolidColor(const float color[4]) {
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_LIGHTING);
        glColor4f(color[0], color[1], color[2], color[3]);
    }

}

void PlayerChams::RenderOverlay(ImDrawList* drawList, float screenW, float screenH) {
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

    jobject currentScreenObject = Minecraft::GetCurrentScreen(env);
    if (currentScreenObject) {
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

    auto* world = reinterpret_cast<World*>(worldObject);
    const auto players = world->GetPlayerEntities(env);
    std::vector<PlayerRenderEntry> renderEntries;
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
        const float previousYaw = player->GetPrevRotationYaw(env);
        const float currentYaw = player->GetRotationYaw(env);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(playerObject);
            continue;
        }

        renderEntries.push_back({
            playerObject,
            previousPos.x + ((currentPos.x - previousPos.x) * partialTicks),
            previousPos.y + ((currentPos.y - previousPos.y) * partialTicks),
            previousPos.z + ((currentPos.z - previousPos.z) * partialTicks),
            previousYaw + ((currentYaw - previousYaw) * partialTicks)
        });
    }

    if (renderEntries.empty()) {
        env->PopLocalFrame(nullptr);
        return;
    }

    jobject renderManagerObject = Minecraft::GetRenderManager(env);
    if (!renderManagerObject) {
        for (const auto& entry : renderEntries) {
            if (entry.entity) {
                env->DeleteLocalRef(entry.entity);
            }
        }
        env->PopLocalFrame(nullptr);
        return;
    }

    const float fallbackOccludedColor[4] = { 1.0f, 0.18f, 0.18f, 0.88f };
    const float fallbackVisibleColor[4] = { 0.28f, 0.92f, 0.45f, 0.88f };
    const float* occludedColor = GetOccludedColor() ? GetOccludedColor()->colorValue : fallbackOccludedColor;
    const float* visibleColor = GetVisibleColor() ? GetVisibleColor()->colorValue : fallbackVisibleColor;
    const int mode = GetMode();
    const bool colorMode = mode == kModeVisibleInvisible;

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT | GL_POLYGON_BIT | GL_TEXTURE_BIT);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(snapshot.projection.data());

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(snapshot.modelView.data());

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    if (colorMode) {
        glDisable(GL_DEPTH_TEST);
        for (const auto& entry : renderEntries) {
            jobject renderObject = GetEntityRenderObject(env, renderManagerObject, entry.entity);
            if (!renderObject) {
                continue;
            }

            glPushMatrix();
            glTranslated(entry.renderX, entry.renderY, entry.renderZ);
            ApplySolidColor(occludedColor);
            RenderEntityWithGameRenderer(env, renderObject, entry.entity, 0.0, 0.0, 0.0, entry.entityYaw, partialTicks);
            glPopMatrix();
            env->DeleteLocalRef(renderObject);
        }

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        for (const auto& entry : renderEntries) {
            jobject renderObject = GetEntityRenderObject(env, renderManagerObject, entry.entity);
            if (!renderObject) {
                continue;
            }

            glPushMatrix();
            glTranslated(entry.renderX, entry.renderY, entry.renderZ);
            ApplySolidColor(visibleColor);
            RenderEntityWithGameRenderer(env, renderObject, entry.entity, 0.0, 0.0, 0.0, entry.entityYaw, partialTicks);
            glPopMatrix();
            env->DeleteLocalRef(renderObject);
        }
    } else {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_TEXTURE_2D);
        for (const auto& entry : renderEntries) {
            jobject renderObject = GetEntityRenderObject(env, renderManagerObject, entry.entity);
            if (!renderObject) {
                continue;
            }

            glPushMatrix();
            glTranslated(entry.renderX, entry.renderY, entry.renderZ);
            glColor4f(1.0f, 1.0f, 1.0f, 0.92f);
            RenderEntityWithGameRenderer(env, renderObject, entry.entity, 0.0, 0.0, 0.0, entry.entityYaw, partialTicks);
            glPopMatrix();
            env->DeleteLocalRef(renderObject);
        }
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);
    glDisable(GL_LINE_SMOOTH);
    glLineWidth(1.0f);
    glDisable(GL_BLEND);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();

    env->DeleteLocalRef(renderManagerObject);
    for (const auto& entry : renderEntries) {
        if (entry.entity) {
            env->DeleteLocalRef(entry.entity);
        }
    }

    MarkInUse(120);
    env->PopLocalFrame(nullptr);
}
