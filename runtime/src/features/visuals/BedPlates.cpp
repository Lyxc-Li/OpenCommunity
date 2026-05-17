#include "pch.h"
#include "BedPlates.h"

#include "BlockVisuals.h"

#include "../../game/classes/Minecraft.h"
#include "../../game/classes/Player.h"
#include "../../game/classes/RenderHelper.h"
#include "../../game/classes/RenderItem.h"
#include "../../game/classes/Timer.h"
#include "../../game/jni/GameInstance.h"
#include "../../game/mapping/Mapper.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <gl/GL.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {
    constexpr auto kBedScanInterval = std::chrono::milliseconds(700);
    constexpr size_t kMaxBedEntries = 48;

    std::chrono::steady_clock::time_point g_LastBedScanTime =
        std::chrono::steady_clock::time_point::min();

    bool IsFiniteVec(const Vec3D& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    uint64_t MakeBedKey(int x, int y, int z) {
        return (static_cast<uint64_t>(static_cast<uint16_t>(x)) << 32) |
               (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16) |
               static_cast<uint64_t>(static_cast<uint16_t>(z));
    }

    // ── JNI cache for block → ItemStack conversion ────────────────────────────

    struct ItemFromBlockCache {
        std::mutex mutex;
        enum class State { Uninitialized, Ready, Failed } state = State::Uninitialized;
        // java.lang.reflect.Method object: static Item getItemFromBlock(Block)
        jobject reflectMethodRef = nullptr;   // global ref
        jmethodID methodInvokeId = nullptr;   // Method.invoke(Object, Object[])
        jmethodID itemStackCtor = nullptr;    // ItemStack(Item, int, int)
        jclass itemStackClass = nullptr;      // global ref
        jclass objectClass = nullptr;         // global ref
    } g_ItemFromBlockCache;

    // Finds a static method on itemClass with signature: (blockClass) -> itemClass
    // Returns a global ref to the java.lang.reflect.Method, or nullptr.
    jobject FindGetItemFromBlockMethod(JNIEnv* env, jclass itemClass, jclass blockClass) {
        jclass classOfItemClass = env->GetObjectClass(reinterpret_cast<jobject>(itemClass));
        if (!classOfItemClass) return nullptr;

        jmethodID getMethodsId = env->GetMethodID(classOfItemClass, "getMethods",
                                                   "()[Ljava/lang/reflect/Method;");
        env->DeleteLocalRef(classOfItemClass);
        if (!getMethodsId || env->ExceptionCheck()) {
            env->ExceptionClear();
            return nullptr;
        }

        jobjectArray methods = static_cast<jobjectArray>(
            env->CallObjectMethod(reinterpret_cast<jobject>(itemClass), getMethodsId));
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        if (!methods) return nullptr;

        jclass methodClass = env->FindClass("java/lang/reflect/Method");
        if (!methodClass) { env->DeleteLocalRef(methods); return nullptr; }

        jmethodID getReturnType = env->GetMethodID(methodClass, "getReturnType",
                                                    "()Ljava/lang/Class;");
        jmethodID getParamTypes = env->GetMethodID(methodClass, "getParameterTypes",
                                                    "()[Ljava/lang/Class;");
        jmethodID getModifiers = env->GetMethodID(methodClass, "getModifiers", "()I");
        env->DeleteLocalRef(methodClass);

        if (!getReturnType || !getParamTypes || !getModifiers) {
            env->DeleteLocalRef(methods);
            return nullptr;
        }

        jobject foundMethod = nullptr;
        const jsize count = env->GetArrayLength(methods);
        constexpr jint STATIC_MODIFIER = 0x8;

        for (jsize i = 0; i < count && !foundMethod; ++i) {
            jobject m = env->GetObjectArrayElement(methods, i);
            if (!m) continue;

            jint mods = env->CallIntMethod(m, getModifiers);
            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(m); continue; }
            if (!(mods & STATIC_MODIFIER)) { env->DeleteLocalRef(m); continue; }

            jobject retType = env->CallObjectMethod(m, getReturnType);
            if (env->ExceptionCheck()) { env->ExceptionClear(); if (retType) env->DeleteLocalRef(retType); env->DeleteLocalRef(m); continue; }
            const bool returnsItem = retType && env->IsSameObject(retType, reinterpret_cast<jobject>(itemClass));
            if (retType) env->DeleteLocalRef(retType);
            if (!returnsItem) { env->DeleteLocalRef(m); continue; }

            jobjectArray params = static_cast<jobjectArray>(env->CallObjectMethod(m, getParamTypes));
            if (env->ExceptionCheck()) { env->ExceptionClear(); if (params) env->DeleteLocalRef(params); env->DeleteLocalRef(m); continue; }

            bool takesBlock = false;
            if (params && env->GetArrayLength(params) == 1) {
                jobject p0 = env->GetObjectArrayElement(params, 0);
                takesBlock = p0 && env->IsSameObject(p0, reinterpret_cast<jobject>(blockClass));
                if (p0) env->DeleteLocalRef(p0);
            }
            if (params) env->DeleteLocalRef(params);

            if (takesBlock) {
                foundMethod = env->NewGlobalRef(m);
            }
            env->DeleteLocalRef(m);
        }

        env->DeleteLocalRef(methods);
        return foundMethod; // global ref or nullptr
    }

    bool EnsureItemFromBlockCache(JNIEnv* env) {
        auto& cache = g_ItemFromBlockCache;
        if (cache.state == ItemFromBlockCache::State::Ready) return true;
        if (cache.state == ItemFromBlockCache::State::Failed) return false;

        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.state != ItemFromBlockCache::State::Uninitialized) {
            return cache.state == ItemFromBlockCache::State::Ready;
        }

        cache.state = ItemFromBlockCache::State::Failed; // optimistic fail

        if (!g_Game || !g_Game->IsInitialized()) return false;

        const std::string itemName     = Mapper::Get("net/minecraft/item/Item");
        const std::string blockName    = Mapper::Get("net/minecraft/block/Block");
        const std::string stackName    = Mapper::Get("net/minecraft/item/ItemStack");
        const std::string itemSig      = Mapper::Get("net/minecraft/item/Item", 2);

        if (itemName.empty() || blockName.empty() || stackName.empty() || itemSig.empty()) {
            return false;
        }

        jclass itemClass  = reinterpret_cast<jclass>(g_Game->FindClass(itemName));
        jclass blockClass = reinterpret_cast<jclass>(g_Game->FindClass(blockName));
        jclass stackClass = reinterpret_cast<jclass>(g_Game->FindClass(stackName));
        if (!itemClass || !blockClass || !stackClass) return false;

        // Find getItemFromBlock via reflection
        jobject refMethod = FindGetItemFromBlockMethod(env, itemClass, blockClass);
        if (!refMethod) return false;

        // Cache Method.invoke jmethodID
        jclass methodClassLookup = env->GetObjectClass(refMethod);
        jmethodID invokeId = methodClassLookup
            ? env->GetMethodID(methodClassLookup,
                               "invoke",
                               "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;")
            : nullptr;
        if (methodClassLookup) env->DeleteLocalRef(methodClassLookup);
        if (!invokeId || env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteGlobalRef(refMethod);
            return false;
        }

        // ItemStack(Item, int, int) constructor
        const std::string ctorSig = "(" + itemSig + "II)V";
        jmethodID ctor = env->GetMethodID(stackClass, "<init>", ctorSig.c_str());
        if (!ctor || env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteGlobalRef(refMethod);
            return false;
        }

        jclass objectCls = env->FindClass("java/lang/Object");
        if (!objectCls) {
            env->DeleteGlobalRef(refMethod);
            return false;
        }

        cache.reflectMethodRef = refMethod;
        cache.methodInvokeId   = invokeId;
        cache.itemStackCtor    = ctor;
        cache.itemStackClass   = reinterpret_cast<jclass>(env->NewGlobalRef(reinterpret_cast<jobject>(stackClass)));
        cache.objectClass      = reinterpret_cast<jclass>(env->NewGlobalRef(reinterpret_cast<jobject>(objectCls)));
        env->DeleteLocalRef(objectCls);
        cache.state = ItemFromBlockCache::State::Ready;
        return true;
    }

    // Creates a local ref to an ItemStack for the given block, or nullptr on failure.
    jobject TryCreateItemStackForBlock(JNIEnv* env, jobject blockRef) {
        if (!env || !blockRef) return nullptr;
        if (!EnsureItemFromBlockCache(env)) return nullptr;

        auto& cache = g_ItemFromBlockCache;

        // Build Object[] args = { blockRef }
        jobjectArray args = env->NewObjectArray(1, cache.objectClass, nullptr);
        if (!args) return nullptr;
        env->SetObjectArrayElement(args, 0, blockRef);

        // Item item = getItemFromBlock(block)  via Method.invoke(null, args)
        jobject item = env->CallObjectMethod(cache.reflectMethodRef,
                                             cache.methodInvokeId,
                                             static_cast<jobject>(nullptr),
                                             args);
        env->DeleteLocalRef(args);

        if (!item || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (item) env->DeleteLocalRef(item);
            return nullptr;
        }

        // ItemStack stack = new ItemStack(item, 1, 0)
        jobject stack = env->NewObject(cache.itemStackClass, cache.itemStackCtor,
                                       item, static_cast<jint>(1), static_cast<jint>(0));
        env->DeleteLocalRef(item);

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            if (stack) env->DeleteLocalRef(stack);
            return nullptr;
        }
        return stack; // local ref
    }

    // ── Barrier block global ref cache ───────────────────────────────────────

    std::mutex g_BarrierMutex;
    jobject g_BarrierBlockRef = nullptr;
    bool g_BarrierAttempted = false;

    // Returns the cached barrier Block global ref (or nullptr).
    jobject TryGetBarrierBlockRef(JNIEnv* env) {
        std::lock_guard<std::mutex> lock(g_BarrierMutex);
        if (g_BarrierAttempted) return g_BarrierBlockRef;
        g_BarrierAttempted = true;

        if (!g_Game || !g_Game->IsInitialized()) return nullptr;

        const std::string blocksName = Mapper::Get("net/minecraft/init/Blocks");
        if (blocksName.empty()) return nullptr;

        jclass blocksClass = reinterpret_cast<jclass>(g_Game->FindClass(blocksName));
        if (!blocksClass) return nullptr;

        // Try the known field names for Blocks.barrier
        const char* fieldNames[] = { "barrier", "field_150379_bu" };
        const std::string blockSig = Mapper::Get("net/minecraft/block/Block", 2);
        if (blockSig.empty()) return nullptr;

        for (const char* fieldName : fieldNames) {
            jfieldID fid = env->GetStaticFieldID(blocksClass, fieldName, blockSig.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!fid) continue;

            jobject barrier = env->GetStaticObjectField(blocksClass, fid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); if (barrier) env->DeleteLocalRef(barrier); continue; }
            if (barrier) {
                g_BarrierBlockRef = env->NewGlobalRef(barrier);
                env->DeleteLocalRef(barrier);
                return g_BarrierBlockRef;
            }
        }
        return nullptr;
    }
}

