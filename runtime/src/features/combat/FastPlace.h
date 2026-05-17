#pragma once

#include "../../../../shared/common/modules/Module.h"
#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../deps/imgui/images/modules/no_hit_delay_icon.h"

#ifdef _RUNTIME
#include "../../game/classes/Minecraft.h"
#endif

class FastPlace : public Module {
public:
    MODULE_INFO(FastPlace, "FastPlace", "Removes the delay between placing blocks.", ModuleCategory::Combat) {
        SetImagePrefix(module_icons::no_hit_delay_icon_data, module_icons::no_hit_delay_icon_data_size);
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) return;
        config->FastPlace.m_Enabled = IsEnabled();
        config->Modules.m_FastPlace = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) return;
        SetEnabled(config->FastPlace.m_Enabled);
    }

#ifdef _RUNTIME
    bool IsSynchronous() const override { return true; }
    void TickSynchronous(void* envPtr) override;
#endif
};
