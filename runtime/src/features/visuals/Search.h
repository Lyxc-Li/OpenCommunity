#pragma once

#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../shared/common/modules/Module.h"
#include "../../../../deps/imgui/images/modules/no_render_icon.h"

#include <algorithm>

#ifdef _RUNTIME
#include "../../../../deps/imgui/imgui.h"

#include <jni.h>
#include <mutex>
#include <string>
#include <vector>
#endif

class Search : public Module {
public:
    MODULE_INFO(Search, "Search", "Renders selected block outlines through walls.", ModuleCategory::Visuals) {
        SetImagePrefix(module_icons::no_render_icon_data, module_icons::no_render_icon_data_size);
        AddOption(ModuleOption::SliderInt("Range", 32, 4, 48));
        AddOption(ModuleOption::Toggle("Only caves", false));
        AddOption(ModuleOption::Toggle("Use tracers", false));
        AddOption(ModuleOption::Color("Outline Color", 0.15f, 0.85f, 1.0f, 0.95f));
        AddOption(ModuleOption::Text("Block 1", "minecraft:bed", 63));
        AddOption(ModuleOption::Toggle("Enabled 1", true));
        AddOption(ModuleOption::Text("Block 2", "", 63));
        AddOption(ModuleOption::Toggle("Enabled 2", false));
        AddOption(ModuleOption::Text("Block 3", "", 63));
        AddOption(ModuleOption::Toggle("Enabled 3", false));
        AddOption(ModuleOption::Text("Block 4", "", 63));
        AddOption(ModuleOption::Toggle("Enabled 4", false));
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        config->Search.m_Enabled = IsEnabled();
        config->Search.m_Range = GetRange();
        config->Search.m_OnlyCaves = IsOnlyCavesEnabled();
        config->Search.m_UseTracers = IsTracersEnabled();
        for (size_t index = 0; index < 4; ++index) {
            config->Search.m_Color[index] = GetColor()[index];
        }
        for (size_t entryIndex = 0; entryIndex < kEntryCount; ++entryIndex) {
            config->Search.m_BlockEnabled[entryIndex] = IsBlockEntryEnabled(entryIndex);
            strncpy_s(
                config->Search.m_BlockQueries[entryIndex],
                sizeof(config->Search.m_BlockQueries[entryIndex]),
                GetBlockEntryText(entryIndex).c_str(),
                _TRUNCATE);
        }
        config->Modules.m_Search = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        SetEnabled(config->Search.m_Enabled);
        SetRange(config->Search.m_Range);
        SetOnlyCavesEnabled(config->Search.m_OnlyCaves);
        SetTracersEnabled(config->Search.m_UseTracers);
        for (size_t index = 0; index < 4; ++index) {
            m_Options[kColorOption].colorValue[index] = config->Search.m_Color[index];
        }
        for (size_t entryIndex = 0; entryIndex < kEntryCount; ++entryIndex) {
            SetBlockEntryText(entryIndex, config->Search.m_BlockQueries[entryIndex]);
            SetBlockEntryEnabled(entryIndex, config->Search.m_BlockEnabled[entryIndex]);
        }
    }

#ifdef _RUNTIME
    bool IsSynchronous() const override { return true; }
    void TickSynchronous(void* envPtr) override;
    void RenderOverlay(ImDrawList* drawList, float screenW, float screenH) override;

private:
    void RunScan(std::vector<std::string> queries, int range, bool caveOnly);

    struct SearchRenderEntry {
        double minX = 0.0;
        double minY = 0.0;
        double minZ = 0.0;
        double maxX = 0.0;
        double maxY = 0.0;
        double maxZ = 0.0;
        double centerX = 0.0;
        double centerY = 0.0;
        double centerZ = 0.0;
        double distanceSq = 0.0;
    };

    std::vector<std::string> GetActiveQueries() const;

    std::atomic<bool> m_ScanRunning{false};
    mutable std::mutex m_EntriesMutex;
    std::vector<SearchRenderEntry> m_RenderEntries;
#endif

private:
    static constexpr size_t kRangeOption = 0;
    static constexpr size_t kOnlyCavesOption = 1;
    static constexpr size_t kTracersOption = 2;
    static constexpr size_t kColorOption = 3;
    static constexpr size_t kFirstEntryTextOption = 4;
    static constexpr size_t kFirstEntryEnabledOption = 5;
    static constexpr size_t kEntryCount = 4;

    size_t GetEntryTextOptionIndex(size_t entryIndex) const {
        return kFirstEntryTextOption + (entryIndex * 2);
    }

    size_t GetEntryEnabledOptionIndex(size_t entryIndex) const {
        return kFirstEntryEnabledOption + (entryIndex * 2);
    }

    int GetRange() const {
        return m_Options.size() > kRangeOption ? (std::clamp)(m_Options[kRangeOption].intValue, 4, 48) : 32;
    }

    bool IsOnlyCavesEnabled() const {
        return m_Options.size() > kOnlyCavesOption && m_Options[kOnlyCavesOption].boolValue;
    }

    bool IsTracersEnabled() const {
        return m_Options.size() > kTracersOption && m_Options[kTracersOption].boolValue;
    }

    const float* GetColor() const {
        static const float fallback[4] = { 0.15f, 0.85f, 1.0f, 0.95f };
        return m_Options.size() > kColorOption ? m_Options[kColorOption].colorValue : fallback;
    }

    std::string GetBlockEntryText(size_t entryIndex) const {
        const size_t optionIndex = GetEntryTextOptionIndex(entryIndex);
        return m_Options.size() > optionIndex ? m_Options[optionIndex].textValue : std::string{};
    }

    bool IsBlockEntryEnabled(size_t entryIndex) const {
        const size_t optionIndex = GetEntryEnabledOptionIndex(entryIndex);
        return m_Options.size() > optionIndex && m_Options[optionIndex].boolValue;
    }

    void SetRange(int value) {
        if (m_Options.size() > kRangeOption) {
            m_Options[kRangeOption].intValue = (std::clamp)(value, 4, 48);
        }
    }

    void SetOnlyCavesEnabled(bool value) {
        if (m_Options.size() > kOnlyCavesOption) {
            m_Options[kOnlyCavesOption].boolValue = value;
        }
    }

    void SetTracersEnabled(bool value) {
        if (m_Options.size() > kTracersOption) {
            m_Options[kTracersOption].boolValue = value;
        }
    }

    void SetBlockEntryText(size_t entryIndex, const char* value) {
        const size_t optionIndex = GetEntryTextOptionIndex(entryIndex);
        if (m_Options.size() > optionIndex) {
            m_Options[optionIndex].textValue = value ? value : "";
        }
    }

    void SetBlockEntryEnabled(size_t entryIndex, bool value) {
        const size_t optionIndex = GetEntryEnabledOptionIndex(entryIndex);
        if (m_Options.size() > optionIndex) {
            m_Options[optionIndex].boolValue = value;
        }
    }
};
