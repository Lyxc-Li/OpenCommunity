#include "pch.h"
#include "BedPlates.h"

#include "BlockVisuals.h"

#include "../../game/classes/Minecraft.h"
#include "../../game/classes/Player.h"
#include "../../game/classes/Timer.h"
#include "../../game/jni/GameInstance.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <gl/GL.h>
#include <map>
#include <sstream>

namespace {
    constexpr auto kBedScanInterval = std::chrono::milliseconds(700);
    constexpr size_t kMaxBedEntries = 48;

    std::chrono::steady_clock::time_point g_LastBedScanTime = std::chrono::steady_clock::time_point::min();

    bool IsFiniteVec(const Vec3D& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    std::string MakeBedKey(int x, int y, int z) {
        std::ostringstream stream;
        stream << x << ':' << y << ':' << z;
        return stream.str();
    }

    std::string BuildChipLabel(const std::string& displayName) {
        std::string normalized;
        normalized.reserve(displayName.size());
        for (char raw : displayName) {
            unsigned char ch = static_cast<unsigned char>(raw);
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
            std::string initials;
            for (const std::string& token : words) {
                if (!token.empty()) {
                    initials.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(token.front()))));
                }
                if (initials.size() >= 3) {
                    break;
                }
            }
            if (!initials.empty()) {
                return initials;
            }
        }

        if (!words.empty()) {
            std::string compact = words.front().substr(0, (std::min)(size_t{ 3 }, words.front().size()));
            std::transform(compact.begin(), compact.end(), compact.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
            return compact;
        }

        return "BLK";
    }
}

