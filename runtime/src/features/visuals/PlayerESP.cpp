#include "pch.h"
#include "PlayerESP.h"

#include "BlockVisuals.h"
#include "../../game/classes/Minecraft.h"
#include "../../game/classes/Player.h"
#include "../../game/classes/Timer.h"
#include "../../game/classes/World.h"
#include "../../game/jni/GameInstance.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <gl/GL.h>
#include <vector>

namespace {
    struct PlayerRenderEntry {
        jobject entity  = nullptr;
        double renderX  = 0.0;
        double renderY  = 0.0;
        double renderZ  = 0.0;
        float health    = 0.0f;
        float maxHealth = 20.0f;
        float width     = 0.6f;
        float height    = 1.8f;
    };

    ImU32 GetHealthBarColor(float health, float maxHealth) {
        float safeMax = maxHealth <= 0.0f ? 20.0f : maxHealth;
        if (health > safeMax) safeMax = health;
        const float ratio = safeMax > 0.0f ? health / safeMax : 0.0f;
        if (ratio >= 0.70f) return IM_COL32(91, 214, 97, 255);
        if (ratio >= 0.45f) return IM_COL32(255, 215, 92, 255);
        if (ratio >= 0.20f) return IM_COL32(255, 165, 80, 255);
        return IM_COL32(255, 82, 82, 255);
    }
}

void PlayerESP::RenderOverlay(ImDrawList* /*drawList*/, float screenW, float screenH) {
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

    const BlockVisuals::RenderMatrixSnapshot snapshot = BlockVisuals::CaptureRenderMatrixSnapshot();
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
        if (!player) continue;

        jobject playerObject = reinterpret_cast<jobject>(player);
        if (env->IsSameObject(playerObject, localPlayerObject)) {
            env->DeleteLocalRef(playerObject);
            continue;
        }

        const float health = player->GetHealth(env);
        if (health <= 0.0f || player->HasZeroedBoundingBox(env)) {
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

        float maxHealth = player->GetMaxHealth(env);
        if (!std::isfinite(maxHealth) || maxHealth <= 0.0f) maxHealth = 20.0f;

        float width  = player->GetWidth(env);
        float height = player->GetHeight(env);
        if (width  <= 0.05f) width  = 0.6f;
        if (height <= 0.05f) height = 1.8f;

        renderEntries.push_back({
            playerObject,
            previousPos.x + (currentPos.x - previousPos.x) * partialTicks,
            previousPos.y + (currentPos.y - previousPos.y) * partialTicks,
            previousPos.z + (currentPos.z - previousPos.z) * partialTicks,
            health, maxHealth, width, height
        });
    }

    if (renderEntries.empty()) {
        env->PopLocalFrame(nullptr);
        return;
    }

    const float* color = GetColor();

    // GL wire-box pass
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
        const double hw = static_cast<double>(entry.width) * 0.5;
        BlockVisuals::DrawWireBox(
            entry.renderX - hw, entry.renderY,               entry.renderZ - hw,
            entry.renderX + hw, entry.renderY + entry.height, entry.renderZ + hw);
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

    // ImGui health bar pass — renders a vertical bar on the right side of each ESP box
    if (IsHealthBarsEnabled()) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (dl) {
            for (const auto& entry : renderEntries) {
                const float hw = entry.width * 0.5f;
                const std::array<BlockVisuals::WorldPoint, 8> corners = {{
                    { (float)(entry.renderX - hw), (float)entry.renderY,               (float)(entry.renderZ - hw) },
                    { (float)(entry.renderX + hw), (float)entry.renderY,               (float)(entry.renderZ - hw) },
                    { (float)(entry.renderX + hw), (float)entry.renderY,               (float)(entry.renderZ + hw) },
                    { (float)(entry.renderX - hw), (float)entry.renderY,               (float)(entry.renderZ + hw) },
                    { (float)(entry.renderX - hw), (float)(entry.renderY + entry.height), (float)(entry.renderZ - hw) },
                    { (float)(entry.renderX + hw), (float)(entry.renderY + entry.height), (float)(entry.renderZ - hw) },
                    { (float)(entry.renderX + hw), (float)(entry.renderY + entry.height), (float)(entry.renderZ + hw) },
                    { (float)(entry.renderX - hw), (float)(entry.renderY + entry.height), (float)(entry.renderZ + hw) }
                }};

                float minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
                int visible = 0;
                for (const auto& c : corners) {
                    BlockVisuals::ScreenPoint sp;
                    if (!BlockVisuals::TryProjectPoint(c, snapshot, sp)) continue;
                    if (sp.y < minY) minY = sp.y;
                    if (sp.x > maxX) maxX = sp.x;
                    if (sp.y > maxY) maxY = sp.y;
                    ++visible;
                }
                if (visible == 0 || maxY <= minY) continue;

                const float barW   = 3.0f;
                const float barGap = 3.0f;
                const float ratio  = (std::max)(0.0f, (std::min)(1.0f,
                    entry.maxHealth > 0.0f ? entry.health / entry.maxHealth : 0.0f));

                const float barLeft = maxX + barGap;
                const ImVec2 barMin(barLeft, minY);
                const ImVec2 barMax(barLeft + barW, maxY < minY + 8.0f ? minY + 8.0f : maxY);
                const float  barH = barMax.y - barMin.y;
                const ImVec2 fillMax(barMax.x, barMin.y + barH * ratio);

                dl->AddRectFilled(barMin, barMax, IM_COL32(0, 0, 0, 160), 1.5f);
                dl->AddRectFilled(barMin, fillMax, GetHealthBarColor(entry.health, entry.maxHealth), 1.5f);
            }
        }
    }

    for (const auto& entry : renderEntries) {
        if (entry.entity) {
            env->DeleteLocalRef(entry.entity);
        }
    }

    MarkInUse(120);
    env->PopLocalFrame(nullptr);
}
