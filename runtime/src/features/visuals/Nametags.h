#pragma once

#include "../../../../shared/common/ModuleConfig.h"
#include "../../../../shared/common/modules/Module.h"
#include "../../../../deps/imgui/images/modules/tag_icon.h"

#ifdef _RUNTIME
#include "../../../../deps/imgui/imgui.h"

#include <jni.h>
#include <vector>
#endif

class Nametags : public Module {
public:
    MODULE_INFO(Nametags, "Nametags", "Renders Vape-style name tags through walls.", ModuleCategory::Visuals) {
        SetBeta();
        SetImagePrefix(module_icons::tag_icon_data, module_icons::tag_icon_size);
        AddOption(ModuleOption::Toggle("Health", true));
        AddOption(ModuleOption::Toggle("Distance", false));
        AddOption(ModuleOption::Toggle("Equipment", false));
        AddOption(ModuleOption::Toggle("Effects", true));
        AddOption(ModuleOption::Toggle("Mobs", false));
        AddOption(ModuleOption::Toggle("Animals", false));
        AddOption(ModuleOption::Toggle("Auto Scale", true));
        AddOption(ModuleOption::SliderFloat("Scale", 1.0f, 0.5f, 2.0f));
        AddOption(ModuleOption::SliderInt("Max Distance", 64, 16, 255));
    }

    void SyncToConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) return;

        config->Nametags.m_Enabled             = IsEnabled();
        config->NametagsVisuals.m_Enabled       = IsEnabled();
        config->NametagsVisuals.m_ShowHealth    = IsHealthEnabled();
        config->NametagsVisuals.m_ShowDistance  = IsDistanceEnabled();
        config->NametagsVisuals.m_ShowEquipment = IsEquipmentEnabled();
        config->NametagsVisuals.m_ShowEffects   = IsEffectsEnabled();
        config->NametagsVisuals.m_ShowMobs      = IsMobsEnabled();
        config->NametagsVisuals.m_ShowAnimals   = IsAnimalsEnabled();
        config->NametagsVisuals.m_AutoScale     = IsAutoScaleEnabled();
        config->NametagsVisuals.m_Scale         = GetScale();
        config->NametagsVisuals.m_MaxDistance   = GetMaxDistance();
        config->Modules.m_Nametags              = IsEnabled();
    }

    void SyncFromConfig(void* configPtr) override {
        auto* config = static_cast<ModuleConfig*>(configPtr);
        if (!config) return;

        SetEnabled(config->Nametags.m_Enabled || config->NametagsVisuals.m_Enabled);
        SetHealthEnabled(config->NametagsVisuals.m_ShowHealth);
        SetDistanceEnabled(config->NametagsVisuals.m_ShowDistance);
        SetEquipmentEnabled(config->NametagsVisuals.m_ShowEquipment);
        SetEffectsEnabled(config->NametagsVisuals.m_ShowEffects);
        SetMobsEnabled(config->NametagsVisuals.m_ShowMobs);
        SetAnimalsEnabled(config->NametagsVisuals.m_ShowAnimals);
        SetAutoScaleEnabled(config->NametagsVisuals.m_AutoScale);
        SetScale(config->NametagsVisuals.m_Scale);
        SetMaxDistance(config->NametagsVisuals.m_MaxDistance);
    }

#ifdef _RUNTIME
public:
    static void SetFont(ImFont* font);

    void RenderOverlay(ImDrawList* drawList, float screenW, float screenH) override;
    void ShutdownRuntime(void* envPtr) override;

private:
    static void ItemIconCallback(const ImDrawList* drawList, const ImDrawCmd* cmd);

    struct FrameIconDraw {
        jobject itemStackGlobalRef = nullptr;
        float x    = 0.0f;
        float y    = 0.0f;
        float size = 0.0f;
    };

    static ImFont* s_SanFranciscoBoldFont;

    std::vector<FrameIconDraw> m_FrameIconDraws;
    std::vector<jobject>       m_FrameItemGlobalRefs;
    std::vector<jobject>       m_PrevFrameItemGlobalRefs;
#endif

private:
    static constexpr size_t kHealthOption      = 0;
    static constexpr size_t kDistanceOption    = 1;
    static constexpr size_t kEquipmentOption   = 2;
    static constexpr size_t kEffectsOption     = 3;
    static constexpr size_t kMobsOption        = 4;
    static constexpr size_t kAnimalsOption     = 5;
    static constexpr size_t kAutoScaleOption   = 6;
    static constexpr size_t kScaleOption       = 7;
    static constexpr size_t kMaxDistanceOption = 8;

    bool IsHealthEnabled()    const { return m_Options.size() > kHealthOption    && m_Options[kHealthOption].boolValue; }
    bool IsDistanceEnabled()  const { return m_Options.size() > kDistanceOption  && m_Options[kDistanceOption].boolValue; }
    bool IsEquipmentEnabled() const { return m_Options.size() > kEquipmentOption && m_Options[kEquipmentOption].boolValue; }
    bool IsEffectsEnabled()   const { return m_Options.size() > kEffectsOption   && m_Options[kEffectsOption].boolValue; }
    bool IsMobsEnabled()      const { return m_Options.size() > kMobsOption      && m_Options[kMobsOption].boolValue; }
    bool IsAnimalsEnabled()   const { return m_Options.size() > kAnimalsOption   && m_Options[kAnimalsOption].boolValue; }
    bool IsAutoScaleEnabled() const { return m_Options.size() > kAutoScaleOption && m_Options[kAutoScaleOption].boolValue; }

    float GetScale() const {
        return m_Options.size() > kScaleOption
            ? (std::max)(0.5f, (std::min)(2.0f, m_Options[kScaleOption].floatValue))
            : 1.0f;
    }

    int GetMaxDistance() const {
        return m_Options.size() > kMaxDistanceOption
            ? (std::max)(16, (std::min)(255, m_Options[kMaxDistanceOption].intValue))
            : 64;
    }

    void SetHealthEnabled(bool v)    { if (m_Options.size() > kHealthOption)    m_Options[kHealthOption].boolValue = v; }
    void SetDistanceEnabled(bool v)  { if (m_Options.size() > kDistanceOption)  m_Options[kDistanceOption].boolValue = v; }
    void SetEquipmentEnabled(bool v) { if (m_Options.size() > kEquipmentOption) m_Options[kEquipmentOption].boolValue = v; }
    void SetEffectsEnabled(bool v)   { if (m_Options.size() > kEffectsOption)   m_Options[kEffectsOption].boolValue = v; }
    void SetMobsEnabled(bool v)      { if (m_Options.size() > kMobsOption)      m_Options[kMobsOption].boolValue = v; }
    void SetAnimalsEnabled(bool v)   { if (m_Options.size() > kAnimalsOption)   m_Options[kAnimalsOption].boolValue = v; }
    void SetAutoScaleEnabled(bool v) { if (m_Options.size() > kAutoScaleOption) m_Options[kAutoScaleOption].boolValue = v; }

    void SetScale(float v) {
        if (m_Options.size() > kScaleOption)
            m_Options[kScaleOption].floatValue = (std::max)(0.5f, (std::min)(2.0f, v));
    }

    void SetMaxDistance(int v) {
        if (m_Options.size() > kMaxDistanceOption)
            m_Options[kMaxDistanceOption].intValue = (std::max)(16, (std::min)(255, v));
    }
};
