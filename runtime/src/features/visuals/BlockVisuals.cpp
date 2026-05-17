#include "pch.h"
#include "BlockVisuals.h"

#include "../../core/RenderHook.h"
#include "../../game/jni/Class.h"
#include "../../game/jni/Field.h"
#include "../../game/jni/GameInstance.h"
#include "../../game/jni/Method.h"
#include "../../game/mapping/Mapper.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <gl/GL.h>
#include <mutex>
#include <unordered_map>

namespace {
    struct ClipPoint {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
    };

    std::mutex g_BlockDescriptorCacheMutex;
    std::unordered_map<int, BlockVisuals::BlockDescriptor> g_BlockDescriptorCache;

    void ClearJniException(JNIEnv* env) {
        if (env && env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    int GetObjectIdentityHash(JNIEnv* env, jobject object) {
        if (!env || !object) {
            return 0;
        }

        static jclass s_SystemClass = nullptr;
        static jmethodID s_IdentityHashCode = nullptr;
        if (!s_SystemClass || !s_IdentityHashCode) {
            jclass systemClass = env->FindClass("java/lang/System");
            if (!systemClass) {
                ClearJniException(env);
                return 0;
            }

            s_SystemClass = reinterpret_cast<jclass>(env->NewGlobalRef(systemClass));
            env->DeleteLocalRef(systemClass);
            if (!s_SystemClass) {
                return 0;
            }

            s_IdentityHashCode = env->GetStaticMethodID(s_SystemClass, "identityHashCode", "(Ljava/lang/Object;)I");
            if (!s_IdentityHashCode) {
                ClearJniException(env);
                return 0;
            }
        }

        const jint hash = env->CallStaticIntMethod(s_SystemClass, s_IdentityHashCode, object);
        ClearJniException(env);
        return static_cast<int>(hash);
    }

    std::string ReadJString(JNIEnv* env, jstring stringObject) {
        if (!env || !stringObject) {
            return {};
        }

        const char* chars = env->GetStringUTFChars(stringObject, nullptr);
        std::string result = chars ? chars : "";
        if (chars) {
            env->ReleaseStringUTFChars(stringObject, chars);
        }
        return result;
    }

    std::string TrimCopy(std::string text) {
        const auto notSpace = [](unsigned char ch) {
            return !std::isspace(ch);
        };

        const auto begin = std::find_if(text.begin(), text.end(), notSpace);
        if (begin == text.end()) {
            return {};
        }

        const auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
        return std::string(begin, end);
    }

    std::string StripMinecraftFormatting(const std::string& text) {
        std::string result;
        result.reserve(text.size());

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

            result.push_back(text[index]);
        }

        return result;
    }

    std::string NormalizeSpaces(const std::string& input) {
        std::string normalized;
        normalized.reserve(input.size());

        bool lastWasSpace = false;
        for (char raw : input) {
            unsigned char ch = static_cast<unsigned char>(raw);
            if (std::isalnum(ch)) {
                normalized.push_back(static_cast<char>(std::tolower(ch)));
                lastWasSpace = false;
                continue;
            }

            if (!lastWasSpace) {
                normalized.push_back(' ');
                lastWasSpace = true;
            }
        }

        return TrimCopy(normalized);
    }

    std::string CompactName(const std::string& text) {
        std::string compact;
        compact.reserve(text.size());
        for (char raw : text) {
            unsigned char ch = static_cast<unsigned char>(raw);
            if (std::isalnum(ch)) {
                compact.push_back(static_cast<char>(std::tolower(ch)));
            }
        }
        return compact;
    }

    std::string HumanizeBlockName(std::string text) {
        text = StripMinecraftFormatting(text);

        auto erasePrefix = [&text](const std::string& prefix) {
            if (text.rfind(prefix, 0) == 0) {
                text.erase(0, prefix.size());
            }
        };

        erasePrefix("minecraft:");
        erasePrefix("tile.");
        erasePrefix("block.");

        if (text.size() > 5 && text.ends_with(".name")) {
            text.erase(text.size() - 5);
        }

        for (char& ch : text) {
            if (ch == '_' || ch == '.' || ch == ':' || ch == '-') {
                ch = ' ';
            }
        }

        text = NormalizeSpaces(text);
        if (text.empty()) {
            return {};
        }

        bool makeUpper = true;
        for (char& ch : text) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                makeUpper = true;
                continue;
            }

            if (makeUpper) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                makeUpper = false;
            }
        }

        return text;
    }

    bool ContainsWholeToken(const std::string& haystack, const std::string& needle) {
        if (haystack.empty() || needle.empty()) {
            return false;
        }

        size_t offset = haystack.find(needle);
        while (offset != std::string::npos) {
            const bool leftBoundary = offset == 0 || !std::isalnum(static_cast<unsigned char>(haystack[offset - 1]));
            const size_t end = offset + needle.size();
            const bool rightBoundary = end >= haystack.size() || !std::isalnum(static_cast<unsigned char>(haystack[end]));
            if (leftBoundary && rightBoundary) {
                return true;
            }

            offset = haystack.find(needle, offset + 1);
        }

        return false;
    }

    ClipPoint MultiplyMatrixVector(const ClipPoint& vec, const std::vector<float>& mat) {
        if (mat.size() != 16) {
            return {};
        }

        return {
            vec.x * mat[0] + vec.y * mat[4] + vec.z * mat[8] + vec.w * mat[12],
            vec.x * mat[1] + vec.y * mat[5] + vec.z * mat[9] + vec.w * mat[13],
            vec.x * mat[2] + vec.y * mat[6] + vec.z * mat[10] + vec.w * mat[14],
            vec.x * mat[3] + vec.y * mat[7] + vec.z * mat[11] + vec.w * mat[15]
        };
    }

    std::vector<std::string> CollectBlockNames(JNIEnv* env, jobject blockObject) {
        std::vector<std::string> names;
        if (!env || !blockObject) {
            return names;
        }

        jclass blockClass = env->GetObjectClass(blockObject);
        if (!blockClass) {
            return names;
        }

        jclass classMetaClass = env->GetObjectClass(blockClass);
        if (!classMetaClass) {
            env->DeleteLocalRef(blockClass);
            return names;
        }

        jmethodID getMethodsMethod = env->GetMethodID(classMetaClass, "getMethods", "()[Ljava/lang/reflect/Method;");
        jmethodID getSimpleNameMethod = env->GetMethodID(classMetaClass, "getSimpleName", "()Ljava/lang/String;");
        ClearJniException(env);

        if (getSimpleNameMethod) {
            jstring simpleNameObject = static_cast<jstring>(env->CallObjectMethod(blockClass, getSimpleNameMethod));
            ClearJniException(env);
            if (simpleNameObject) {
                names.push_back(ReadJString(env, simpleNameObject));
                env->DeleteLocalRef(simpleNameObject);
            }
        }

        if (!getMethodsMethod) {
            env->DeleteLocalRef(classMetaClass);
            env->DeleteLocalRef(blockClass);
            return names;
        }

        jobjectArray methods = static_cast<jobjectArray>(env->CallObjectMethod(blockClass, getMethodsMethod));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            methods = nullptr;
        }

        jclass methodClass = env->FindClass("java/lang/reflect/Method");
        jclass objectClass = env->FindClass("java/lang/Object");
        jclass stringClass = env->FindClass("java/lang/String");
        if (!methods || !methodClass || !objectClass || !stringClass) {
            if (methods) {
                env->DeleteLocalRef(methods);
            }
            if (methodClass) {
                env->DeleteLocalRef(methodClass);
            }
            if (objectClass) {
                env->DeleteLocalRef(objectClass);
            }
            if (stringClass) {
                env->DeleteLocalRef(stringClass);
            }
            env->DeleteLocalRef(classMetaClass);
            env->DeleteLocalRef(blockClass);
            return names;
        }

        jmethodID getParameterTypesMethod = env->GetMethodID(methodClass, "getParameterTypes", "()[Ljava/lang/Class;");
        jmethodID getReturnTypeMethod = env->GetMethodID(methodClass, "getReturnType", "()Ljava/lang/Class;");
        jmethodID invokeMethod = env->GetMethodID(methodClass, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
        ClearJniException(env);
        jobjectArray emptyArgs = env->NewObjectArray(0, objectClass, nullptr);

        if (getParameterTypesMethod && getReturnTypeMethod && invokeMethod && emptyArgs) {
            const jsize methodCount = env->GetArrayLength(methods);
            for (jsize index = 0; index < methodCount && names.size() < 8; ++index) {
                jobject methodObject = env->GetObjectArrayElement(methods, index);
                if (!methodObject) {
                    continue;
                }

                jobjectArray parameterTypes = static_cast<jobjectArray>(env->CallObjectMethod(methodObject, getParameterTypesMethod));
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    if (parameterTypes) {
                        env->DeleteLocalRef(parameterTypes);
                    }
                    env->DeleteLocalRef(methodObject);
                    continue;
                }

                const bool noParameters = parameterTypes && env->GetArrayLength(parameterTypes) == 0;
                if (parameterTypes) {
                    env->DeleteLocalRef(parameterTypes);
                }
                if (!noParameters) {
                    env->DeleteLocalRef(methodObject);
                    continue;
                }

                jobject returnType = env->CallObjectMethod(methodObject, getReturnTypeMethod);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    if (returnType) {
                        env->DeleteLocalRef(returnType);
                    }
                    env->DeleteLocalRef(methodObject);
                    continue;
                }

                const bool returnsString = returnType && env->IsSameObject(returnType, stringClass);
                if (returnType) {
                    env->DeleteLocalRef(returnType);
                }
                if (!returnsString) {
                    env->DeleteLocalRef(methodObject);
                    continue;
                }

                jobject value = env->CallObjectMethod(methodObject, invokeMethod, blockObject, emptyArgs);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    if (value) {
                        env->DeleteLocalRef(value);
                    }
                    env->DeleteLocalRef(methodObject);
                    continue;
                }

                if (value) {
                    std::string text = ReadJString(env, static_cast<jstring>(value));
                    if (!text.empty() && text.size() < 96) {
                        names.push_back(text);
                    }
                    env->DeleteLocalRef(value);
                }

                env->DeleteLocalRef(methodObject);
            }
        }

        if (emptyArgs) {
            env->DeleteLocalRef(emptyArgs);
        }
        env->DeleteLocalRef(methods);
        env->DeleteLocalRef(methodClass);
        env->DeleteLocalRef(objectClass);
        env->DeleteLocalRef(stringClass);
        env->DeleteLocalRef(classMetaClass);
        env->DeleteLocalRef(blockClass);

        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }
}

