#include "pch.h"
#include "Nametags.h"

#include "BlockVisuals.h"
#include "Target.h"

#include "../../core/Bridge.h"
#include "../../game/classes/ItemStack.h"
#include "../../game/classes/Minecraft.h"
#include "../../game/classes/Player.h"
#include "../../game/classes/RenderHelper.h"
#include "../../game/classes/RenderItem.h"
#include "../../game/classes/Scoreboard.h"
#include "../../game/classes/Timer.h"
#include "../../game/classes/World.h"
#include "../../game/jni/Class.h"
#include "../../game/jni/GameInstance.h"
#include "../../game/jni/Method.h"
#include "../../game/mapping/Mapper.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <gl/GL.h>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
    constexpr float kBaseFontSize        = 15.0f;
    constexpr float kShadowOffset        = 1.0f;
    constexpr float kAnchorVerticalOffset = 0.2f;

    struct ProjectedNametagBox {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
    };

    struct ColoredTextPart {
        std::string text;
        ImU32 color = IM_COL32_WHITE;
    };

    struct NametagEntry {
        std::string name;
        float health    = 0.0f;
        float maxHealth = 20.0f;
        float absorption = 0.0f;
        float distance  = 0.0f;
        float width     = 0.6f;
        float height    = 1.8f;
        Vec3D currentPos{};
        Vec3D lastPos{};
        bool  isPlayer  = false;
        std::vector<jobject> equipmentRefs; // raw ptrs into Nametags::m_FrameItemGlobalRefs
    };

    bool IsInstanceOfMapped(JNIEnv* env, jobject object, const char* mappingKey) {
        if (!env || !object || !g_Game || !g_Game->IsInitialized() || !mappingKey) return false;
        const std::string className = Mapper::Get(mappingKey);
        if (className.empty()) return false;
        jclass mappedClass = reinterpret_cast<jclass>(g_Game->FindClass(className));
        return mappedClass && env->IsInstanceOf(object, mappedClass) == JNI_TRUE;
    }

    bool HasZeroedBoundingBox(Player* entity, JNIEnv* env) {
        return !entity || !env || entity->HasZeroedBoundingBox(env);
    }

    float ResolveNametagHealth(Player* player, JNIEnv* env) {
        if (!player || !env) return -1.0f;
        float realHealth = player->GetRealHealth(env);
        if (!std::isfinite(realHealth) || realHealth <= 0.0f)
            realHealth = player->GetHealth(env);
        return std::isfinite(realHealth) ? realHealth : -1.0f;
    }

    bool TryGetAbsorptionAmount(Player* player, JNIEnv* env, float& amount) {
        amount = 0.0f;
        if (!player || !env) return false;

        auto* playerClass = reinterpret_cast<Class*>(env->GetObjectClass(reinterpret_cast<jobject>(player)));
        if (!playerClass) return false;

        Method* method = playerClass->GetMethod(env, "getAbsorptionAmount", "()F");
        if (!method)
            method = playerClass->GetMethod(env, "func_110139_bj", "()F");

        amount = method ? method->CallFloatMethod(env, player) : 0.0f;
        env->DeleteLocalRef(reinterpret_cast<jclass>(playerClass));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            amount = 0.0f;
            return false;
        }

        if (!std::isfinite(amount) || amount < 0.0f) {
            amount = 0.0f;
            return false;
        }
        return amount > 0.0f;
    }

    bool TryBuildProjectedNametagBox(
        double entityX, double entityY, double entityZ,
        float entityWidth, float entityHeight,
        const BlockVisuals::RenderMatrixSnapshot& snapshot,
        ProjectedNametagBox& projectedBox)
    {
        if (!snapshot.IsValid() ||
            !std::isfinite(entityX) || !std::isfinite(entityY) || !std::isfinite(entityZ) ||
            !std::isfinite(entityWidth) || !std::isfinite(entityHeight) ||
            entityWidth <= 0.05f || entityHeight <= 0.05f) {
            return false;
        }

        const float halfWidth = entityWidth * 0.5f;
        const std::array<BlockVisuals::WorldPoint, 8> corners = {{
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY),               static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY),               static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY),               static_cast<float>(entityZ + halfWidth) },
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY),               static_cast<float>(entityZ + halfWidth) },
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ + halfWidth) },
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ + halfWidth) }
        }};

        int visibleCorners = 0;
        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;

        for (const auto& corner : corners) {
            BlockVisuals::ScreenPoint sp;
            if (!BlockVisuals::TryProjectPoint(corner, snapshot, sp)) continue;
            minX = (std::min)(minX, sp.x);
            minY = (std::min)(minY, sp.y);
            maxX = (std::max)(maxX, sp.x);
            maxY = (std::max)(maxY, sp.y);
            ++visibleCorners;
        }

        if (visibleCorners == 0 || maxX <= minX || maxY <= minY) return false;

        projectedBox = { minX, minY, maxX, maxY };
        return true;
    }

    std::string NormalizeTargetName(std::string name) {
        const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
        const auto begin = std::find_if(name.begin(), name.end(), notSpace);
        if (begin == name.end()) return {};
        const auto end = std::find_if(name.rbegin(), name.rend(), notSpace).base();
        std::string normalized(begin, end);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return normalized;
    }

    bool TargetNamesMatch(const std::string& left, const std::string& right) {
        return !left.empty() && !right.empty() && NormalizeTargetName(left) == NormalizeTargetName(right);
    }

    std::string GetNametagTargetName() {
        auto* bridge = Bridge::Get();
        auto* config = bridge ? bridge->GetConfig() : nullptr;
        if (!config || !config->Target.m_Enabled) return {};
        if (config->Target.m_AutoTarget || config->Target.m_TargetSwitch)
            return Target::GetCurrentTargetName();
        return config->Target.m_PlayerName;
    }

    bool ShouldRenderOnlyTargetNametag() {
        auto* bridge = Bridge::Get();
        auto* config = bridge ? bridge->GetConfig() : nullptr;
        return config && config->Target.m_Enabled && !GetNametagTargetName().empty();
    }

    std::string StripFormatting(const std::string& text) {
        std::string clean;
        clean.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == 0xA7 || c == 0xC2) {
                if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA7)
                    ++i;
                if (i + 1 < text.size()) ++i;
                continue;
            }
            clean.push_back(text[i]);
        }
        return clean;
    }

    ImU32 BuildHealthColor(float realHealth, float maxHealth) {
        float safeMax = maxHealth <= 0.0f ? 20.0f : maxHealth;
        if (realHealth > safeMax) safeMax = realHealth;
        const float ratio = safeMax > 0.0f ? realHealth / safeMax : 0.0f;
        if (ratio >= 0.70f) return IM_COL32(91, 214, 97, 255);
        if (ratio >= 0.45f) return IM_COL32(255, 215, 92, 255);
        if (ratio >= 0.20f) return IM_COL32(255, 165, 80, 255);
        return IM_COL32(255, 82, 82, 255);
    }

    ImU32 GetMinecraftCodeColor(char code, ImU32 fallback) {
        switch (static_cast<char>(std::tolower(static_cast<unsigned char>(code)))) {
        case '0': return IM_COL32(0, 0, 0, 255);
        case '1': return IM_COL32(0, 0, 170, 255);
        case '2': return IM_COL32(0, 170, 0, 255);
        case '3': return IM_COL32(0, 170, 170, 255);
        case '4': return IM_COL32(170, 0, 0, 255);
        case '5': return IM_COL32(170, 0, 170, 255);
        case '6': return IM_COL32(255, 170, 0, 255);
        case '7': return IM_COL32(170, 170, 170, 255);
        case '8': return IM_COL32(85, 85, 85, 255);
        case '9': return IM_COL32(85, 85, 255, 255);
        case 'a': return IM_COL32(85, 255, 85, 255);
        case 'b': return IM_COL32(85, 255, 255, 255);
        case 'c': return IM_COL32(255, 85, 85, 255);
        case 'd': return IM_COL32(255, 85, 255, 255);
        case 'e': return IM_COL32(255, 255, 85, 255);
        case 'f': return IM_COL32(255, 255, 255, 255);
        case 'r': return fallback;
        default:  return fallback;
        }
    }

    std::vector<ColoredTextPart> ParseColoredText(const std::string& formattedText, ImU32 defaultColor) {
        std::vector<ColoredTextPart> parts;
        std::string current;
        ImU32 currentColor = defaultColor;

        auto flush = [&]() {
            if (!current.empty()) {
                parts.push_back({ current, currentColor });
                current.clear();
            }
        };

        for (size_t i = 0; i < formattedText.size(); ++i) {
            unsigned char b = static_cast<unsigned char>(formattedText[i]);
            if (b == 0xC2 && i + 1 < formattedText.size() &&
                static_cast<unsigned char>(formattedText[i + 1]) == 0xA7) {
                if (i + 2 < formattedText.size()) {
                    flush();
                    currentColor = GetMinecraftCodeColor(formattedText[i + 2], defaultColor);
                    i += 2;
                    continue;
                }
            }
            if (b == 0xA7 && i + 1 < formattedText.size()) {
                flush();
                currentColor = GetMinecraftCodeColor(formattedText[i + 1], defaultColor);
                ++i;
                continue;
            }
            current.push_back(formattedText[i]);
        }
        flush();
        return parts;
    }

    ImVec2 MeasureColoredText(ImFont* font, float fontSize, const std::vector<ColoredTextPart>& parts) {
        ImVec2 size(0.0f, 0.0f);
        if (!font) return size;
        for (const auto& part : parts) {
            if (part.text.empty()) continue;
            const ImVec2 ps = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, part.text.c_str());
            size.x += ps.x;
            size.y = (std::max)(size.y, ps.y);
        }
        return size;
    }

    void DrawColoredText(ImDrawList* dl, ImFont* font, float fontSize, ImVec2 pos, const std::vector<ColoredTextPart>& parts) {
        if (!dl || !font) return;
        float curX = pos.x;
        for (const auto& part : parts) {
            if (part.text.empty()) continue;
            dl->AddText(font, fontSize, ImVec2(curX + kShadowOffset, pos.y + kShadowOffset), IM_COL32(0, 0, 0, 180), part.text.c_str());
            dl->AddText(font, fontSize, ImVec2(curX, pos.y), part.color, part.text.c_str());
            curX += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, part.text.c_str()).x;
        }
    }
}

