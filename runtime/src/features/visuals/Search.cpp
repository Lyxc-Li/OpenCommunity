#include "pch.h"
#include "Search.h"

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

namespace {
    constexpr size_t kMaxSearchResults = 192;
    constexpr auto kSearchScanInterval = std::chrono::milliseconds(650);

    std::chrono::steady_clock::time_point g_LastSearchScanTime = std::chrono::steady_clock::time_point::min();

    bool IsFiniteVec(const Vec3D& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
}

std::vector<std::string> Search::GetActiveQueries() const {
    std::vector<std::string> queries;
    queries.reserve(kEntryCount);

    for (size_t entryIndex = 0; entryIndex < kEntryCount; ++entryIndex) {
        if (!IsBlockEntryEnabled(entryIndex)) {
            continue;
        }

        const std::string query = BlockVisuals::NormalizeBlockQuery(GetBlockEntryText(entryIndex));
        if (!query.empty()) {
            queries.push_back(query);
        }
    }

    std::sort(queries.begin(), queries.end());
    queries.erase(std::unique(queries.begin(), queries.end()), queries.end());
    return queries;
}

void Search::TickSynchronous(void* /*envPtr*/) {
    if (!IsEnabled()) {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        m_RenderEntries.clear();
        return;
    }

    if (m_ScanRunning.load(std::memory_order_acquire)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_LastSearchScanTime != std::chrono::steady_clock::time_point::min() &&
        (now - g_LastSearchScanTime) < kSearchScanInterval) {
        return;
    }

    const auto queries = GetActiveQueries();
    if (queries.empty()) {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        m_RenderEntries.clear();
        return;
    }

    g_LastSearchScanTime = now;
    m_ScanRunning.store(true, std::memory_order_release);
    std::thread([this, queries, range = GetRange(), caveOnly = IsOnlyCavesEnabled()]() mutable {
        RunScan(std::move(queries), range, caveOnly);
    }).detach();
}

void Search::RunScan(std::vector<std::string> queries, int range, bool caveOnly) {
    if (!g_Game || !g_Game->IsInitialized()) {
        m_ScanRunning.store(false, std::memory_order_release);
        return;
    }

    JNIEnv* env = g_Game->GetCurrentEnv();
    if (!env || env->PushLocalFrame(512) != 0) {
        m_ScanRunning.store(false, std::memory_order_release);
        return;
    }

    jobject worldObject = Minecraft::GetTheWorld(env);
    jobject localPlayerObject = Minecraft::GetThePlayer(env);
    if (!worldObject || !localPlayerObject) {
        if (worldObject) env->DeleteLocalRef(worldObject);
        if (localPlayerObject) env->DeleteLocalRef(localPlayerObject);
        env->PopLocalFrame(nullptr);
        m_ScanRunning.store(false, std::memory_order_release);
        return;
    }

    auto* localPlayer = reinterpret_cast<Player*>(localPlayerObject);
    const Vec3D localPos = localPlayer->GetPos(env);
    if (!IsFiniteVec(localPos) || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(localPlayerObject);
        env->DeleteLocalRef(worldObject);
        env->PopLocalFrame(nullptr);
        m_ScanRunning.store(false, std::memory_order_release);
        return;
    }

    const int baseX = static_cast<int>(std::floor(localPos.x));
    const int baseY = static_cast<int>(std::floor(localPos.y));
    const int baseZ = static_cast<int>(std::floor(localPos.z));
    const int rangeSq = range * range;

    std::vector<SearchRenderEntry> foundEntries;
    foundEntries.reserve(96);

    for (int dx = -range; dx <= range && foundEntries.size() < kMaxSearchResults; ++dx) {
        for (int dy = -range; dy <= range && foundEntries.size() < kMaxSearchResults; ++dy) {
            for (int dz = -range; dz <= range && foundEntries.size() < kMaxSearchResults; ++dz) {
                const int distanceSq = (dx * dx) + (dy * dy) + (dz * dz);
                if (distanceSq > rangeSq) {
                    continue;
                }

                const int blockX = baseX + dx;
                const int blockY = baseY + dy;
                const int blockZ = baseZ + dz;

                jobject blockObject = BlockVisuals::GetBlockObjectAt(env, worldObject, blockX, blockY, blockZ);
                if (!blockObject) continue;

                const BlockVisuals::BlockDescriptor descriptor = BlockVisuals::DescribeBlock(env, blockObject);
                if (descriptor.isAir) {
                    env->DeleteLocalRef(blockObject);
                    continue;
                }

                bool matchesQuery = false;
                for (const std::string& query : queries) {
                    if (BlockVisuals::BlockMatchesQuery(descriptor, query)) {
                        matchesQuery = true;
                        break;
                    }
                }

                if (!matchesQuery) {
                    env->DeleteLocalRef(blockObject);
                    continue;
                }

                if (caveOnly && !BlockVisuals::IsExposedToAir(env, worldObject, blockX, blockY, blockZ)) {
                    env->DeleteLocalRef(blockObject);
                    continue;
                }

                foundEntries.push_back({
                    static_cast<double>(blockX),     static_cast<double>(blockY),     static_cast<double>(blockZ),
                    static_cast<double>(blockX) + 1.0, static_cast<double>(blockY) + 1.0, static_cast<double>(blockZ) + 1.0,
                    static_cast<double>(blockX) + 0.5, static_cast<double>(blockY) + 0.5, static_cast<double>(blockZ) + 0.5,
                    static_cast<double>(distanceSq)
                });

                env->DeleteLocalRef(blockObject);
            }
        }
    }

    std::sort(foundEntries.begin(), foundEntries.end(), [](const SearchRenderEntry& a, const SearchRenderEntry& b) {
        return a.distanceSq < b.distanceSq;
    });

    {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        m_RenderEntries = std::move(foundEntries);
    }

    env->DeleteLocalRef(localPlayerObject);
    env->DeleteLocalRef(worldObject);
    env->PopLocalFrame(nullptr);
    m_ScanRunning.store(false, std::memory_order_release);
}

void Search::RenderOverlay(ImDrawList* drawList, float screenW, float screenH) {
    (void)drawList;
    (void)screenW;
    (void)screenH;

    if (!IsEnabled() || !g_Game || !g_Game->IsInitialized()) {
        return;
    }

    std::vector<SearchRenderEntry> renderEntries;
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

    jobject localPlayerObject = Minecraft::GetThePlayer(env);
    jobject timerObject = Minecraft::GetTimer(env);
    if (!localPlayerObject || !timerObject) {
        env->PopLocalFrame(nullptr);
        return;
    }

    const BlockVisuals::RenderMatrixSnapshot snapshot = BlockVisuals::CaptureRenderMatrixSnapshot();
    if (!snapshot.IsValid()) {
        env->PopLocalFrame(nullptr);
        return;
    }

    auto* localPlayer = reinterpret_cast<Player*>(localPlayerObject);
    const Vec3D currentPos = localPlayer->GetPos(env);
    const Vec3D previousPos = localPlayer->GetLastTickPos(env);
    const float partialTicks = reinterpret_cast<Timer*>(timerObject)->GetRenderPartialTicks(env);
    if (!IsFiniteVec(currentPos) || !IsFiniteVec(previousPos) || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->PopLocalFrame(nullptr);
        return;
    }

    const double eyeX = previousPos.x + ((currentPos.x - previousPos.x) * partialTicks);
    const double eyeY = previousPos.y + ((currentPos.y - previousPos.y) * partialTicks) + localPlayer->GetEyeHeight(env);
    const double eyeZ = previousPos.z + ((currentPos.z - previousPos.z) * partialTicks);
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
        BlockVisuals::DrawWireBox(entry.minX, entry.minY, entry.minZ, entry.maxX, entry.maxY, entry.maxZ);
    }

    if (IsTracersEnabled()) {
        glBegin(GL_LINES);
        for (const auto& entry : renderEntries) {
            glVertex3d(eyeX, eyeY, eyeZ);
            glVertex3d(entry.centerX, entry.centerY, entry.centerZ);
        }
        glEnd();
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

    MarkInUse(120);
    env->PopLocalFrame(nullptr);
}