// ── BedPlates::ReleaseEntryGlobalRefs ────────────────────────────────────────

void BedPlates::ReleaseEntryGlobalRefs(BedPlateEntry& entry) {
    JNIEnv* env = g_Game ? g_Game->GetCurrentEnv() : nullptr;
    for (auto& chip : entry.chips) {
        if (chip.blockGlobalRef && env) {
            env->DeleteGlobalRef(chip.blockGlobalRef);
        }
        chip.blockGlobalRef = nullptr;
    }
}

void BedPlates::ShutdownRuntime(void* /*envPtr*/) {
    std::lock_guard<std::mutex> lock(m_EntriesMutex);
    for (auto& entry : m_RenderEntries) {
        ReleaseEntryGlobalRefs(entry);
    }
    m_RenderEntries.clear();
}

// ── TickSynchronous ───────────────────────────────────────────────────────────

void BedPlates::TickSynchronous(void* /*envPtr*/) {
    if (!IsEnabled()) {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        for (auto& entry : m_RenderEntries) {
            ReleaseEntryGlobalRefs(entry);
        }
        m_RenderEntries.clear();
        return;
    }

    if (m_ScanRunning.load(std::memory_order_acquire)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_LastBedScanTime != std::chrono::steady_clock::time_point::min() &&
        (now - g_LastBedScanTime) < kBedScanInterval) {
        return;
    }

    g_LastBedScanTime = now;
    m_ScanRunning.store(true, std::memory_order_release);
    std::thread([this, range = GetRange()]() { RunScan(range); }).detach();
}

