#pragma once

#include "../../../../shared/common/modules/Module.h"
#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../deps/imgui/images/modules/no_hit_delay_icon.h"

#ifdef _RUNTIME
#include "../../game/classes/Minecraft.h"
#endif

class FastPlace : public Module {
public:
    MODULE_INFO(FastPlace, "FastPlace", "Reduces the delay between placing blocks.", ModuleCategory::Utility) {
        SetImagePrefix(module_icons::no_hit_delay_icon_data, module_icons::no_hit_delay_icon_data_size);
        AddOption(ModuleOption::SliderInt("Delay", 1, 1, 6));
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) return;
        config->FastPlace.m_Enabled = IsEnabled();
        config->FastPlace.m_Delay = GetDelay();
        config->Modules.m_FastPlace = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) return;
        SetEnabled(config->FastPlace.m_Enabled);
        SetDelay(config->FastPlace.m_Delay);
    }

#ifdef _RUNTIME
    bool IsSynchronous() const override { return true; }
    void TickSynchronous(void* envPtr) override;

private:
    int m_TicksSinceReset = 0;
#endif

private:
    static constexpr size_t kDelayOption = 0;

    int GetDelay() const {
        return m_Options.size() > kDelayOption ? (std::clamp)(m_Options[kDelayOption].intValue, 1, 6) : 1;
    }

    void SetDelay(int value) {
        if (m_Options.size() > kDelayOption) {
            m_Options[kDelayOption].intValue = (std::clamp)(value, 1, 6);
        }
    }
};
