#pragma once

#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../shared/common/modules/Module.h"
#include "../../../../deps/imgui/images/modules/tag_icon.h"

#ifdef _RUNTIME
#include "../../../../deps/imgui/imgui.h"

#include <jni.h>
#endif

class Nametags : public Module {
public:
    MODULE_INFO(Nametags, "Nametags", "Renders Vape-style name tags through walls.", ModuleCategory::Visuals) {
        SetBeta();
        SetImagePrefix(module_icons::tag_icon_data, module_icons::tag_icon_size);
        AddOption(ModuleOption::Toggle("Health", true));
        AddOption(ModuleOption::Toggle("Distance", false));
        AddOption(ModuleOption::Toggle("Equipment", false));
        AddOption(ModuleOption::Toggle("Graphical Health", true));
        AddOption(ModuleOption::Toggle("Absorption", true));
        AddOption(ModuleOption::Toggle("Mobs", false));
        AddOption(ModuleOption::Toggle("Animals", false));
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        config->Nametags.m_Enabled = IsEnabled();
        config->NametagsVisuals.m_Enabled = IsEnabled();
        config->NametagsVisuals.m_ShowHealth = IsHealthEnabled();
        config->NametagsVisuals.m_ShowDistance = IsDistanceEnabled();
        config->NametagsVisuals.m_ShowEquipment = IsEquipmentEnabled();
        config->NametagsVisuals.m_ShowGraphicalHealth = IsGraphicalHealthEnabled();
        config->NametagsVisuals.m_ShowAbsorption = IsAbsorptionEnabled();
        config->NametagsVisuals.m_ShowMobs = IsMobsEnabled();
        config->NametagsVisuals.m_ShowAnimals = IsAnimalsEnabled();
        config->Modules.m_Nametags = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) {
            return;
        }

        SetEnabled(config->Nametags.m_Enabled || config->NametagsVisuals.m_Enabled);
        SetHealthEnabled(config->NametagsVisuals.m_ShowHealth);
        SetDistanceEnabled(config->NametagsVisuals.m_ShowDistance);
        SetEquipmentEnabled(config->NametagsVisuals.m_ShowEquipment);
        SetGraphicalHealthEnabled(config->NametagsVisuals.m_ShowGraphicalHealth);
        SetAbsorptionEnabled(config->NametagsVisuals.m_ShowAbsorption);
        SetMobsEnabled(config->NametagsVisuals.m_ShowMobs);
        SetAnimalsEnabled(config->NametagsVisuals.m_ShowAnimals);
    }

#ifdef _RUNTIME
public:
    static void SetFont(ImFont* font);

    void RenderOverlay(ImDrawList* drawList, float screenW, float screenH) override;

private:
    static ImFont* s_SanFranciscoBoldFont;
#endif

private:
    static constexpr size_t kHealthOption = 0;
    static constexpr size_t kDistanceOption = 1;
    static constexpr size_t kEquipmentOption = 2;
    static constexpr size_t kGraphicalHealthOption = 3;
    static constexpr size_t kAbsorptionOption = 4;
    static constexpr size_t kMobsOption = 5;
    static constexpr size_t kAnimalsOption = 6;

    bool IsHealthEnabled() const {
        return m_Options.size() > kHealthOption && m_Options[kHealthOption].boolValue;
    }

    bool IsDistanceEnabled() const {
        return m_Options.size() > kDistanceOption && m_Options[kDistanceOption].boolValue;
    }

    bool IsEquipmentEnabled() const {
        return m_Options.size() > kEquipmentOption && m_Options[kEquipmentOption].boolValue;
    }

    bool IsGraphicalHealthEnabled() const {
        return m_Options.size() > kGraphicalHealthOption && m_Options[kGraphicalHealthOption].boolValue;
    }

    bool IsAbsorptionEnabled() const {
        return m_Options.size() > kAbsorptionOption && m_Options[kAbsorptionOption].boolValue;
    }

    bool IsMobsEnabled() const {
        return m_Options.size() > kMobsOption && m_Options[kMobsOption].boolValue;
    }

    bool IsAnimalsEnabled() const {
        return m_Options.size() > kAnimalsOption && m_Options[kAnimalsOption].boolValue;
    }

    void SetHealthEnabled(bool value) {
        if (m_Options.size() > kHealthOption) {
            m_Options[kHealthOption].boolValue = value;
        }
    }

    void SetDistanceEnabled(bool value) {
        if (m_Options.size() > kDistanceOption) {
            m_Options[kDistanceOption].boolValue = value;
        }
    }

    void SetEquipmentEnabled(bool value) {
        if (m_Options.size() > kEquipmentOption) {
            m_Options[kEquipmentOption].boolValue = value;
        }
    }

    void SetGraphicalHealthEnabled(bool value) {
        if (m_Options.size() > kGraphicalHealthOption) {
            m_Options[kGraphicalHealthOption].boolValue = value;
        }
    }

    void SetAbsorptionEnabled(bool value) {
        if (m_Options.size() > kAbsorptionOption) {
            m_Options[kAbsorptionOption].boolValue = value;
        }
    }

    void SetMobsEnabled(bool value) {
        if (m_Options.size() > kMobsOption) {
            m_Options[kMobsOption].boolValue = value;
        }
    }

    void SetAnimalsEnabled(bool value) {
        if (m_Options.size() > kAnimalsOption) {
            m_Options[kAnimalsOption].boolValue = value;
        }
    }
};
