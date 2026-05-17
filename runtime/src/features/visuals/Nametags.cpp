#include "pch.h"
#include "Nametags.h"

#include "BlockVisuals.h"
#include "Target.h"

#include "../../core/Bridge.h"
#include "../../game/classes/ItemStack.h"
#include "../../game/classes/Minecraft.h"
#include "../../game/classes/Player.h"
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
#include <string>
#include <unordered_set>
#include <vector>

namespace {
    constexpr float kBaseFontSize = 15.0f;
    constexpr float kEquipmentFontSize = 11.5f;
    constexpr float kShadowOffset = 1.0f;
    constexpr float kBarWidth = 28.0f;
    constexpr float kBarHeight = 2.5f;
    constexpr float kMaximumRenderableDistance = 255.0f;
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
        std::string equipment;
        float health = 0.0f;
        float maxHealth = 20.0f;
        float absorption = 0.0f;
        float distance = 0.0f;
        float width = 0.6f;
        float height = 1.8f;
        Vec3D currentPos{};
        Vec3D lastPos{};
        bool isPlayer = false;
    };

    void ClearJniException(JNIEnv* env) {
        if (env && env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    bool IsInstanceOfMapped(JNIEnv* env, jobject object, const char* mappingKey) {
        if (!env || !object || !g_Game || !g_Game->IsInitialized() || !mappingKey) {
            return false;
        }

        const std::string className = Mapper::Get(mappingKey);
        if (className.empty()) {
            return false;
        }

        jclass mappedClass = reinterpret_cast<jclass>(g_Game->FindClass(className));
        return mappedClass && env->IsInstanceOf(object, mappedClass) == JNI_TRUE;
    }

    bool HasZeroedBoundingBox(Player* entity, JNIEnv* env) {
        return !entity || !env || entity->HasZeroedBoundingBox(env);
    }

    float ResolveNametagHealth(Player* player, JNIEnv* env) {
        if (!player || !env) {
            return -1.0f;
        }

        float realHealth = player->GetRealHealth(env);
        if (!std::isfinite(realHealth) || realHealth <= 0.0f) {
            realHealth = player->GetHealth(env);
        }

        return std::isfinite(realHealth) ? realHealth : -1.0f;
    }

    bool TryGetAbsorptionAmount(Player* player, JNIEnv* env, float& amount) {
        amount = 0.0f;
        if (!player || !env) {
            return false;
        }

        auto* playerClass = reinterpret_cast<Class*>(env->GetObjectClass(reinterpret_cast<jobject>(player)));
        if (!playerClass) {
            return false;
        }

        Method* method = playerClass->GetMethod(env, "getAbsorptionAmount", "()F");
        if (!method) {
            method = playerClass->GetMethod(env, "func_110139_bj", "()F");
        }

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
        double entityX,
        double entityY,
        double entityZ,
        float entityWidth,
        float entityHeight,
        const BlockVisuals::RenderMatrixSnapshot& snapshot,
        ProjectedNametagBox& projectedBox) {
        if (!snapshot.IsValid() ||
            !std::isfinite(entityX) || !std::isfinite(entityY) || !std::isfinite(entityZ) ||
            !std::isfinite(entityWidth) || !std::isfinite(entityHeight) ||
            entityWidth <= 0.05f || entityHeight <= 0.05f) {
            return false;
        }

        const float halfWidth = entityWidth * 0.5f;
        const std::array<BlockVisuals::WorldPoint, 8> corners = {{
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY),                static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY),                static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY),                static_cast<float>(entityZ + halfWidth) },
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY),                static_cast<float>(entityZ + halfWidth) },
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ - halfWidth) },
            { static_cast<float>(entityX + halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ + halfWidth) },
            { static_cast<float>(entityX - halfWidth), static_cast<float>(entityY + entityHeight), static_cast<float>(entityZ + halfWidth) }
        }};

        int visibleCorners = 0;
        float minX = FLT_MAX;
        float minY = FLT_MAX;
        float maxX = -FLT_MAX;
        float maxY = -FLT_MAX;

        for (const auto& corner : corners) {
            BlockVisuals::ScreenPoint projectedCorner;
            if (!BlockVisuals::TryProjectPoint(corner, snapshot, projectedCorner)) {
                continue;
            }

            minX = (std::min)(minX, projectedCorner.x);
            minY = (std::min)(minY, projectedCorner.y);
            maxX = (std::max)(maxX, projectedCorner.x);
            maxY = (std::max)(maxY, projectedCorner.y);
            ++visibleCorners;
        }

        if (visibleCorners == 0 || maxX <= minX || maxY <= minY) {
            return false;
        }

        projectedBox.minX = minX;
        projectedBox.minY = minY;
        projectedBox.maxX = maxX;
        projectedBox.maxY = maxY;
        return true;
    }

    std::string NormalizeTargetName(std::string name) {
        const auto notSpace = [](unsigned char ch) {
            return !std::isspace(ch);
        };

        const auto begin = std::find_if(name.begin(), name.end(), notSpace);
        if (begin == name.end()) {
            return {};
        }

        const auto end = std::find_if(name.rbegin(), name.rend(), notSpace).base();
        std::string normalized(begin, end);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return normalized;
    }

    bool TargetNamesMatch(const std::string& left, const std::string& right) {
        return !left.empty() && !right.empty() && NormalizeTargetName(left) == NormalizeTargetName(right);
    }

    std::string GetNametagTargetName() {
        auto* bridge = Bridge::Get();
        auto* config = bridge ? bridge->GetConfig() : nullptr;
        if (!config || !config->Target.m_Enabled) {
            return {};
        }

        if (config->Target.m_AutoTarget || config->Target.m_TargetSwitch) {
            return Target::GetCurrentTargetName();
        }

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

        for (size_t index = 0; index < text.size(); ++index) {
            const unsigned char current = static_cast<unsigned char>(text[index]);
            if (current == 0xA7 || current == 0xC2) {
                if (current == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA7) {
                    ++index;
                }
                if (index + 1 < text.size()) {
                    ++index;
                }
                continue;
            }

            clean.push_back(text[index]);
        }

        return clean;
    }

    std::string AbbreviateEquipmentName(std::string text) {
        text = StripFormatting(text);
        std::string normalized;
        normalized.reserve(text.size());
        for (char raw : text) {
            const unsigned char ch = static_cast<unsigned char>(raw);
            if (std::isalnum(ch) || std::isspace(ch)) {
                normalized.push_back(static_cast<char>(std::tolower(ch)));
            }
        }

        std::istringstream stream(normalized);
        std::vector<std::string> words;
        std::string word;
        while (stream >> word) {
            words.push_back(word);
        }

        if (words.size() >= 2) {
            std::string label;
            for (const std::string& token : words) {
                if (!token.empty()) {
                    label.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(token.front()))));
                }
                if (label.size() >= 3) {
                    break;
                }
            }
            if (!label.empty()) {
                return label;
            }
        }

        if (!words.empty()) {
            std::string compact = words.front().substr(0, (std::min)(size_t{ 4 }, words.front().size()));
            std::transform(compact.begin(), compact.end(), compact.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
            return compact;
        }

        return {};
    }

    std::string BuildEquipmentText(Player* player, JNIEnv* env) {
        if (!player || !env) {
            return {};
        }

        std::vector<std::string> labels;
        labels.reserve(5);

        jobject heldItem = player->GetHeldItem(env);
        if (heldItem) {
            const std::string label = AbbreviateEquipmentName(reinterpret_cast<ItemStack*>(heldItem)->GetDisplayName(env));
            if (!label.empty()) {
                labels.push_back(label);
            }
            env->DeleteLocalRef(heldItem);
        }

        for (int slot = 3; slot >= 0; --slot) {
            jobject armorObject = player->GetCurrentArmor(slot, env);
            if (!armorObject) {
                continue;
            }

            const std::string label = AbbreviateEquipmentName(reinterpret_cast<ItemStack*>(armorObject)->GetDisplayName(env));
            if (!label.empty()) {
                labels.push_back(label);
            }
            env->DeleteLocalRef(armorObject);
        }

        std::string result;
        for (size_t index = 0; index < labels.size(); ++index) {
            if (index != 0) {
                result.append("  ");
            }
            result.append(labels[index]);
        }

        return result;
    }

    ImU32 BuildHealthColor(float realHealth, float maxHealth) {
        float safeMax = maxHealth <= 0.0f ? 20.0f : maxHealth;
        if (realHealth > safeMax) {
            safeMax = realHealth;
        }

        const float ratio = safeMax > 0.0f ? realHealth / safeMax : 0.0f;
        if (ratio >= 0.70f) {
            return IM_COL32(91, 214, 97, 255);
        }
        if (ratio >= 0.45f) {
            return IM_COL32(255, 215, 92, 255);
        }
        if (ratio >= 0.20f) {
            return IM_COL32(255, 165, 80, 255);
        }
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
        default: return fallback;
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

        for (size_t index = 0; index < formattedText.size(); ++index) {
            unsigned char currentByte = static_cast<unsigned char>(formattedText[index]);
            if (currentByte == 0xC2 && index + 1 < formattedText.size() &&
                static_cast<unsigned char>(formattedText[index + 1]) == 0xA7) {
                if (index + 2 < formattedText.size()) {
                    flush();
                    currentColor = GetMinecraftCodeColor(formattedText[index + 2], defaultColor);
                    index += 2;
                    continue;
                }
            }

            if (currentByte == 0xA7 && index + 1 < formattedText.size()) {
                flush();
                currentColor = GetMinecraftCodeColor(formattedText[index + 1], defaultColor);
                ++index;
                continue;
            }

            current.push_back(formattedText[index]);
        }

        flush();
        return parts;
    }

    ImVec2 MeasureColoredText(ImFont* font, float fontSize, const std::vector<ColoredTextPart>& parts) {
        ImVec2 size(0.0f, 0.0f);
        if (!font) {
            return size;
        }

        for (const auto& part : parts) {
            if (part.text.empty()) {
                continue;
            }
            const ImVec2 partSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, part.text.c_str());
            size.x += partSize.x;
            size.y = (std::max)(size.y, partSize.y);
        }

        return size;
    }

    void DrawColoredText(ImDrawList* drawList, ImFont* font, float fontSize, ImVec2 pos, const std::vector<ColoredTextPart>& parts) {
        if (!drawList || !font) {
            return;
        }

        float cursorX = pos.x;
        for (const auto& part : parts) {
            if (part.text.empty()) {
                continue;
            }

            drawList->AddText(font, fontSize, ImVec2(cursorX + kShadowOffset, pos.y + kShadowOffset), IM_COL32(0, 0, 0, 180), part.text.c_str());
            drawList->AddText(font, fontSize, ImVec2(cursorX, pos.y), part.color, part.text.c_str());
            cursorX += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, part.text.c_str()).x;
        }
    }
}