BlockVisuals::RenderMatrixSnapshot BlockVisuals::CaptureRenderMatrixSnapshot() {
    std::lock_guard<std::mutex> lock(RenderCache::mtx);
    return {
        RenderCache::modelView,
        RenderCache::projection,
        RenderCache::viewportW,
        RenderCache::viewportH
    };
}

bool BlockVisuals::TryProjectPoint(const WorldPoint& worldPoint, const RenderMatrixSnapshot& snapshot, ScreenPoint& screenPoint) {
    if (!snapshot.IsValid()) {
        return false;
    }

    const ClipPoint clipPoint = MultiplyMatrixVector(
        MultiplyMatrixVector({ worldPoint.x, worldPoint.y, worldPoint.z, 1.0f }, snapshot.modelView),
        snapshot.projection);
    if (!std::isfinite(clipPoint.x) || !std::isfinite(clipPoint.y) ||
        !std::isfinite(clipPoint.z) || !std::isfinite(clipPoint.w) ||
        std::fabs(clipPoint.w) < 0.0001f) {
        return false;
    }

    const float ndcX = clipPoint.x / clipPoint.w;
    const float ndcY = clipPoint.y / clipPoint.w;
    const float ndcZ = clipPoint.z / clipPoint.w;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || !std::isfinite(ndcZ) ||
        ndcZ < -1.0f || ndcZ > 1.0f) {
        return false;
    }

    screenPoint.x = snapshot.viewportWidth * ((ndcX + 1.0f) * 0.5f);
    screenPoint.y = snapshot.viewportHeight * ((1.0f - ndcY) * 0.5f);
    return std::isfinite(screenPoint.x) && std::isfinite(screenPoint.y);
}