// ── RunScan ───────────────────────────────────────────────────────────────────

void BedPlates::RunScan(int range) {
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

    static constexpr std::array<std::array<int, 3>, 4> kHorizontalNeighbors = {{
        {{ 1, 0, 0 }}, {{ -1, 0, 0 }}, {{ 0, 0, 1 }}, {{ 0, 0, -1 }}
    }};

    std::vector<BedPlateEntry> foundEntries;
    foundEntries.reserve(16);
    std::unordered_set<uint64_t> seenBeds;

    for (int dx = -range; dx <= range && foundEntries.size() < kMaxBedEntries; ++dx) {
        for (int dy = -6; dy <= 6 && foundEntries.size() < kMaxBedEntries; ++dy) {
            for (int dz = -range; dz <= range && foundEntries.size() < kMaxBedEntries; ++dz) {
                const int distanceSq = dx * dx + dy * dy + dz * dz;
                if (distanceSq > rangeSq) continue;

                const int bx = baseX + dx;
                const int by = baseY + dy;
                const int bz = baseZ + dz;

                jobject blockObj = BlockVisuals::GetBlockObjectAt(env, worldObject, bx, by, bz);
                if (!blockObj) continue;

                const BlockVisuals::BlockDescriptor desc = BlockVisuals::DescribeBlock(env, blockObj);
                env->DeleteLocalRef(blockObj);
                if (!BlockVisuals::BlockMatchesQuery(desc, "bed")) continue;

                int otherX = bx, otherY = by, otherZ = bz;
                bool hasOther = false;
                for (const auto& off : kHorizontalNeighbors) {
                    jobject nb = BlockVisuals::GetBlockObjectAt(env, worldObject,
                                                                bx + off[0], by + off[1], bz + off[2]);
                    if (!nb) continue;
                    const BlockVisuals::BlockDescriptor nd = BlockVisuals::DescribeBlock(env, nb);
                    env->DeleteLocalRef(nb);
                    if (!BlockVisuals::BlockMatchesQuery(nd, "bed")) continue;
                    otherX = bx + off[0]; otherY = by + off[1]; otherZ = bz + off[2];
                    hasOther = true;
                    break;
                }

                int canonX = bx, canonY = by, canonZ = bz;
                if (hasOther && std::tie(otherX, otherY, otherZ) < std::tie(canonX, canonY, canonZ)) {
                    canonX = otherX; canonY = otherY; canonZ = otherZ;
                }

                if (!seenBeds.insert(MakeBedKey(canonX, canonY, canonZ)).second) continue;

                BedPlateEntry entry;
                entry.x = canonX; entry.y = canonY; entry.z = canonZ;
                entry.hasSecondHalf = hasOther;
                entry.otherX = hasOther ? otherX : canonX;
                entry.otherY = hasOther ? otherY : canonY;
                entry.otherZ = hasOther ? otherZ : canonZ;
                entry.distance = std::sqrt(static_cast<float>(distanceSq));

                const int minX = (std::min)(entry.x, entry.otherX);
                const int maxX = (std::max)(entry.x, entry.otherX);
                const int minZ = (std::min)(entry.z, entry.otherZ);
                const int maxZ = (std::max)(entry.z, entry.otherZ);

                struct DefInfo { jobject blockGlobalRef; std::string displayName; std::string key; int count = 0; };
                std::vector<DefInfo> defInfos;

                for (int sx = minX - 1; sx <= maxX + 1; ++sx) {
                    for (int sy = entry.y; sy <= entry.y + 2; ++sy) {
                        for (int sz = minZ - 1; sz <= maxZ + 1; ++sz) {
                            const bool isBedPos =
                                (sx == entry.x && sy == entry.y && sz == entry.z) ||
                                (entry.hasSecondHalf && sx == entry.otherX &&
                                 sy == entry.otherY && sz == entry.otherZ);
                            if (isBedPos) continue;

                            jobject defBlock = BlockVisuals::GetBlockObjectAt(env, worldObject, sx, sy, sz);
                            if (!defBlock) continue;
                            const BlockVisuals::BlockDescriptor dd = BlockVisuals::DescribeBlock(env, defBlock);

                            if (dd.isAir || BlockVisuals::BlockMatchesQuery(dd, "bed")) {
                                env->DeleteLocalRef(defBlock);
                                continue;
                            }

                            // Find or create DefInfo entry for this block type
                            auto it = std::find_if(defInfos.begin(), defInfos.end(),
                                [&](const DefInfo& di) { return di.key == dd.compactName; });

                            if (it == defInfos.end()) {
                                DefInfo di;
                                di.key = dd.compactName;
                                di.displayName = dd.displayName;
                                di.blockGlobalRef = env->NewGlobalRef(defBlock);
                                di.count = 1;
                                defInfos.push_back(std::move(di));
                            } else {
                                ++it->count;
                            }
                            env->DeleteLocalRef(defBlock);
                        }
                    }
                }

                // Sort descending by count, keep top 3
                std::sort(defInfos.begin(), defInfos.end(),
                    [](const DefInfo& a, const DefInfo& b) { return a.count > b.count; });

                for (size_t i = 0; i < defInfos.size() && i < 3; ++i) {
                    DefenseChip chip;
                    chip.blockGlobalRef = defInfos[i].blockGlobalRef; // transfer ownership
                    chip.count = defInfos[i].count;
                    chip.fallbackLabel = defInfos[i].displayName;
                    entry.chips.push_back(std::move(chip));
                    defInfos[i].blockGlobalRef = nullptr; // ownership transferred
                }

                // Release global refs for non-top-3 entries
                for (size_t i = 3; i < defInfos.size(); ++i) {
                    if (defInfos[i].blockGlobalRef) {
                        env->DeleteGlobalRef(defInfos[i].blockGlobalRef);
                    }
                }

                // No defense chips → add a "barrier" chip
                if (entry.chips.empty()) {
                    DefenseChip open;
                    open.blockGlobalRef = nullptr; // filled in at render time from barrier cache
                    open.count = 0;
                    open.fallbackLabel = "OPEN";
                    entry.chips.push_back(std::move(open));
                }

                foundEntries.push_back(std::move(entry));
            }
        }
    }

    std::sort(foundEntries.begin(), foundEntries.end(),
        [](const BedPlateEntry& a, const BedPlateEntry& b) { return a.distance < b.distance; });

    // Swap old entries, releasing their global refs first
    std::vector<BedPlateEntry> old;
    {
        std::lock_guard<std::mutex> lock(m_EntriesMutex);
        old = std::move(m_RenderEntries);
        m_RenderEntries = std::move(foundEntries);
    }
    for (auto& e : old) {
        ReleaseEntryGlobalRefs(e);
    }

    env->DeleteLocalRef(localPlayerObject);
    env->DeleteLocalRef(worldObject);
    env->PopLocalFrame(nullptr);
    m_ScanRunning.store(false, std::memory_order_release);
}