ImFont* Nametags::s_SanFranciscoBoldFont = nullptr;

void Nametags::SetFont(ImFont* font) {
    s_SanFranciscoBoldFont = font;
}

void Nametags::RenderOverlay(ImDrawList* drawList, float screenW, float screenH) {
    if (!IsEnabled() || !drawList || !g_Game || !g_Game->IsInitialized()) {
        return;
    }

    JNIEnv* env = g_Game->GetCurrentEnv();
    if (!env || env->PushLocalFrame(512) != 0) {
        return;
    }

    jobject localPlayerObject = Minecraft::GetThePlayer(env);
    jobject worldObject = Minecraft::GetTheWorld(env);
    jobject timerObject = Minecraft::GetTimer(env);
    if (!localPlayerObject || !worldObject || !timerObject) {
        env->PopLocalFrame(nullptr);
        return;
    }

    auto* localPlayer = reinterpret_cast<Player*>(localPlayerObject);
    auto* world = reinterpret_cast<World*>(worldObject);
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

    std::vector<NametagEntry> entries;
    entries.reserve(64);

    const auto players = world->GetPlayerEntities(env);
    for (auto* player : players) {
        if (!player) {
            continue;
        }

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
        if (plainName.empty() || health <= 0.0f || !std::isfinite(distance) || distance <= 0.0f || distance > kMaximumRenderableDistance || HasZeroedBoundingBox(player, env)) {
            env->DeleteLocalRef(playerObject);
            continue;
        }

        float maxHealth = player->GetMaxHealth(env);
        if (!std::isfinite(maxHealth) || maxHealth <= 0.0f) {
            maxHealth = (std::max)(20.0f, health);
        }

        float absorption = 0.0f;
        if (IsAbsorptionEnabled()) {
            TryGetAbsorptionAmount(player, env, absorption);
        }

        std::string formattedName = player->GetFormattedDisplayName(env);
        if (formattedName.empty()) {
            formattedName = plainName;
        }

        const std::string formattedClanTag = scoreboard ? player->GetFormattedClanTag(env, scoreboard) : std::string{};
        if (!formattedClanTag.empty() && StripFormatting(formattedName).find(StripFormatting(formattedClanTag)) == std::string::npos) {
            formattedName.append(" ");
            formattedName.append(formattedClanTag);
        }

        entries.push_back({
            formattedName,
            IsEquipmentEnabled() ? BuildEquipmentText(player, env) : std::string{},
            health,
            maxHealth,
            absorption,
            distance,
            (std::max)(0.6f, player->GetWidth(env)),
            (std::max)(1.8f, player->GetHeight(env)),
            player->GetPos(env),
            player->GetLastTickPos(env),
            true
        });

        env->DeleteLocalRef(playerObject);
    }

    if (IsMobsEnabled() || IsAnimalsEnabled()) {
        const auto entities = world->GetLoadedEntities(env);
        for (jobject entityObject : entities) {
            if (!entityObject || env->IsSameObject(entityObject, localPlayerObject)) {
                if (entityObject) {
                    env->DeleteLocalRef(entityObject);
                }
                continue;
            }

            const bool isPlayerEntity = IsInstanceOfMapped(env, entityObject, "net/minecraft/entity/player/EntityPlayer");
            const bool isMobEntity = IsMobsEnabled() && IsInstanceOfMapped(env, entityObject, "net/minecraft/entity/monster/EntityMob");
            const bool isAnimalEntity = IsAnimalsEnabled() && IsInstanceOfMapped(env, entityObject, "net/minecraft/entity/passive/EntityAnimal");
            if (isPlayerEntity || (!isMobEntity && !isAnimalEntity)) {
                env->DeleteLocalRef(entityObject);
                continue;
            }

            auto* entity = reinterpret_cast<Player*>(entityObject);
            const float health = ResolveNametagHealth(entity, env);
            const float distance = localPlayer->GetDistanceToEntity(entityObject, env);
            if (health <= 0.0f || !std::isfinite(distance) || distance <= 0.0f || distance > kMaximumRenderableDistance || HasZeroedBoundingBox(entity, env)) {
                env->DeleteLocalRef(entityObject);
                continue;
            }

            float maxHealth = entity->GetMaxHealth(env);
            if (!std::isfinite(maxHealth) || maxHealth <= 0.0f) {
                maxHealth = (std::max)(20.0f, health);
            }

            const std::string entityName = entity->GetName(env, true);
            if (entityName.empty()) {
                env->DeleteLocalRef(entityObject);
                continue;
            }

            entries.push_back({
                entityName,
                {},
                health,
                maxHealth,
                0.0f,
                distance,
                (std::max)(0.6f, entity->GetWidth(env)),
                (std::max)(1.8f, entity->GetHeight(env)),
                entity->GetPos(env),
                entity->GetLastTickPos(env),
                false
            });

            env->DeleteLocalRef(entityObject);
        }
    }

    if (entries.empty()) {
        if (scoreboardObject) {
            env->DeleteLocalRef(scoreboardObject);
        }
        env->PopLocalFrame(nullptr);
        return;
    }

    std::sort(entries.begin(), entries.end(), [](const NametagEntry& left, const NametagEntry& right) {
        return left.distance > right.distance;
    });

    ImDrawList* backgroundDrawList = ImGui::GetBackgroundDrawList();
    if (!backgroundDrawList) {
        backgroundDrawList = drawList;
    }

    ImFont* tagFont = s_SanFranciscoBoldFont ? s_SanFranciscoBoldFont : ImGui::GetFont();
    bool renderedAny = false;

    for (const auto& entry : entries) {
        const double interpolatedX = entry.lastPos.x + ((entry.currentPos.x - entry.lastPos.x) * partialTicks);
        const double interpolatedY = entry.lastPos.y + ((entry.currentPos.y - entry.lastPos.y) * partialTicks);
        const double interpolatedZ = entry.lastPos.z + ((entry.currentPos.z - entry.lastPos.z) * partialTicks);

        ProjectedNametagBox projectedBox;
        if (!TryBuildProjectedNametagBox(
                interpolatedX,
                interpolatedY,
                interpolatedZ,
                entry.width,
                entry.height,
                snapshot,
                projectedBox)) {
            continue;
        }

        const float fontScale = (std::max)(0.78f, (std::min)(1.08f, 1.0f - (entry.distance * 0.0125f)));
        const float fontSize = kBaseFontSize * fontScale;
        const float equipmentFontSize = kEquipmentFontSize * fontScale;

        std::vector<ColoredTextPart> lineParts = ParseColoredText(entry.name, entry.isPlayer ? IM_COL32(255, 214, 88, 255) : IM_COL32(238, 238, 238, 255));
        if (IsHealthEnabled()) {
            char healthBuffer[32];
            std::snprintf(healthBuffer, sizeof(healthBuffer), " %.1f", entry.health);
            lineParts.push_back({ healthBuffer, BuildHealthColor(entry.health, entry.maxHealth) });
        }
        if (IsAbsorptionEnabled() && entry.absorption > 0.0f) {
            char absorptionBuffer[24];
            std::snprintf(absorptionBuffer, sizeof(absorptionBuffer), " +%.1f", entry.absorption);
            lineParts.push_back({ absorptionBuffer, IM_COL32(255, 214, 88, 255) });
        }
        if (IsDistanceEnabled()) {
            char distanceBuffer[24];
            std::snprintf(distanceBuffer, sizeof(distanceBuffer), " %dm", static_cast<int>(std::round(entry.distance)));
            lineParts.push_back({ distanceBuffer, IM_COL32(166, 172, 180, 255) });
        }

        const ImVec2 lineSize = MeasureColoredText(tagFont, fontSize, lineParts);
        const ImVec2 equipmentSize = entry.equipment.empty()
            ? ImVec2(0.0f, 0.0f)
            : tagFont->CalcTextSizeA(equipmentFontSize, FLT_MAX, 0.0f, entry.equipment.c_str());

        const float anchorX = std::round((projectedBox.minX + projectedBox.maxX) * 0.5f);
        const float anchorY = std::round(projectedBox.minY - (2.0f + (kAnchorVerticalOffset * fontScale)));
        const ImVec2 linePos(
            std::round(anchorX - (lineSize.x * 0.5f)),
            std::round(anchorY - lineSize.y));

        if (linePos.x < -80.0f || linePos.y < -80.0f || linePos.x > screenW + 80.0f || linePos.y > screenH + 80.0f) {
            continue;
        }

        DrawColoredText(backgroundDrawList, tagFont, fontSize, linePos, lineParts);

        if (IsGraphicalHealthEnabled()) {
            const float absorptionHealth = IsAbsorptionEnabled() ? entry.absorption : 0.0f;
            const float effectiveMaxHealth = (std::max)(entry.maxHealth + absorptionHealth, 1.0f);
            const float totalHealth = (std::max)(0.0f, entry.health + absorptionHealth);
            const float ratio = (std::max)(0.0f, (std::min)(1.0f, totalHealth / effectiveMaxHealth));
            const float barWidth = kBarWidth * fontScale;
            const ImVec2 barMin(std::round(anchorX - (barWidth * 0.5f)), std::round(linePos.y + lineSize.y + 1.5f));
            const ImVec2 barMax(std::round(barMin.x + barWidth), std::round(barMin.y + (kBarHeight * fontScale)));
            const float fillWidth = (barMax.x - barMin.x) * ratio;

            backgroundDrawList->AddRectFilled(barMin, barMax, IM_COL32(0, 0, 0, 160), 2.0f);
            backgroundDrawList->AddRectFilled(barMin, ImVec2(barMin.x + fillWidth, barMax.y), BuildHealthColor(entry.health, entry.maxHealth), 2.0f);
            if (absorptionHealth > 0.0f && effectiveMaxHealth > 0.0f) {
                const float baseRatio = (std::max)(0.0f, (std::min)(1.0f, entry.health / effectiveMaxHealth));
                const float absorptionRatio = (std::max)(0.0f, (std::min)(1.0f, totalHealth / effectiveMaxHealth));
                backgroundDrawList->AddRectFilled(
                    ImVec2(barMin.x + ((barMax.x - barMin.x) * baseRatio), barMin.y),
                    ImVec2(barMin.x + ((barMax.x - barMin.x) * absorptionRatio), barMax.y),
                    IM_COL32(255, 214, 88, 255),
                    2.0f);
            }
        }

        if (IsEquipmentEnabled() && !entry.equipment.empty()) {
            const ImVec2 equipmentPos(
                std::round(anchorX - (equipmentSize.x * 0.5f)),
                std::round(linePos.y + lineSize.y + (IsGraphicalHealthEnabled() ? 6.0f * fontScale : 3.0f)));
            backgroundDrawList->AddText(tagFont, equipmentFontSize, ImVec2(equipmentPos.x + 1.0f, equipmentPos.y + 1.0f), IM_COL32(0, 0, 0, 170), entry.equipment.c_str());
            backgroundDrawList->AddText(tagFont, equipmentFontSize, equipmentPos, IM_COL32(214, 219, 225, 255), entry.equipment.c_str());
        }

        renderedAny = true;
    }

    if (renderedAny) {
        MarkInUse(120);
    }

    if (scoreboardObject) {
        env->DeleteLocalRef(scoreboardObject);
    }
    env->PopLocalFrame(nullptr);
}