void BlockVisuals::DrawWireBox(double minX, double minY, double minZ, double maxX, double maxY, double maxZ) {
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

void BlockVisuals::DrawWireBox(const AxisAlignedBB_t& bounds) {
    DrawWireBox(bounds.minX, bounds.minY, bounds.minZ, bounds.maxX, bounds.maxY, bounds.maxZ);
}

jobject BlockVisuals::GetBlockObjectAt(JNIEnv* env, jobject worldObject, int x, int y, int z) {
    if (!env || !worldObject || !g_Game || !g_Game->IsInitialized()) {
        return nullptr;
    }

    const std::string blockPosClassName = Mapper::Get("net/minecraft/util/BlockPos");
    const std::string blockPosSignature = Mapper::Get("net/minecraft/util/BlockPos", 2);
    const std::string blockStateSignature = Mapper::Get("net/minecraft/block/state/IBlockState", 2);
    const std::string blockSignature = Mapper::Get("net/minecraft/block/Block", 2);
    if (blockPosClassName.empty() || blockPosSignature.empty() ||
        blockStateSignature.empty() || blockSignature.empty()) {
        return nullptr;
    }

    jclass blockPosClass = reinterpret_cast<jclass>(g_Game->FindClass(blockPosClassName));
    if (!blockPosClass) {
        return nullptr;
    }

    jmethodID blockPosCtor = env->GetMethodID(blockPosClass, "<init>", "(III)V");
    if (!blockPosCtor || env->ExceptionCheck()) {
        ClearJniException(env);
        return nullptr;
    }

    jobject blockPos = env->NewObject(blockPosClass, blockPosCtor, static_cast<jint>(x), static_cast<jint>(y), static_cast<jint>(z));
    if (!blockPos || env->ExceptionCheck()) {
        ClearJniException(env);
        if (blockPos) {
            env->DeleteLocalRef(blockPos);
        }
        return nullptr;
    }

    auto* worldClass = reinterpret_cast<Class*>(env->GetObjectClass(worldObject));
    if (!worldClass) {
        env->DeleteLocalRef(blockPos);
        return nullptr;
    }

    const std::string getBlockStateName = Mapper::Get("getBlockState");
    const std::string getBlockStateSignature = "(" + blockPosSignature + ")" + blockStateSignature;
    Method* getBlockStateMethod = getBlockStateName.empty()
        ? nullptr
        : worldClass->GetMethod(env, getBlockStateName.c_str(), getBlockStateSignature.c_str());
    jobject blockState = getBlockStateMethod ? getBlockStateMethod->CallObjectMethod(env, worldObject, false, blockPos) : nullptr;
    env->DeleteLocalRef(reinterpret_cast<jclass>(worldClass));
    env->DeleteLocalRef(blockPos);
    if (!blockState || env->ExceptionCheck()) {
        ClearJniException(env);
        if (blockState) {
            env->DeleteLocalRef(blockState);
        }
        return nullptr;
    }

    auto* blockStateClass = reinterpret_cast<Class*>(env->GetObjectClass(blockState));
    if (!blockStateClass) {
        env->DeleteLocalRef(blockState);
        return nullptr;
    }

    const std::string getBlockName = Mapper::Get("getBlock");
    const std::string getBlockSignature = "()" + blockSignature;
    Method* getBlockMethod = getBlockName.empty()
        ? nullptr
        : blockStateClass->GetMethod(env, getBlockName.c_str(), getBlockSignature.c_str());
    jobject block = getBlockMethod ? getBlockMethod->CallObjectMethod(env, blockState) : nullptr;
    env->DeleteLocalRef(reinterpret_cast<jclass>(blockStateClass));
    env->DeleteLocalRef(blockState);

    if (env->ExceptionCheck()) {
        ClearJniException(env);
        if (block) {
            env->DeleteLocalRef(block);
        }
        return nullptr;
    }

    return block;
}

BlockVisuals::BlockDescriptor BlockVisuals::DescribeBlock(JNIEnv* env, jobject blockObject) {
    if (!env || !blockObject) {
        return {};
    }

    const int identityHash = GetObjectIdentityHash(env, blockObject);
    if (identityHash != 0) {
        std::lock_guard<std::mutex> lock(g_BlockDescriptorCacheMutex);
        auto found = g_BlockDescriptorCache.find(identityHash);
        if (found != g_BlockDescriptorCache.end()) {
            return found->second;
        }
    }

    BlockDescriptor descriptor;
    const std::vector<std::string> rawNames = CollectBlockNames(env, blockObject);
    for (const std::string& rawName : rawNames) {
        const std::string humanized = HumanizeBlockName(rawName);
        if (!humanized.empty()) {
            descriptor.aliases.push_back(NormalizeSpaces(humanized));
            if (descriptor.displayName.empty() || humanized.size() < descriptor.displayName.size()) {
                descriptor.displayName = humanized;
            }
        }

        const std::string compact = CompactName(rawName);
        if (!compact.empty()) {
            descriptor.aliases.push_back(compact);
            if (descriptor.compactName.empty() || compact.size() < descriptor.compactName.size()) {
                descriptor.compactName = compact;
            }
        }
    }

    if (descriptor.displayName.empty()) {
        descriptor.displayName = "Block";
    }
    if (descriptor.compactName.empty()) {
        descriptor.compactName = CompactName(descriptor.displayName);
    }

    descriptor.aliases.push_back(NormalizeSpaces(descriptor.displayName));
    descriptor.aliases.push_back(descriptor.compactName);

    std::sort(descriptor.aliases.begin(), descriptor.aliases.end());
    descriptor.aliases.erase(std::remove_if(descriptor.aliases.begin(), descriptor.aliases.end(), [](const std::string& value) {
        return value.empty();
    }), descriptor.aliases.end());
    descriptor.aliases.erase(std::unique(descriptor.aliases.begin(), descriptor.aliases.end()), descriptor.aliases.end());

    for (const std::string& alias : descriptor.aliases) {
        if (alias == "air") {
            descriptor.isAir = true;
            break;
        }
    }

    if (identityHash != 0) {
        std::lock_guard<std::mutex> lock(g_BlockDescriptorCacheMutex);
        g_BlockDescriptorCache[identityHash] = descriptor;
    }

    return descriptor;
}

std::string BlockVisuals::NormalizeBlockQuery(const std::string& text) {
    std::string normalized = HumanizeBlockName(text);
    if (normalized.empty()) {
        normalized = NormalizeSpaces(text);
    }
    return normalized;
}

bool BlockVisuals::BlockMatchesQuery(const BlockDescriptor& descriptor, const std::string& normalizedQuery) {
    if (normalizedQuery.empty()) {
        return false;
    }

    const std::string compactQuery = CompactName(normalizedQuery);
    for (const std::string& alias : descriptor.aliases) {
        if (alias.empty()) {
            continue;
        }

        if (alias == normalizedQuery || alias == compactQuery) {
            return true;
        }

        if (ContainsWholeToken(alias, normalizedQuery)) {
            return true;
        }
    }

    return false;
}

bool BlockVisuals::IsExposedToAir(JNIEnv* env, jobject worldObject, int x, int y, int z) {
    static constexpr std::array<std::array<int, 3>, 6> kNeighborOffsets = {{
        {{ 1, 0, 0 }},
        {{ -1, 0, 0 }},
        {{ 0, 1, 0 }},
        {{ 0, -1, 0 }},
        {{ 0, 0, 1 }},
        {{ 0, 0, -1 }}
    }};

    for (const auto& offset : kNeighborOffsets) {
        jobject blockObject = GetBlockObjectAt(env, worldObject, x + offset[0], y + offset[1], z + offset[2]);
        if (!blockObject) {
            continue;
        }

        const BlockDescriptor descriptor = DescribeBlock(env, blockObject);
        env->DeleteLocalRef(blockObject);
        if (descriptor.isAir) {
            return true;
        }
    }

    return false;
}
