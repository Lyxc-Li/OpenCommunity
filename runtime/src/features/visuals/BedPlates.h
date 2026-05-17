#pragma once

#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../shared/common/modules/Module.h"
#include "../../../../deps/imgui/images/modules/view_details_icon.h"

#include <algorithm>

#ifdef _RUNTIME
#include "../../../../deps/imgui/imgui.h"

#include <atomic>
#include <jni.h>
#include <mutex>
#include <string>
#include <vector>
#endif

class BedPlates : public Module {
public:
    MODULE_INFO(BedPlates, "BedPlates", "Highlights beds and renders compact defense plates.", ModuleCategory::Visuals) {
        SetImagePrefix(module_icons::view_details_icon_data, module_icons::view_details_icon_data_size);
        AddOption(ModuleOption::SliderInt("Range", 64, 16, 128));
        AddOption(ModuleOption::Color("Outline Color", 0.15f, 0.85f, 1.0f, 0.95f));
        AddOption(ModuleOption::SliderFloat("Scale", 1.0f, 0.75f, 1.4f));
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        config->BedPlates.m_Enabled = IsEnabled();
        config->BedPlates.m_Range = GetRange();
        config->BedPlates.m_Scale = GetScale();
        for (size_t index = 0; index < 4; ++index) {
            config->BedPlates.m_Color[index] = GetColor()[index];
        }
        config->Modules.m_BedPlates = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        SetEnabled(config->BedPlates.m_Enabled);
        SetRange(config->BedPlates.m_Range);
        SetScale(config->BedPlates.m_Scale);
        for (size_t index = 0; index < 4; ++index) {
            m_Options[kColorOption].colorValue[index] = config->BedPlates.m_Color[index];
        }
    }

#ifdef _RUNTIME
    bool IsSynchronous() const override { return true; }
    void TickSynchronous(void* envPtr) override;
    void RenderOverlay(ImDrawList* drawList, float screenW, float screenH) override;
    void ShutdownRuntime(void* envPtr) override;

private:
    void RunScan(int range);
    static void BlockIconCallback(const ImDrawList* drawList, const ImDrawCmd* cmd);

    // Defense chip stores a global ref to the block object for icon rendering.
    // blockGlobalRef is nullptr if acquisition failed; fallbackLabel is used instead.
    struct DefenseChip {
        jobject blockGlobalRef = nullptr;
        int count = 0;
        std::string fallbackLabel;
    };

    struct BedPlateEntry {
        int x = 0;
        int y = 0;
        int z = 0;
        bool hasSecondHalf = false;
        int otherX = 0;
        int otherY = 0;
        int otherZ = 0;
        float distance = 0.0f;
        std::vector<DefenseChip> chips;
    };

    // Per-frame icon draw queue for the GL callback
    struct FrameIconDraw {
        jobject blockRef = nullptr; // raw ptr to a global ref owned by chips
        float x = 0.0f;
        float y = 0.0f;
        float size = 0.0f;
    };

    static void ReleaseEntryGlobalRefs(BedPlateEntry& entry);

    std::atomic<bool> m_ScanRunning{false};
    mutable std::mutex m_EntriesMutex;
    std::vector<BedPlateEntry> m_RenderEntries;

    // Frame icon draw queue — populated in RenderOverlay, consumed by BlockIconCallback
    std::vector<FrameIconDraw> m_FrameIconDraws;

    // Cached global ref to a barrier block object for "open bed" display
    jobject m_BarrierBlockGlobalRef = nullptr;
    bool m_BarrierBlockAttempted = false;
#endif

private:
    static constexpr size_t kRangeOption = 0;
    static constexpr size_t kColorOption = 1;
    static constexpr size_t kScaleOption = 2;

    int GetRange() const {
        return m_Options.size() > kRangeOption ? (std::clamp)(m_Options[kRangeOption].intValue, 16, 128) : 64;
    }

    const float* GetColor() const {
        static const float fallback[4] = { 0.15f, 0.85f, 1.0f, 0.95f };
        return m_Options.size() > kColorOption ? m_Options[kColorOption].colorValue : fallback;
    }

    float GetScale() const {
        return m_Options.size() > kScaleOption ? (std::clamp)(m_Options[kScaleOption].floatValue, 0.75f, 1.4f) : 1.0f;
    }

    void SetRange(int value) {
        if (m_Options.size() > kRangeOption) {
            m_Options[kRangeOption].intValue = (std::clamp)(value, 16, 128);
        }
    }

    void SetScale(float value) {
        if (m_Options.size() > kScaleOption) {
            m_Options[kScaleOption].floatValue = (std::clamp)(value, 0.75f, 1.4f);
        }
    }
};