void BedPlates::TickSynchronous(void* envPtr) {
    auto* env = static_cast<JNIEnv*>(envPtr);
    if (!env) {
        return;
    }

    if (!IsEnabled()) {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        m_RenderEntries.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_LastBedScanTime != std::chrono::steady_clock::time_point::min() &&
        (now - g_LastBedScanTime) < kBedScanInterval) {
        return;
    }

    jobject worldObject = Minecraft::GetTheWorld(env);
    jobject localPlayerObject = Minecraft::GetThePlayer(env);
    if (!worldObject || !localPlayerObject) {
        if (worldObject) {
            env->DeleteLocalRef(worldObject);
        }
        if (localPlayerObject) {
            env->DeleteLocalRef(localPlayerObject);
        }
        return;
    }

    auto* localPlayer = reinterpret_cast<Player*>(localPlayerObject);
    const Vec3D localPos = localPlayer->GetPos(env);
    if (!IsFiniteVec(localPos) || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(localPlayerObject);
        env->DeleteLocalRef(worldObject);
        return;
    }

    const int range = GetRange();
    const int baseX = static_cast<int>(std::floor(localPos.x));
    const int baseY = static_cast<int>(std::floor(localPos.y));
    const int baseZ = static_cast<int>(std::floor(localPos.z));
    const int rangeSq = range * range;
    static constexpr std::array<std::array<int, 3>, 4> kHorizontalNeighbors = {{
        {{ 1, 0, 0 }},
        {{ -1, 0, 0 }},
        {{ 0, 0, 1 }},
        {{ 0, 0, -1 }}
    }};

    std::vector<BedPlateEntry> foundEntries;
    foundEntries.reserve(16);
    std::map<std::string, size_t> seenBeds;

    for (int dx = -range; dx <= range && foundEntries.size() < kMaxBedEntries; ++dx) {
        for (int dy = -6; dy <= 6 && foundEntries.size() < kMaxBedEntries; ++dy) {
            for (int dz = -range; dz <= range && foundEntries.size() < kMaxBedEntries; ++dz) {
                const int distanceSq = (dx * dx) + (dy * dy) + (dz * dz);
                if (distanceSq > rangeSq) {
                    continue;
                }

                const int blockX = baseX + dx;
                const int blockY = baseY + dy;
                const int blockZ = baseZ + dz;

                jobject blockObject = BlockVisuals::GetBlockObjectAt(env, worldObject, blockX, blockY, blockZ);
                if (!blockObject) {
                    continue;
                }

                const BlockVisuals::BlockDescriptor descriptor = BlockVisuals::DescribeBlock(env, blockObject);
                env->DeleteLocalRef(blockObject);
                if (!BlockVisuals::BlockMatchesQuery(descriptor, "bed")) {
                    continue;
                }

                int otherX = blockX;
                int otherY = blockY;
                int otherZ = blockZ;
                bool hasSecondHalf = false;

                for (const auto& offset : kHorizontalNeighbors) {
                    jobject neighborBlock = BlockVisuals::GetBlockObjectAt(env, worldObject, blockX + offset[0], blockY + offset[1], blockZ + offset[2]);
                    if (!neighborBlock) {
                        continue;
                    }

                    const BlockVisuals::BlockDescriptor neighborDescriptor = BlockVisuals::DescribeBlock(env, neighborBlock);
                    env->DeleteLocalRef(neighborBlock);
                    if (!BlockVisuals::BlockMatchesQuery(neighborDescriptor, "bed")) {
                        continue;
                    }

                    otherX = blockX + offset[0];
                    otherY = blockY + offset[1];
                    otherZ = blockZ + offset[2];
                    hasSecondHalf = true;
                    break;
                }

                int canonicalX = blockX;
                int canonicalY = blockY;
                int canonicalZ = blockZ;
                if (hasSecondHalf) {
                    if (std::tie(otherX, otherY, otherZ) < std::tie(canonicalX, canonicalY, canonicalZ)) {
                        canonicalX = otherX;
                        canonicalY = otherY;
                        canonicalZ = otherZ;
                    }
                }

                const std::string key = MakeBedKey(canonicalX, canonicalY, canonicalZ);
                if (seenBeds.find(key) != seenBeds.end()) {
                    continue;
                }

                BedPlateEntry entry;
                entry.x = canonicalX;
                entry.y = canonicalY;
                entry.z = canonicalZ;
                entry.hasSecondHalf = hasSecondHalf;
                entry.otherX = hasSecondHalf ? otherX : canonicalX;
                entry.otherY = hasSecondHalf ? otherY : canonicalY;
                entry.otherZ = hasSecondHalf ? otherZ : canonicalZ;
                entry.distance = std::sqrt(static_cast<float>(distanceSq));

                const int minX = (std::min)(entry.x, entry.otherX);
                const int maxX = (std::max)(entry.x, entry.otherX);
                const int minZ = (std::min)(entry.z, entry.otherZ);
                const int maxZ = (std::max)(entry.z, entry.otherZ);

                struct DefenseInfo {
                    std::string displayName;
                    int count = 0;
                };
                std::map<std::string, DefenseInfo> defenseCounts;

                for (int sampleX = minX - 1; sampleX <= maxX + 1; ++sampleX) {
                    for (int sampleY = entry.y; sampleY <= entry.y + 2; ++sampleY) {
                        for (int sampleZ = minZ - 1; sampleZ <= maxZ + 1; ++sampleZ) {
                            const bool isBedBlock = (sampleX == entry.x && sampleY == entry.y && sampleZ == entry.z) ||
                                (entry.hasSecondHalf && sampleX == entry.otherX && sampleY == entry.otherY && sampleZ == entry.otherZ);
                            if (isBedBlock) {
                                continue;
                            }

                            jobject defenseBlock = BlockVisuals::GetBlockObjectAt(env, worldObject, sampleX, sampleY, sampleZ);
                            if (!defenseBlock) {
                                continue;
                            }

                            const BlockVisuals::BlockDescriptor defenseDescriptor = BlockVisuals::DescribeBlock(env, defenseBlock);
                            env->DeleteLocalRef(defenseBlock);
                            if (defenseDescriptor.isAir || BlockVisuals::BlockMatchesQuery(defenseDescriptor, "bed")) {
                                continue;
                            }

                            auto& info = defenseCounts[defenseDescriptor.compactName];
                            info.displayName = defenseDescriptor.displayName;
                            ++info.count;
                        }
                    }
                }

                std::vector<DefenseInfo> sortedDefense;
                sortedDefense.reserve(defenseCounts.size());
                for (const auto& [keyName, info] : defenseCounts) {
                    (void)keyName;
                    sortedDefense.push_back(info);
                }

                std::sort(sortedDefense.begin(), sortedDefense.end(), [](const DefenseInfo& left, const DefenseInfo& right) {
                    return left.count > right.count;
                });

                for (size_t index = 0; index < sortedDefense.size() && index < 3; ++index) {
                    entry.chips.push_back({ BuildChipLabel(sortedDefense[index].displayName), sortedDefense[index].count });
                }

                if (entry.chips.empty()) {
                    entry.chips.push_back({ "BED", 1 });
                }

                seenBeds.emplace(key, foundEntries.size());
                foundEntries.push_back(entry);
            }
        }
    }

    std::sort(foundEntries.begin(), foundEntries.end(), [](const BedPlateEntry& left, const BedPlateEntry& right) {
        return left.distance < right.distance;
    });

    {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        m_RenderEntries = std::move(foundEntries);
    }

    g_LastBedScanTime = now;
    env->DeleteLocalRef(localPlayerObject);
    env->DeleteLocalRef(worldObject);
}