ImFont* Nametags::s_SanFranciscoBoldFont = nullptr;

void Nametags::SetFont(ImFont* font) {
    s_SanFranciscoBoldFont = font;
}

// ── ItemIconCallback ──────────────────────────────────────────────────────────

void Nametags::ItemIconCallback(const ImDrawList* /*drawList*/, const ImDrawCmd* cmd) {
    auto* self = static_cast<Nametags*>(cmd->UserCallbackData);
    if (!self || self->m_FrameIconDraws.empty()) return;
    if (!g_Game || !g_Game->IsInitialized()) return;

    JNIEnv* env = g_Game->GetCurrentEnv();
    if (!env) return;

    jobject renderItemObject = Minecraft::GetRenderItem(env);
    if (!renderItemObject) return;
    auto* renderItem = reinterpret_cast<RenderItem*>(renderItemObject);

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);

    // Snapshot entry stack depths before any changes
    GLint entryProjDepth, entryMVDepth;
    glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &entryProjDepth);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH,  &entryMVDepth);

    // Save matrices without touching the stack
    GLfloat savedProj[16], savedMV[16];
    glGetFloatv(GL_PROJECTION_MATRIX, savedProj);
    glGetFloatv(GL_MODELVIEW_MATRIX,  savedMV);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(viewport[2]), static_cast<double>(viewport[3]),
            0.0, -1000.0, 1000.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    RenderHelper::EnableGUIStandardItemLighting(env);

    if (env->PushLocalFrame(static_cast<jint>(self->m_FrameIconDraws.size() * 4 + 8)) == 0) {
        for (const auto& draw : self->m_FrameIconDraws) {
            if (!draw.itemStackGlobalRef) continue;

            const float mcScale = draw.size / 16.0f;

            GLint projBefore, mvBefore;
            glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projBefore);
            glGetIntegerv(GL_MODELVIEW_STACK_DEPTH,  &mvBefore);

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glTranslatef(draw.x, draw.y, 0.0f);
            glScalef(mcScale, mcScale, 1.0f);
            renderItem->RenderItemIntoGUI(draw.itemStackGlobalRef, 0, 0, env);
            if (env->ExceptionCheck()) env->ExceptionClear();

            GLint projAfter, mvAfter;
            glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projAfter);
            glGetIntegerv(GL_MODELVIEW_STACK_DEPTH,  &mvAfter);

            if (projAfter > projBefore) {
                glMatrixMode(GL_PROJECTION);
                for (GLint i = projAfter; i > projBefore; --i) glPopMatrix();
            }
            glMatrixMode(GL_MODELVIEW);
            for (GLint i = mvAfter; i > mvBefore; --i) glPopMatrix();
        }
        env->PopLocalFrame(nullptr);
    }

    RenderHelper::DisableStandardItemLighting(env);

    // Final drain: bring both stacks back to entry depths
    GLint finalProj, finalMV;
    glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &finalProj);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH,  &finalMV);
    if (finalProj > entryProjDepth) {
        glMatrixMode(GL_PROJECTION);
        for (GLint i = finalProj; i > entryProjDepth; --i) glPopMatrix();
    }
    if (finalMV > entryMVDepth) {
        glMatrixMode(GL_MODELVIEW);
        for (GLint i = finalMV; i > entryMVDepth; --i) glPopMatrix();
    }

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(savedProj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(savedMV);
    glPopAttrib();
}