// ── BlockIconCallback ─────────────────────────────────────────────────────────

void BedPlates::BlockIconCallback(const ImDrawList* /*drawList*/, const ImDrawCmd* cmd) {
    auto* self = static_cast<BedPlates*>(cmd->UserCallbackData);
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

    if (env->PushLocalFrame(static_cast<jint>(self->m_FrameIconDraws.size() * 4)) == 0) {
        for (const auto& draw : self->m_FrameIconDraws) {
            if (!draw.blockRef) continue;

            jobject stack = TryCreateItemStackForBlock(env, draw.blockRef);
            if (!stack) continue;

            const float mcScale = draw.size / 16.0f;
            GLint projBefore, mvBefore;
            glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projBefore);
            glGetIntegerv(GL_MODELVIEW_STACK_DEPTH,  &mvBefore);

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glTranslatef(draw.x, draw.y, 0.0f);
            glScalef(mcScale, mcScale, 1.0f);
            renderItem->RenderItemIntoGUI(stack, 0, 0, env);
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

            env->DeleteLocalRef(stack);
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

// ── RenderOverlay ─────────────────────────────────────────────────────────────

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

    // Ensure barrier block ref is ready (for "OPEN" chips)
    TryGetBarrierBlockRef(env);

    const BlockVisuals::RenderMatrixSnapshot snapshot = BlockVisuals::CaptureRenderMatrixSnapshot();
    if (!snapshot.IsValid()) {
        env->PopLocalFrame(nullptr);
        return;
    }

    // ── GL wire-box pass (bed outlines) ──────────────────────────────────────
    const float* color = GetColor();
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                 GL_LINE_BIT | GL_POLYGON_BIT | GL_TEXTURE_BIT | GL_LIGHTING_BIT);

    // Save without pushing — projection stack min-depth is 2, pushing risks overflow
    GLfloat savedWireProj[16], savedWireMV[16];
    glGetFloatv(GL_PROJECTION_MATRIX, savedWireProj);
    glGetFloatv(GL_MODELVIEW_MATRIX,  savedWireMV);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(snapshot.projection.data());
    glMatrixMode(GL_MODELVIEW);
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
        BlockVisuals::DrawWireBox(entry.x, entry.y, entry.z,
                                  entry.x + 1.0, entry.y + 1.0, entry.z + 1.0);
        if (entry.hasSecondHalf) {
            BlockVisuals::DrawWireBox(entry.otherX, entry.otherY, entry.otherZ,
                                      entry.otherX + 1.0, entry.otherY + 1.0, entry.otherZ + 1.0);
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
    glLoadMatrixf(savedWireProj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(savedWireMV);
    glPopAttrib();

    // ── ImGui plate pass ─────────────────────────────────────────────────────
    // Draw order matters for correct layering:
    //   Pass 1: plate backgrounds + slot boxes  (ImGui)
    //   GL callback: block icons via RenderItemIntoGUI  (GL, on top of boxes)
    //   Reset state callback
    //   Pass 2: count badge text  (ImGui, on top of icons)
    //   Pass 2 (fallback): abbreviated text for chips without block refs

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (!bg) bg = drawList;

    ImFont* font = ImGui::GetFont();
    const float scale = GetScale();
    const float iconSize    = 18.0f * scale;
    const float iconPad     = 3.0f * scale;
    const float platePadX   = 7.0f * scale;
    const float platePadY   = 5.0f * scale;
    const float distFontSize  = 12.5f * scale;
    const float countFontSize = 9.0f * scale;

    // Deferred text draws accumulated during pass 1, applied in pass 2
    struct DeferredText {
        float x, y;
        float fontSize;
        ImU32 shadowColor;
        ImU32 textColor;
        std::string text;
    };
    std::vector<DeferredText> deferredTexts;

    m_FrameIconDraws.clear();

    // ── Pass 1: backgrounds, slot boxes, queue icons ──────────────────────────
    for (const auto& entry : renderEntries) {
        const float anchorX = entry.hasSecondHalf
            ? (static_cast<float>(entry.x + entry.otherX + 1) * 0.5f)
            : static_cast<float>(entry.x) + 0.5f;
        const float anchorY = static_cast<float>(entry.y) + 0.72f;
        const float anchorZ = entry.hasSecondHalf
            ? (static_cast<float>(entry.z + entry.otherZ + 1) * 0.5f)
            : static_cast<float>(entry.z) + 0.5f;

        BlockVisuals::ScreenPoint sp;
        if (!BlockVisuals::TryProjectPoint({ anchorX, anchorY, anchorZ }, snapshot, sp)) continue;
        if (sp.x < -80.0f || sp.y < -80.0f ||
            sp.x > screenW + 80.0f || sp.y > screenH + 80.0f) continue;

        const std::string distText =
            std::to_string(static_cast<int>(std::round(entry.distance))) + "m";
        const ImVec2 distSize = font->CalcTextSizeA(distFontSize, FLT_MAX, 0.0f, distText.c_str());

        const int numChips = static_cast<int>(entry.chips.size());
        const float iconsRowW = static_cast<float>(numChips) * iconSize
                              + static_cast<float>((std::max)(0, numChips - 1)) * iconPad;

        const float plateW = (std::max)(distSize.x, iconsRowW) + platePadX * 2.0f;
        const float plateH = distSize.y + iconSize + platePadY * 2.0f + 4.0f * scale;

        const ImVec2 pMin(std::round(sp.x - plateW * 0.5f),
                          std::round(sp.y - plateH * 0.5f));
        const ImVec2 pMax(pMin.x + plateW, pMin.y + plateH);

        bg->AddRectFilled(pMin, pMax, IM_COL32(10, 10, 12, 212), 8.0f * scale);
        bg->AddRect(pMin, pMax, IM_COL32(255, 255, 255, 22), 8.0f * scale, 0, 1.0f);
        bg->AddRectFilled(
            pMin, ImVec2(pMax.x, pMin.y + 2.0f * scale),
            IM_COL32(static_cast<int>(color[0] * 255.0f),
                     static_cast<int>(color[1] * 255.0f),
                     static_cast<int>(color[2] * 255.0f), 220),
            8.0f * scale, ImDrawFlags_RoundCornersTop);

        // Distance label (goes in pass 1 — above the icon row, fine to draw early)
        const ImVec2 distPos(std::round(sp.x - distSize.x * 0.5f),
                             std::round(pMin.y + platePadY));
        bg->AddText(font, distFontSize,
                    ImVec2(distPos.x + 1.0f, distPos.y + 1.0f),
                    IM_COL32(0, 0, 0, 200), distText.c_str());
        bg->AddText(font, distFontSize, distPos,
                    IM_COL32(245, 245, 245, 255), distText.c_str());

        // Icon row slot boxes
        const float iconRowStartX = std::round(sp.x - iconsRowW * 0.5f);
        const float iconRowY = std::round(pMin.y + platePadY + distSize.y + 4.0f * scale);

        for (int ci = 0; ci < numChips; ++ci) {
            const auto& chip = entry.chips[ci];
            const float ix = iconRowStartX + static_cast<float>(ci) * (iconSize + iconPad);
            const ImVec2 iMin(ix, iconRowY);
            const ImVec2 iMax(ix + iconSize, iconRowY + iconSize);

            bg->AddRectFilled(iMin, iMax, IM_COL32(20, 20, 24, 220), 3.0f * scale);
            bg->AddRect(iMin, iMax, IM_COL32(255, 255, 255, 30), 3.0f * scale, 0, 1.0f);

            jobject blockRef = chip.blockGlobalRef;
            if (!blockRef && chip.fallbackLabel == "OPEN") {
                blockRef = g_BarrierBlockRef;
            }

            if (blockRef) {
                m_FrameIconDraws.push_back({ blockRef, ix, iconRowY, iconSize });
            }

            // Defer count badge + fallback text to pass 2 (must appear above icons)
            if (blockRef) {
                if (chip.count > 0) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%d", chip.count);
                    const ImVec2 cs = font->CalcTextSizeA(countFontSize, FLT_MAX, 0.0f, buf);
                    deferredTexts.push_back({
                        std::round(iMax.x - cs.x - 2.0f * scale),
                        std::round(iMax.y - cs.y),
                        countFontSize,
                        IM_COL32(0, 0, 0, 200),
                        IM_COL32(255, 255, 255, 240),
                        buf
                    });
                }
            } else {
                // No block ref — draw abbreviation centered (deferred so it's above fallback bg)
                const std::string& lbl = chip.fallbackLabel.empty() ? "?" : chip.fallbackLabel;
                std::string abbr;
                bool nextUpper = true;
                for (unsigned char c : lbl) {
                    if (c == ' ') { nextUpper = true; continue; }
                    if (nextUpper) { abbr += static_cast<char>(std::toupper(c)); nextUpper = false; }
                    if (abbr.size() >= 3) break;
                }
                if (abbr.empty()) abbr = "?";
                const float lblSize = 9.5f * scale;
                const ImVec2 ts = font->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, abbr.c_str());
                deferredTexts.push_back({
                    std::round(ix + (iconSize - ts.x) * 0.5f),
                    std::round(iconRowY + (iconSize - ts.y) * 0.5f),
                    lblSize,
                    IM_COL32(0, 0, 0, 0),          // no shadow for fallback
                    IM_COL32(200, 210, 220, 255),
                    abbr
                });
            }
        }
    }

    // ── GL callback: render block icons on top of slot boxes ─────────────────
    if (!m_FrameIconDraws.empty()) {
        bg->AddCallback(BlockIconCallback, this);
    }

    // ── Pass 2: count badges and fallback text (above icons) ─────────────────
    for (const auto& dt : deferredTexts) {
        if (dt.shadowColor & 0xFF000000u) {
            bg->AddText(font, dt.fontSize,
                        ImVec2(dt.x + 1.0f, dt.y + 1.0f),
                        dt.shadowColor, dt.text.c_str());
        }
        bg->AddText(font, dt.fontSize, ImVec2(dt.x, dt.y), dt.textColor, dt.text.c_str());
    }

    MarkInUse(120);
    env->PopLocalFrame(nullptr);
}