void BedPlates::RenderOverlay(ImDrawList* drawList, float screenW, float screenH) {
    if (!IsEnabled() || !drawList || !g_Game || !g_Game->IsInitialized()) {
        return;
    }

    std::vector<BedPlateEntry> renderEntries;
    {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        renderEntries = m_RenderEntries;
    }

    if (renderEntries.empty()) {
        return;
    }

    JNIEnv* env = g_Game->GetCurrentEnv();
    if (!env || env->PushLocalFrame(64) != 0) {
        return;
    }

    const BlockVisuals::RenderMatrixSnapshot snapshot = BlockVisuals::CaptureRenderMatrixSnapshot();
    if (!snapshot.IsValid()) {
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
    glLineWidth(1.5f);
    glColor4f(color[0], color[1], color[2], color[3]);

    for (const auto& entry : renderEntries) {
        BlockVisuals::DrawWireBox(entry.x, entry.y, entry.z, entry.x + 1.0, entry.y + 1.0, entry.z + 1.0);
        if (entry.hasSecondHalf) {
            BlockVisuals::DrawWireBox(entry.otherX, entry.otherY, entry.otherZ, entry.otherX + 1.0, entry.otherY + 1.0, entry.otherZ + 1.0);
        }
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

    ImDrawList* backgroundDrawList = ImGui::GetBackgroundDrawList();
    if (!backgroundDrawList) {
        backgroundDrawList = drawList;
    }

    ImFont* font = ImGui::GetFont();
    const float scale = GetScale();

    for (const auto& entry : renderEntries) {
        const float anchorX = entry.hasSecondHalf
            ? (static_cast<float>(entry.x + entry.otherX + 1) * 0.5f)
            : static_cast<float>(entry.x) + 0.5f;
        const float anchorY = static_cast<float>(entry.y) + 0.72f;
        const float anchorZ = entry.hasSecondHalf
            ? (static_cast<float>(entry.z + entry.otherZ + 1) * 0.5f)
            : static_cast<float>(entry.z) + 0.5f;

        BlockVisuals::ScreenPoint screenPoint;
        if (!BlockVisuals::TryProjectPoint({ anchorX, anchorY, anchorZ }, snapshot, screenPoint)) {
            continue;
        }

        if (screenPoint.x < -80.0f || screenPoint.y < -80.0f || screenPoint.x > screenW + 80.0f || screenPoint.y > screenH + 80.0f) {
            continue;
        }

        const std::string distanceText = std::to_string(static_cast<int>(std::round(entry.distance))) + "m";
        const float titleFontSize = 12.5f * scale;
        const float chipFontSize = 11.0f * scale;
        const float chipHeight = 16.0f * scale;
        const float chipSpacing = 4.0f * scale;
        const float platePaddingX = 8.0f * scale;
        const float platePaddingY = 5.0f * scale;

        const ImVec2 distanceSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, distanceText.c_str());
        float chipsWidth = 0.0f;
        std::vector<float> chipWidths;
        chipWidths.reserve(entry.chips.size());
        for (const auto& chip : entry.chips) {
            const ImVec2 chipTextSize = font->CalcTextSizeA(chipFontSize, FLT_MAX, 0.0f, chip.label.c_str());
            const float chipWidth = chipTextSize.x + (10.0f * scale);
            chipWidths.push_back(chipWidth);
            chipsWidth += chipWidth;
        }
        if (!chipWidths.empty()) {
            chipsWidth += chipSpacing * static_cast<float>(chipWidths.size() - 1);
        }

        const float plateWidth = (std::max)(distanceSize.x, chipsWidth) + (platePaddingX * 2.0f);
        const float plateHeight = distanceSize.y + chipHeight + (platePaddingY * 2.0f) + (4.0f * scale);
        const ImVec2 plateMin(
            std::round(screenPoint.x - (plateWidth * 0.5f)),
            std::round(screenPoint.y - (plateHeight * 0.5f)));
        const ImVec2 plateMax(
            std::round(plateMin.x + plateWidth),
            std::round(plateMin.y + plateHeight));

        backgroundDrawList->AddRectFilled(plateMin, plateMax, IM_COL32(10, 10, 12, 212), 8.0f * scale);
        backgroundDrawList->AddRect(plateMin, plateMax, IM_COL32(255, 255, 255, 22), 8.0f * scale, 0, 1.0f);
        backgroundDrawList->AddRectFilled(
            plateMin,
            ImVec2(plateMax.x, plateMin.y + 2.0f * scale),
            IM_COL32(
                static_cast<int>(color[0] * 255.0f),
                static_cast<int>(color[1] * 255.0f),
                static_cast<int>(color[2] * 255.0f),
                220),
            8.0f * scale,
            ImDrawFlags_RoundCornersTop);

        const ImVec2 titlePos(
            std::round(screenPoint.x - (distanceSize.x * 0.5f)),
            std::round(plateMin.y + platePaddingY));
        backgroundDrawList->AddText(font, titleFontSize, ImVec2(titlePos.x + 1.0f, titlePos.y + 1.0f), IM_COL32(0, 0, 0, 200), distanceText.c_str());
        backgroundDrawList->AddText(font, titleFontSize, titlePos, IM_COL32(245, 245, 245, 255), distanceText.c_str());

        float chipCursorX = std::round(screenPoint.x - (chipsWidth * 0.5f));
        const float chipY = std::round(plateMin.y + platePaddingY + distanceSize.y + (4.0f * scale));
        for (size_t index = 0; index < entry.chips.size() && index < chipWidths.size(); ++index) {
            const ImVec2 chipMin(chipCursorX, chipY);
            const ImVec2 chipMax(chipCursorX + chipWidths[index], chipY + chipHeight);
            backgroundDrawList->AddRectFilled(chipMin, chipMax, IM_COL32(22, 22, 26, 232), 5.0f * scale);
            backgroundDrawList->AddRect(chipMin, chipMax, IM_COL32(255, 255, 255, 18), 5.0f * scale, 0, 1.0f);

            const ImVec2 chipTextSize = font->CalcTextSizeA(chipFontSize, FLT_MAX, 0.0f, entry.chips[index].label.c_str());
            const ImVec2 chipTextPos(
                std::round(chipMin.x + ((chipWidths[index] - chipTextSize.x) * 0.5f)),
                std::round(chipMin.y + ((chipHeight - chipTextSize.y) * 0.5f)));
            backgroundDrawList->AddText(font, chipFontSize, ImVec2(chipTextPos.x + 1.0f, chipTextPos.y + 1.0f), IM_COL32(0, 0, 0, 180), entry.chips[index].label.c_str());
            backgroundDrawList->AddText(font, chipFontSize, chipTextPos, IM_COL32(236, 240, 244, 255), entry.chips[index].label.c_str());

            chipCursorX += chipWidths[index] + chipSpacing;
        }
    }

    MarkInUse(120);
    env->PopLocalFrame(nullptr);
}