// ── ShutdownRuntime ───────────────────────────────────────────────────────────

void Nametags::ShutdownRuntime(void* /*envPtr*/) {
    JNIEnv* env = g_Game ? g_Game->GetCurrentEnv() : nullptr;
    auto releaseVec = [&](std::vector<jobject>& v) {
        if (env) {
            for (jobject ref : v) {
                if (ref) env->DeleteGlobalRef(ref);
            }
        }
        v.clear();
    };
    releaseVec(m_FrameItemGlobalRefs);
    releaseVec(m_PrevFrameItemGlobalRefs);
    m_FrameIconDraws.clear();
}

// ── RenderOverlay ─────────────────────────────────────────────────────────────

void Nametags::RenderOverlay(ImDrawList* drawList, float screenW, float screenH) {
    if (!IsEnabled() || !drawList || !g_Game || !g_Game->IsInitialized()) return;

    // Release the previous frame's item global refs (they've been consumed by last frame's GL callback)
    if (g_Game) {
        JNIEnv* envClean = g_Game->GetCurrentEnv();
        if (envClean) {
            for (jobject ref : m_PrevFrameItemGlobalRefs) {
                if (ref) envClean->DeleteGlobalRef(ref);
            }
        }
    }
    m_PrevFrameItemGlobalRefs = std::move(m_FrameItemGlobalRefs);
    m_FrameItemGlobalRefs.clear();
    m_FrameIconDraws.clear();

    JNIEnv* env = g_Game->GetCurrentEnv();
    if (!env || env->PushLocalFrame(512) != 0) return;

    jobject localPlayerObject = Minecraft::GetThePlayer(env);
    jobject worldObject       = Minecraft::GetTheWorld(env);
    jobject timerObject       = Minecraft::GetTimer(env);
    if (!localPlayerObject || !worldObject || !timerObject) {
        env->PopLocalFrame(nullptr);
        return;
    }

    auto* localPlayer = reinterpret_cast<Player*>(localPlayerObject);
    auto* world       = reinterpret_cast<World*>(worldObject);
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

    jobject scoreboardObject = world->GetScoreboard(env);
    auto* scoreboard = reinterpret_cast<Scoreboard*>(scoreboardObject);

    const bool renderOnlyTarget = ShouldRenderOnlyTargetNametag();
    const std::string targetName = renderOnlyTarget ? GetNametagTargetName() : std::string{};
    const float maxDist = static_cast<float>(GetMaxDistance());
    const bool collectEquipment = IsEquipmentEnabled();

    std::vector<NametagEntry> entries;
    entries.reserve(64);

    // ── Collect player entries ────────────────────────────────────────────────
    const auto players = world->GetPlayerEntities(env);
    for (auto* player : players) {
        if (!player) continue;

        jobject playerObject = reinterpret_cast<jobject>(player);
        if (env->IsSameObject(playerObject, localPlayerObject)) {
            env->DeleteLocalRef(playerObject);
            continue;
        }

        const std::string plainName = player->GetName(env, true);
        if (renderOnlyTarget && !TargetNamesMatch(plainName, targetName)) {
            env->DeleteLocalRef(playerObject);
            continue;
        }

        const float health = ResolveNametagHealth(player, env);
        const float distance = localPlayer->GetDistanceToEntity(playerObject, env);
        if (plainName.empty() || health <= 0.0f ||
            !std::isfinite(distance) || distance <= 0.0f ||
            distance > maxDist || HasZeroedBoundingBox(player, env)) {
            env->DeleteLocalRef(playerObject);
            continue;
        }

        float maxHealth = player->GetMaxHealth(env);
        if (!std::isfinite(maxHealth) || maxHealth <= 0.0f)
            maxHealth = (std::max)(20.0f, health);

        float absorption = 0.0f;
        if (IsAbsorptionEnabled())
            TryGetAbsorptionAmount(player, env, absorption);

        std::string formattedName = player->GetFormattedDisplayName(env);
        if (formattedName.empty()) formattedName = plainName;

        const std::string clanTag = scoreboard ? player->GetFormattedClanTag(env, scoreboard) : std::string{};
        if (!clanTag.empty() &&
            StripFormatting(formattedName).find(StripFormatting(clanTag)) == std::string::npos) {
            formattedName += " ";
            formattedName += clanTag;
        }

        // Collect equipment item global refs
        std::vector<jobject> equipRefs;
        if (collectEquipment) {
            jobject held = player->GetHeldItem(env);
            if (held) {
                jobject gr = env->NewGlobalRef(held);
                env->DeleteLocalRef(held);
                if (gr) { m_FrameItemGlobalRefs.push_back(gr); equipRefs.push_back(gr); }
            }
            for (int slot = 3; slot >= 0; --slot) {
                jobject armor = player->GetCurrentArmor(slot, env);
                if (!armor) continue;
                jobject gr = env->NewGlobalRef(armor);
                env->DeleteLocalRef(armor);
                if (gr) { m_FrameItemGlobalRefs.push_back(gr); equipRefs.push_back(gr); }
            }
        }

        NametagEntry entry;
        entry.name        = std::move(formattedName);
        entry.health      = health;
        entry.maxHealth   = maxHealth;
        entry.absorption  = absorption;
        entry.distance    = distance;
        entry.width       = (std::max)(0.6f, player->GetWidth(env));
        entry.height      = (std::max)(1.8f, player->GetHeight(env));
        entry.currentPos  = player->GetPos(env);
        entry.lastPos     = player->GetLastTickPos(env);
        entry.isPlayer    = true;
        entry.equipmentRefs = std::move(equipRefs);
        entries.push_back(std::move(entry));

        env->DeleteLocalRef(playerObject);
    }

    // ── Collect mob / animal entries ──────────────────────────────────────────
    if (IsMobsEnabled() || IsAnimalsEnabled()) {
        const auto entities = world->GetLoadedEntities(env);
        for (jobject entityObject : entities) {
            if (!entityObject || env->IsSameObject(entityObject, localPlayerObject)) {
                if (entityObject) env->DeleteLocalRef(entityObject);
                continue;
            }

            const bool isPlayerEntity = IsInstanceOfMapped(env, entityObject, "net/minecraft/entity/player/EntityPlayer");
            const bool isMob   = IsMobsEnabled()    && IsInstanceOfMapped(env, entityObject, "net/minecraft/entity/monster/EntityMob");
            const bool isAnimal = IsAnimalsEnabled() && IsInstanceOfMapped(env, entityObject, "net/minecraft/entity/passive/EntityAnimal");
            if (isPlayerEntity || (!isMob && !isAnimal)) {
                env->DeleteLocalRef(entityObject);
                continue;
            }

            auto* entity = reinterpret_cast<Player*>(entityObject);
            const float health = ResolveNametagHealth(entity, env);
            const float distance = localPlayer->GetDistanceToEntity(entityObject, env);
            if (health <= 0.0f || !std::isfinite(distance) ||
                distance <= 0.0f || distance > maxDist || HasZeroedBoundingBox(entity, env)) {
                env->DeleteLocalRef(entityObject);
                continue;
            }

            float maxHealth = entity->GetMaxHealth(env);
            if (!std::isfinite(maxHealth) || maxHealth <= 0.0f)
                maxHealth = (std::max)(20.0f, health);

            const std::string entityName = entity->GetName(env, true);
            if (entityName.empty()) {
                env->DeleteLocalRef(entityObject);
                continue;
            }

            NametagEntry entry;
            entry.name       = entityName;
            entry.health     = health;
            entry.maxHealth  = maxHealth;
            entry.distance   = distance;
            entry.width      = (std::max)(0.6f, entity->GetWidth(env));
            entry.height     = (std::max)(1.8f, entity->GetHeight(env));
            entry.currentPos = entity->GetPos(env);
            entry.lastPos    = entity->GetLastTickPos(env);
            entry.isPlayer   = false;
            entries.push_back(std::move(entry));

            env->DeleteLocalRef(entityObject);
        }
    }

    if (entries.empty()) {
        if (scoreboardObject) env->DeleteLocalRef(scoreboardObject);
        env->PopLocalFrame(nullptr);
        return;
    }

    std::sort(entries.begin(), entries.end(), [](const NametagEntry& a, const NametagEntry& b) {
        return a.distance > b.distance;
    });

    // ── PlayerESP wire-box GL pass (if toggle on) ─────────────────────────────
    if (IsPlayerESPEnabled()) {
        glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                     GL_LINE_BIT | GL_POLYGON_BIT | GL_TEXTURE_BIT | GL_LIGHTING_BIT);
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
        glLineWidth(1.5f);
        glColor4f(0.28f, 0.92f, 0.45f, 0.88f);

        for (const auto& entry : entries) {
            if (!entry.isPlayer) continue;
            const double ix = entry.lastPos.x + (entry.currentPos.x - entry.lastPos.x) * partialTicks;
            const double iy = entry.lastPos.y + (entry.currentPos.y - entry.lastPos.y) * partialTicks;
            const double iz = entry.lastPos.z + (entry.currentPos.z - entry.lastPos.z) * partialTicks;
            const double hw = static_cast<double>(entry.width) * 0.5;
            BlockVisuals::DrawWireBox(ix - hw, iy, iz - hw, ix + hw, iy + entry.height, iz + hw);
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
    }

    // ── ImGui nametag pass ────────────────────────────────────────────────────
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (!bg) bg = drawList;

    ImFont* tagFont = s_SanFranciscoBoldFont ? s_SanFranciscoBoldFont : ImGui::GetFont();
    const float manualScale = GetScale();
    const bool  autoScale   = IsAutoScaleEnabled();
    bool renderedAny = false;

    for (const auto& entry : entries) {
        const double ix = entry.lastPos.x + (entry.currentPos.x - entry.lastPos.x) * partialTicks;
        const double iy = entry.lastPos.y + (entry.currentPos.y - entry.lastPos.y) * partialTicks;
        const double iz = entry.lastPos.z + (entry.currentPos.z - entry.lastPos.z) * partialTicks;

        ProjectedNametagBox box;
        if (!TryBuildProjectedNametagBox(ix, iy, iz, entry.width, entry.height, snapshot, box)) continue;

        // Scale: auto scales by distance, manual is fixed; both multiplied by slider
        float fontScale;
        if (autoScale)
            fontScale = (std::max)(0.5f, (std::min)(1.5f, 1.0f - entry.distance * 0.005f)) * manualScale;
        else
            fontScale = manualScale;

        const float fontSize  = kBaseFontSize * fontScale;
        const float iconSize  = 16.0f * fontScale;
        const float iconPad   = 2.0f  * fontScale;
        const float barW      = 3.0f  * fontScale;
        const float barGap    = 3.0f;
        const float iconsGap  = 4.0f  * fontScale;

        // Name line
        std::vector<ColoredTextPart> lineParts = ParseColoredText(
            entry.name,
            entry.isPlayer ? IM_COL32(255, 214, 88, 255) : IM_COL32(238, 238, 238, 255));

        if (IsHealthEnabled()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " %.1f", entry.health);
            lineParts.push_back({ buf, BuildHealthColor(entry.health, entry.maxHealth) });
        }
        if (IsAbsorptionEnabled() && entry.absorption > 0.0f) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), " +%.1f", entry.absorption);
            lineParts.push_back({ buf, IM_COL32(255, 214, 88, 255) });
        }
        if (IsDistanceEnabled()) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), " %dm", static_cast<int>(std::round(entry.distance)));
            lineParts.push_back({ buf, IM_COL32(166, 172, 180, 255) });
        }

        const ImVec2 lineSize = MeasureColoredText(tagFont, fontSize, lineParts);

        // Equipment icons row
        const int    numIcons  = static_cast<int>(entry.equipmentRefs.size());
        const float  iconsRowW = numIcons > 0
            ? (numIcons * iconSize + (std::max)(0, numIcons - 1) * iconPad)
            : 0.0f;
        const bool   showIcons = collectEquipment && numIcons > 0;

        const float contentW = (std::max)(lineSize.x, iconsRowW);

        // Anchor
        const float anchorX = std::round((box.minX + box.maxX) * 0.5f);
        const float anchorY = std::round(box.minY - (2.0f + kAnchorVerticalOffset * fontScale));

        // Row positions
        const float nameTopY  = std::round(anchorY - lineSize.y);
        const float iconsTopY = showIcons ? std::round(anchorY + iconsGap) : anchorY;
        const float iconsBotY = showIcons ? std::round(iconsTopY + iconSize) : anchorY;
        const float contentTopY = nameTopY;
        const float contentBotY = iconsBotY;

        const ImVec2 linePos(std::round(anchorX - lineSize.x * 0.5f), nameTopY);

        if (linePos.x < -80.0f || linePos.y < -80.0f ||
            linePos.x > screenW + 80.0f || linePos.y > screenH + 80.0f) continue;

        DrawColoredText(bg, tagFont, fontSize, linePos, lineParts);

        // Vertical health bar (right side)
        if (IsGraphicalHealthEnabled()) {
            const float absorpH     = IsAbsorptionEnabled() ? entry.absorption : 0.0f;
            const float effectiveMax = (std::max)(entry.maxHealth + absorpH, 1.0f);
            const float totalHP      = (std::max)(0.0f, entry.health + absorpH);
            const float ratio        = (std::max)(0.0f, (std::min)(1.0f, totalHP / effectiveMax));

            const float minBarH = 8.0f * fontScale;
            const float barLeft = std::round(anchorX + contentW * 0.5f + barGap);
            const ImVec2 barMin(barLeft,     contentTopY);
            const ImVec2 barMax(barLeft + barW,
                                contentBotY > contentTopY + minBarH ? contentBotY : contentTopY + minBarH);
            const float  barH  = barMax.y - barMin.y;

            // Fill from top downward (high health = more of the bar filled)
            const ImVec2 fillMax(barMax.x, barMin.y + barH * ratio);

            bg->AddRectFilled(barMin, barMax, IM_COL32(0, 0, 0, 160), 1.5f);
            bg->AddRectFilled(barMin, fillMax, BuildHealthColor(entry.health, entry.maxHealth), 1.5f);

            if (absorpH > 0.0f && effectiveMax > 0.0f) {
                const float baseRatio = (std::max)(0.0f, (std::min)(1.0f, entry.health / effectiveMax));
                if (ratio > baseRatio) {
                    bg->AddRectFilled(
                        ImVec2(barMin.x, barMin.y + barH * baseRatio),
                        ImVec2(barMax.x, barMin.y + barH * ratio),
                        IM_COL32(255, 214, 88, 255), 1.5f);
                }
            }
        }

        // Equipment icon slot boxes + queue icon draws
        if (showIcons) {
            const float startX = std::round(anchorX - iconsRowW * 0.5f);
            for (int ci = 0; ci < numIcons; ++ci) {
                const float ix2 = startX + ci * (iconSize + iconPad);
                const ImVec2 sMin(ix2,            iconsTopY);
                const ImVec2 sMax(ix2 + iconSize, iconsTopY + iconSize);
                bg->AddRectFilled(sMin, sMax, IM_COL32(20, 20, 24, 200), 2.0f);
                bg->AddRect      (sMin, sMax, IM_COL32(255, 255, 255, 30), 2.0f, 0, 1.0f);
                m_FrameIconDraws.push_back({ entry.equipmentRefs[ci], ix2, iconsTopY, iconSize });
            }
        }

        renderedAny = true;
    }

    // GL callback renders equipment icons on top of slot boxes
    if (!m_FrameIconDraws.empty()) {
        bg->AddCallback(ItemIconCallback, this);
        bg->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    if (renderedAny) MarkInUse(120);

    if (scoreboardObject) env->DeleteLocalRef(scoreboardObject);
    env->PopLocalFrame(nullptr);
}
