#pragma once
#include "raylib.h"

namespace gfx {

// Ambient lighting intensity (0..2 scales the default ambient term).
void SetAmbientIntensity(float intensity);

// Scene-wide lighting settings (the Lighting section of the Explorer). Saved with the scene.
struct LightingSettings {
    float timeOfDay = 12.0f;          // hours, 0..24 (12 = noon)
    float dayLengthMinutes = 0.0f;    // real minutes per 24 h; 0 = the time stays where it is set

    // Items inserted from the Explorer's Lighting "+" menu (Time is built in). A removed item goes back to its default.
    bool hasSun = false, hasAmbient = false, hasSky = false, hasFog = false;
    float sunAzimuth = 53.13f;        // degrees, the compass direction the sun shines FROM
    float sunElevation = 63.43f;      // degrees above the horizon (the old fixed sun)
    float sunIntensity = 1.0f;        // 0..3
    float sunColor[3] = { 1.0f, 1.0f, 1.0f };
    float ambient = 1.0f;             // 0..2: scales the ambient light
    float skyColor[3] = { 0.14f, 0.15f, 0.17f };   // the daytime sky (the editor's own clear colour is used until a Sky item is inserted)
    float fogDensity = 0.0f;          // per metre; the fog takes the sky colour
    float fogHeight = 0.0f;           // metres over which the fog thins out with height; 0 = the same at every height
    bool sunFollowsTime = true;       // the sun rises in the east, peaks at sunElevation at noon and sets in the west
    bool hasWeather = false;          // the Weather item
    float overcast = 0.0f;            // 0..1 cloud cover: dimmer, softer sun, grey sky, a little haze
    float rain = 0.0f;                // 0..1 rain: falling rain, wet ground, more cloud and haze
    float wetGround = 0.0f;           // 0..1 wet roads without rain (rain wets them too)

    // The Sky item can be one colour (skyColor, above) or a physical atmosphere: air that scatters the sun's light,
    // with a sun, a moon and stars in it (shaders/sky.glsl, src/Engine/Atmosphere.cpp).
    int skyMode = 0;                  // 0 = one colour, 1 = atmosphere
    float airColor[3] = { 0.175f, 0.410f, 1.0f };   // what the air scatters most: Earth's is blue
    float airDensity = 1.0f;          // 0..4: thin air is dark overhead, thick air is pale with deep red sunsets
    float haze = 2.0f;                // 0..12: dust and moisture, the white glow near the horizon and round the sun (1 = very clear air)
    float hazeColor[3] = { 1.0f, 1.0f, 1.0f };
    float ozone = 1.0f;               // 0..3: keeps the zenith blue at dusk
    float groundColor[3] = { 0.40f, 0.42f, 0.35f }; // the land below the horizon, seen from high up (it also colours the light bounced up off the ground)
    float sunSize = 1.0f;             // 1 = a little larger than the real sun's half a degree, as films and games show it
    float moonSize = 1.0f;
    float moonPhase = 0.5f;           // 0 = new, 0.5 = full, 1 = new again
    float moonLight = 1.0f;           // 0..4: how bright moonlit nights are
    float stars = 1.0f;               // 0..4

    // The Clouds item: a layer of volumetric cloud in the atmosphere sky (shaders/clouds.glsl). The Weather item's
    // cloud cover and rain add cloud to it (or bring some of their own when there is no Clouds item).
    bool hasClouds = false;
    float cloudCoverage = 0.45f;      // 0..1: how much of the sky is cloud
    float cloudDensity = 1.0f;        // 0.2..3: thin and bright, or thick and dark underneath
    float cloudBase = 1300.0f;        // metres: the height of the cloud base
    float cloudThickness = 1700.0f;   // metres: how tall the tallest clouds grow
    float cloudScale = 1.0f;          // 0.3..3: the size of the clouds
    float windSpeed = 14.0f;          // metres a second
    float windDirection = 70.0f;      // degrees, the compass direction the wind blows towards

