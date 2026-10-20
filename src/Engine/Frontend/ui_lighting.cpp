#include "ui_internal.hpp"
#include "../../../include/Graphics/Graphics.hpp"

namespace ui {

// Settings of the selected Lighting item, shown over the properties area.
void DrawImGuiLightingPanel() {
    if (g_lightingSel == 0) return;
    Rectangle rec = GetPropertiesPanelBounds();
    if (rec.width <= 0.0f) return;
    ImGui::SetNextWindowPos(ImVec2(rec.x, rec.y));
    ImGui::SetNextWindowSize(ImVec2(rec.width, rec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LightingProps", nullptr, flags);
    ImGui::TextColored(theme::AccentVec(), "LIGHTING");
    gfx::LightingSettings& L = gfx::Lighting();
    switch (g_lightingSel) {
    case 1: {
        ImGui::TextDisabled("Time");
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##hour", &L.timeOfDay, 0.0f, 24.0f, "");
        const int totalMin = static_cast<int>(L.timeOfDay * 60.0f + 0.5f) % (24 * 60);
        ImGui::Text("Game time  %02d:%02d", totalMin / 60, totalMin % 60);
        if (ImGui::Button("Dawn")) L.timeOfDay = 6.5f;
        ImGui::SameLine();
        if (ImGui::Button("Noon")) L.timeOfDay = 12.0f;
        ImGui::SameLine();
        if (ImGui::Button("Dusk")) L.timeOfDay = 18.0f;
        ImGui::SameLine();
        if (ImGui::Button("Night")) L.timeOfDay = 23.0f;
        ImGui::Spacing();
        ImGui::TextDisabled("Day length (real minutes per day)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##daylen", &L.dayLengthMinutes, 0.1f, 0.0f, 240.0f, L.dayLengthMinutes > 0.0f ? "%.1f min" : "off (time stays put)");
        ImGui::Spacing();
        ImGui::TextWrapped("The whole scene follows this time: sun and sky, window and street lights, car and bus lights. It is saved with the scene.");
        ImGui::Spacing();
        ImGui::TextDisabled("Light nodes (SpotLight, PointLight, SurfaceLight)");
        ImGui::TextDisabled("Most lights lit at once");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##maxlights", &L.maxLights, 1, 32);
        ImGui::TextDisabled("Most shadow-casting lights");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##maxshadows", &L.maxShadowCasters, 0, 16);
        ImGui::TextWrapped("Extra lights farthest from the camera are dropped (or lose their shadows).");
        break;
    }
    case 2:
        ImGui::TextDisabled("Sun");
        ImGui::Separator();
        ImGui::Checkbox("Follow the time of day", &L.sunFollowsTime);
        ImGui::TextDisabled(L.sunFollowsTime ? "Direction at noon (compass, degrees)" : "Direction (compass, degrees)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##az", &L.sunAzimuth, 0.0f, 360.0f, "%.0f");
        ImGui::TextDisabled(L.sunFollowsTime ? "Height at noon" : "Height above the horizon");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##el", &L.sunElevation, 8.0f, 90.0f, "%.0f");
        ImGui::TextDisabled("Intensity");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##sint", &L.sunIntensity, 0.0f, 3.0f, "%.2f");
        ImGui::ColorEdit3("Colour", L.sunColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Spacing();
        if (L.sunFollowsTime) ImGui::TextWrapped("The sun rises at 6:00, crosses the sky (15 degrees of compass an hour), peaks at the height above at noon and sets at 18:00, turning orange near the horizon. Shadows move with it. Untick to pin the sun in place.");
        else ImGui::TextWrapped("Pinned: the sun stays where you put it. A low sun gives long shadows. The time of day still dims it at night.");
        break;
    case 3:
        ImGui::TextDisabled("Ambient");
        ImGui::Separator();
        ImGui::TextDisabled("Strength (1 = default)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##amb", &L.ambient, 0.0f, 2.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("The light that reaches everything, shadows included. Lower it for a moodier scene.");
        break;
    case 4: {
        ImGui::TextDisabled("Sky");
        ImGui::Separator();
        if (ImGui::RadioButton("Atmosphere", L.skyMode == 1)) L.skyMode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("One colour", L.skyMode != 1)) L.skyMode = 0;
        ImGui::Spacing();
        if (L.skyMode != 1) {
            ImGui::ColorEdit3("Daytime sky", L.skyColor, ImGuiColorEditFlags_NoInputs);
            ImGui::Spacing();
            ImGui::TextWrapped("The background colour. It darkens towards night, and the fog uses it.");
            break;
        }
        // The same air with different numbers: each preset only sets the sliders below.
        struct SkyPreset { const char* name; float air[3]; float density, haze; float hazeColor[3]; float ozone; };
        static const SkyPreset presets[] = {
            { "Earth",       { 0.175f, 0.410f, 1.0f }, 1.0f, 2.0f, { 1.0f, 1.0f, 1.0f }, 1.0f },
            { "Clear",       { 0.175f, 0.410f, 1.0f }, 1.0f, 0.6f, { 1.0f, 1.0f, 1.0f }, 1.0f },
            { "Hazy",        { 0.175f, 0.410f, 1.0f }, 1.1f, 7.0f, { 1.0f, 0.97f, 0.92f }, 1.0f },
            { "Dusty",       { 1.0f, 0.55f, 0.25f },   0.7f, 6.0f, { 1.0f, 0.70f, 0.45f }, 0.0f },
            { "Green",       { 0.25f, 1.0f, 0.45f },   1.0f, 1.5f, { 1.0f, 1.0f, 1.0f }, 0.3f },
            { "Violet",      { 0.62f, 0.30f, 1.0f },   1.2f, 2.0f, { 1.0f, 0.9f, 1.0f }, 0.0f },
            { "Thin",        { 0.175f, 0.410f, 1.0f }, 0.2f, 0.3f, { 1.0f, 1.0f, 1.0f }, 1.0f },
        };
        int col = 0;
        for (const SkyPreset& p : presets) {
            if (col++ % 4 != 0) ImGui::SameLine();
            if (ImGui::Button(p.name)) {
                for (int i = 0; i < 3; i++) { L.airColor[i] = p.air[i]; L.hazeColor[i] = p.hazeColor[i]; }
                L.airDensity = p.density; L.haze = p.haze; L.ozone = p.ozone;
            }
        }
        ImGui::Spacing();
        ImGui::ColorEdit3("What the air scatters", L.airColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
        ImGui::TextDisabled("Air (thin .. thick)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##airdensity", &L.airDensity, 0.0f, 4.0f, "%.2f");
        ImGui::TextDisabled("Haze");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##haze", &L.haze, 0.0f, 12.0f, "%.2f");
        ImGui::ColorEdit3("Haze colour", L.hazeColor, ImGuiColorEditFlags_NoInputs);
        ImGui::TextDisabled("Ozone (blue at dusk)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##ozone", &L.ozone, 0.0f, 3.0f, "%.2f");
        ImGui::ColorEdit3("Far ground", L.groundColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Spacing();
        ImGui::TextDisabled("Sun size");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##sunsize", &L.sunSize, 0.3f, 6.0f, "%.2f");
        ImGui::TextDisabled("Moon size");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##moonsize", &L.moonSize, 0.3f, 6.0f, "%.2f");
        ImGui::TextDisabled("Moon phase (new .. full .. new)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##moonphase", &L.moonPhase, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Moonlight");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##moonlight", &L.moonLight, 0.0f, 4.0f, "%.2f");
        ImGui::TextDisabled("Stars");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##stars", &L.stars, 0.0f, 4.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("Air round a planet, lit by the sun: blue overhead, pale at the horizon, red at sunset, and the sunlight on the scene takes its colour from it. The Sun item sets where the sun is and how strong; the moon is opposite it.");
        break;
    }
    case 5:
        ImGui::TextDisabled("Fog");
        ImGui::Separator();
        ImGui::TextDisabled("Density");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##fog", &L.fogDensity, 0.0f, 0.03f, "%.4f");
        ImGui::TextDisabled("Height it thins over (m)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##fogheight", &L.fogHeight, 0.0f, 1500.0f, L.fogHeight > 0.0f ? "%.0f" : "the same at every height");
        ImGui::Spacing();
        ImGui::TextWrapped("Distant things fade into the fog. About 0.004 is a light haze, 0.02 is thick. With a height the fog lies on the ground and the sky shows above it; under an atmosphere sky the sun lights it, and shadows cut shafts through it.");
        break;
    case 7:
        ImGui::TextDisabled("Clouds");
        ImGui::Separator();
        if (ImGui::Button("Few")) { L.cloudCoverage = 0.22f; L.cloudDensity = 1.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Scattered")) { L.cloudCoverage = 0.45f; L.cloudDensity = 1.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Broken")) { L.cloudCoverage = 0.68f; L.cloudDensity = 1.2f; }
        ImGui::SameLine();
        if (ImGui::Button("Overcast")) { L.cloudCoverage = 1.0f; L.cloudDensity = 1.5f; }
        ImGui::Spacing();
        ImGui::TextDisabled("Cover");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudcover", &L.cloudCoverage, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Thickness of the cloud itself");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##clouddensity", &L.cloudDensity, 0.2f, 3.0f, "%.2f");
        ImGui::TextDisabled("Base height (m)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudbase", &L.cloudBase, 200.0f, 8000.0f, "%.0f");
        ImGui::TextDisabled("Tallest clouds (m)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudthick", &L.cloudThickness, 300.0f, 5000.0f, "%.0f");
        ImGui::TextDisabled("Size");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudscale", &L.cloudScale, 0.3f, 3.0f, "%.2f");
        ImGui::TextDisabled("Wind (m/s)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##windspeed", &L.windSpeed, 0.0f, 60.0f, "%.0f");
        ImGui::TextDisabled("Wind blows towards (compass, degrees)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##winddir", &L.windDirection, 0.0f, 360.0f, "%.0f");
        ImGui::Spacing();
        if (!(L.hasSky && L.skyMode == 1)) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Clouds need the Sky item set to Atmosphere.");
        ImGui::TextWrapped("A layer of cloud you can fly through and above. The sun and the sky light it, its shadow drifts over the ground, and the Weather item's cloud cover and rain add to it.");
        break;
    case 8: {
        ImGui::TextDisabled("Picture");
        ImGui::Separator();
        if (ImGui::Button("Neutral")) { L.toneCurve = 1; L.contrast = 1.0f; L.saturation = 1.0f; L.temperature = 0.0f; L.vignette = 0.0f; L.filmGrain = 0.0f; L.bloom = 0.4f; }
        ImGui::SameLine();
        if (ImGui::Button("Filmic")) { L.toneCurve = 2; L.contrast = 1.04f; L.saturation = 1.05f; L.temperature = 0.05f; L.vignette = 0.25f; L.filmGrain = 0.08f; L.bloom = 0.5f; }
        ImGui::SameLine();
        if (ImGui::Button("Soft")) { L.toneCurve = 3; L.contrast = 1.0f; L.saturation = 1.08f; L.temperature = 0.0f; L.vignette = 0.15f; L.filmGrain = 0.0f; L.bloom = 0.6f; }
        ImGui::SameLine();
        if (ImGui::Button("Vivid")) { L.toneCurve = 1; L.contrast = 1.1f; L.saturation = 1.25f; L.temperature = 0.0f; L.vignette = 0.1f; L.filmGrain = 0.0f; L.bloom = 0.5f; }
        ImGui::Spacing();
        ImGui::TextDisabled("Tone curve");
        ImGui::SetNextItemWidth(-1.0f);
        const char* curves[] = { "None (clips)", "Neutral (colours as given)", "ACES filmic", "AgX" };
        ImGui::Combo("##tonecurve", &L.toneCurve, curves, 4);
        ImGui::TextDisabled("Exposure (stops)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##exposure", &L.exposure, -4.0f, 4.0f, "%+.2f");
        ImGui::Checkbox("The eye adapts to the light", &L.autoExposure);
        ImGui::TextDisabled("Bloom");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##bloom", &L.bloom, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Contrast");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##contrast", &L.contrast, 0.5f, 1.5f, "%.2f");
        ImGui::TextDisabled("Saturation");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##saturation", &L.saturation, 0.0f, 2.0f, "%.2f");
        ImGui::TextDisabled("Temperature (cool .. warm)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##temperature", &L.temperature, -1.0f, 1.0f, "%+.2f");
        ImGui::TextDisabled("Vignette");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##vignette", &L.vignette, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Film grain");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##grain", &L.filmGrain, 0.0f, 1.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("How the lit scene becomes the picture. The scene is lit in real light levels (the sun's disc and lamps are far brighter than white paper); the tone curve brings that onto the screen and bloom spreads what is very bright. The eye only adapts under an atmosphere sky. How much the machine spends on all this is in Preferences > Rendering.");
        break;
    }
    case 6:
        ImGui::TextDisabled("Weather");
        ImGui::Separator();
        if (ImGui::Button("Clear")) { L.overcast = 0.0f; L.rain = 0.0f; L.wetGround = 0.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Overcast")) { L.overcast = 0.8f; L.rain = 0.0f; L.wetGround = 0.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Rain")) { L.overcast = 0.6f; L.rain = 0.5f; }
        ImGui::SameLine();
        if (ImGui::Button("Storm")) { L.overcast = 1.0f; L.rain = 1.0f; }
        ImGui::Spacing();
        ImGui::TextDisabled("Cloud cover");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloud", &L.overcast, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Rain");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##rain", &L.rain, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Wet ground (rain wets it too)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##wet", &L.wetGround, 0.0f, 1.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("Clouds dim and soften the sun and grey the sky. Rain falls around the camera, makes roads dark with puddles that mirror the sky, adds haze, and the city's pedestrians put up umbrellas.");
        break;
    default: break;
    }
    ImGui::End();
}

} // namespace ui
