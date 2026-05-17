#pragma once

#include "../../../../shared/common/modules/Module.h"
#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../deps/imgui/images/modules/no_render_icon.h"

#ifdef _RUNTIME
#include "../../../../deps/imgui/imgui.h"
#endif

class PlayerESP : public Module {
public:
    MODULE_INFO(PlayerESP, "Player ESP", "Draws 3D boxes around players.", ModuleCategory::Visuals) {
        SetImagePrefix(module_icons::no_render_icon_data, module_icons::no_render_icon_data_size);
        AddOption(ModuleOption::Color("Color", 0.28f, 0.92f, 0.45f, 0.88f));
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        config->PlayerESP.m_Enabled = IsEnabled();
        for (size_t index = 0; index < 4; ++index) {
            config->PlayerESP.m_Color[index] = GetColor()[index];
        }
        config->Modules.m_PlayerESP = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        SetEnabled(config->PlayerESP.m_Enabled);
        for (size_t index = 0; index < 4; ++index) {
            m_Options[kColorOption].colorValue[index] = config->PlayerESP.m_Color[index];
        }
    }

#ifdef _RUNTIME
    void RenderOverlay(ImDrawList* drawList, float screenW, float screenH) override;
#endif

private:
    static constexpr size_t kColorOption = 0;

    const float* GetColor() const {
        static const float fallback[4] = { 0.28f, 0.92f, 0.45f, 0.88f };
        return m_Options.size() > kColorOption ? m_Options[kColorOption].colorValue : fallback;
    }
};
