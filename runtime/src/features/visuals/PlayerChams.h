#pragma once

#include "../../../../shared/common/modules/Module.h"
#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../deps/imgui/images/modules/item_chams_icon.h"

#include <algorithm>

#ifdef _RUNTIME
#include "../../../../deps/imgui/imgui.h"

#include <jni.h>
#endif

class PlayerChams : public Module {
public:
    MODULE_INFO(PlayerChams, "Player Chams", "Renders player models through walls.", ModuleCategory::Visuals) {
        SetBeta();
        SetImagePrefix(module_icons::item_chams_icon_data, module_icons::item_chams_icon_data_size);
        AddOption(ModuleOption::Combo("Mode", { "Normal", "Visible/Invisible" }, 0));
        AddOption(ModuleOption::Color("Occluded Color", 1.0f, 0.18f, 0.18f, 0.88f));
        AddOption(ModuleOption::Color("Visible Color", 0.28f, 0.92f, 0.45f, 0.88f));
    }

    bool ShouldRenderOption(size_t optionIndex) const override {
        if (optionIndex == kOccludedColorOption || optionIndex == kVisibleColorOption) {
            return GetMode() != kModeNormal;
        }

        return true;
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        config->PlayerChams.m_Enabled = IsEnabled();
        config->PlayerChams.m_Mode = GetMode();
        const float fallbackOccludedColor[4] = { 1.0f, 0.18f, 0.18f, 0.88f };
        const float fallbackVisibleColor[4] = { 0.28f, 0.92f, 0.45f, 0.88f };
        const ModuleOption* occludedColor = GetOccludedColor();
        const ModuleOption* visibleColor = GetVisibleColor();
        std::copy(
            occludedColor ? std::begin(occludedColor->colorValue) : std::begin(fallbackOccludedColor),
            occludedColor ? std::end(occludedColor->colorValue) : std::end(fallbackOccludedColor),
            config->PlayerChams.m_OccludedColor);
        std::copy(
            visibleColor ? std::begin(visibleColor->colorValue) : std::begin(fallbackVisibleColor),
            visibleColor ? std::end(visibleColor->colorValue) : std::end(fallbackVisibleColor),
            config->PlayerChams.m_VisibleColor);
        config->Modules.m_PlayerChams = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        SetEnabled(config->PlayerChams.m_Enabled);
        SetMode(config->PlayerChams.m_Mode);
        SetColorOption(kOccludedColorOption, config->PlayerChams.m_OccludedColor);
        SetColorOption(kVisibleColorOption, config->PlayerChams.m_VisibleColor);
    }

#ifdef _RUNTIME
    void RenderOverlay(ImDrawList* drawList, float screenW, float screenH) override;
#endif

private:
    static constexpr size_t kModeOption = 0;
    static constexpr size_t kOccludedColorOption = 1;
    static constexpr size_t kVisibleColorOption = 2;
    static constexpr int kModeNormal = 0;
    static constexpr int kModeVisibleInvisible = 1;

    int GetMode() const {
        if (m_Options.size() <= kModeOption) {
            return kModeNormal;
        }

        if (m_Options[kModeOption].comboIndex == kModeVisibleInvisible) {
            return kModeVisibleInvisible;
        }
        return kModeNormal;
    }

    ModuleOption* GetOccludedColor() {
        return m_Options.size() > kOccludedColorOption ? &m_Options[kOccludedColorOption] : nullptr;
    }

    const ModuleOption* GetOccludedColor() const {
        return m_Options.size() > kOccludedColorOption ? &m_Options[kOccludedColorOption] : nullptr;
    }

    ModuleOption* GetVisibleColor() {
        return m_Options.size() > kVisibleColorOption ? &m_Options[kVisibleColorOption] : nullptr;
    }

    const ModuleOption* GetVisibleColor() const {
        return m_Options.size() > kVisibleColorOption ? &m_Options[kVisibleColorOption] : nullptr;
    }

    void SetMode(int value) {
        if (m_Options.size() > kModeOption) {
            if (value == kModeVisibleInvisible) {
                m_Options[kModeOption].comboIndex = kModeVisibleInvisible;
            } else {
                m_Options[kModeOption].comboIndex = kModeNormal;
            }
        }
    }

    void SetColorOption(size_t optionIndex, const float color[4]) {
        if (m_Options.size() <= optionIndex || !color) {
            return;
        }

        for (size_t index = 0; index < 4; ++index) {
            m_Options[optionIndex].colorValue[index] = color[index];
        }
    }
};