    // The Picture item: how the lit scene becomes the image on screen (shaders/post.glsl).
    bool hasPicture = false;
    int toneCurve = 1;                // 0 = none (clip), 1 = neutral (colours stay as given), 2 = ACES filmic, 3 = AgX
    float exposure = 0.0f;            // stops, -4..4
    bool autoExposure = true;         // the eye adapts to the brightness of the view (only with an atmosphere sky)
    float bloom = 0.4f;               // 0..1: glow around the sun, lamps, lit windows
    float vignette = 0.0f;            // 0..1: darker corners
    float contrast = 1.0f;            // 0.5..1.5
    float saturation = 1.0f;          // 0..2
    float temperature = 0.0f;         // -1 cool .. 1 warm
    float filmGrain = 0.0f;           // 0..1

    // Light nodes (SpotLight / PointLight / SurfaceLight inserted under objects): how many are lit at once, and how
    // many of those may cast shadows. Extra lights are dropped / shadowless, farthest from the camera first.
    int maxLights = 32;               // 1..32
    int maxShadowCasters = 4;         // 0..16
};
LightingSettings& Lighting();
void ResetLighting();                 // back to noon, no running cycle (a scene that stores no lighting loads like this)
// What a new project starts with: the atmosphere sky with a few clouds, a clear Weather item and the Picture
// item, so the sky, weather and picture settings are all there to change (and visible) from the first frame.
LightingSettings NewSceneLighting();
void TickLighting(float dt);          // advances the time of day when a day length is set

// Time of day in hours (0..24, 12 = noon, the default and the unchanged look). Dims the sun and ambient
// light, darkens the sky and drives the emissive window/street light/car light glow of the city shaders.
void SetTimeOfDay(float hours);
float GetTimeOfDay();
float GetNightAmount();
float LampSwitchOn(float x, float z, float night);   // 0..1: is the street lamp at this spot on? Same rule as the shader (lamps come on one by one at dusk; a few flicker)
Color SkyColor(Color dayColor);  // the clear colour at the current time of day
void SetEngineClearColor(Color c);   // the engine's own background colour (used as the sky until a Sky item is inserted)
Color CurrentSky();              // the sky colour now: the Sky item (or the engine colour) darkened for the time of day

// What the scene's lighting currently is, for shaders that do not go through the lit shader (terrain, water).
// The scene is lit in linear light: a white surface in full noon sun comes to about 1.
Vector3 SunDirection();          // unit vector the sunlight travels along
Vector3 SunRadiance();           // linear light on a surface that faces the sun (colour x intensity x day/night x cloud)
Vector3 SunRadianceShaded();     // the same with the clouds' shadow averaged in, for shaders that do not read the cloud-shadow texture themselves
Vector3 AmbientSky();            // linear light from the sky on a surface that faces up
Vector3 AmbientGround();         // linear light bounced off the ground on a surface that faces down
Vector3 SkyRadiance();           // the sky's own linear light, for reflections (water, puddles)
Vector3 SunLightScale();         // the sun's colour and strength relative to the plain noon sun (about 1,1,1 at noon)
Vector3 AmbientScale();          // the sky's light relative to the default noon ambient
float FogDensity();              // per metre: the Fog item's fog plus the weather's haze
float WeatherHaze();             // per metre: the part of it that bad weather adds
Color FogColorNow();
float GetAmbientIntensity();
Vector3 WaterSky();              // the sky's linear light the water shader reflects when the reflection is off or far (day, dusk, night, cloud)
float SunElevationNow();         // degrees above the horizon of the sun along its path (negative = below, at night)
Vector3 SunPosition();           // unit vector towards the sun in the sky (it points down when the sun is below the horizon)
Vector3 MoonPosition();          // unit vector towards the moon
bool KeyLightIsMoon();           // the scene is lit, and its shadows cast, by the moon now (SunDirection() is then the moon's)
float KeyLightStrength();        // 0..1: how much of that light arrives (it fades out round sunrise and sunset)
float DayAmountNow();            // 1 in daylight, 0 at night
float WetnessNow();              // 0..1: how wet the ground is (rain or the Weather item's wet ground)
float RainNow();                 // 0..1 rain amount
float OvercastNow();             // 0..1 cloud cover (rain adds to it)

} // namespace gfx
