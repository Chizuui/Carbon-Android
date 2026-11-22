#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish, Indonesian. Indonesian is the
// recomp's own text only: the game files have none, so the game itself stays in English
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};
inline bool KnownLanguage(const std::string& code) {
    return std::find(std::begin(kLanguages), std::end(kLanguages), code) != std::end(kLanguages);
}

// SSAA renders at twice the height, the presenter averages it down
inline constexpr uint32_t kAaOff = 0, kAaFxaa = 1, kAaSsaa = 2;

struct Options {
    bool fps_counter = false;
    uint32_t fps_position = 0;  // the counter's place, src/common/menu.cpp's kFpsPositionNames
    bool fps_usage = true;      // the counter's CPU and GPU load
    bool fps_temps = true;      // the counter's CPU, GPU and battery temperatures
    uint32_t overlay_layout = 0;  // the counter's shape: 0 vertical (stacked), 1 horizontal (one line)
    uint32_t screen_filter = 0;  // the present's look: 0 none, 1 sharpen, 2 vivid, 3 scanlines, 4 CRT, 5 HDR
    uint32_t anti_aliasing = 0;  // kAaOff, kAaFxaa or kAaSsaa
    uint32_t aspect_ratio = 0;  // the frame's shape in hundredths (133 is 4:3), 0 the whole screen; common/aspect.h
    uint32_t brightness = 100;  // the present's gamma, percent: the frame's own too dark, native/vulkan.cpp
    bool bloom = true;  // the glow added over the frame, native/vulkan.cpp skips it when off
    bool fog = true;    // the distance fog, native/vulkan.cpp draws without it when off
    // the cars' reflection, six more views: drawn every frame (0), every 2nd (1), every 4th (2) or off (3, a grey
    // cube read instead); the per-game ApplyGraphicsSwitches (src/carbon/carbon.cpp for Carbon)
    uint32_t reflections = 0;
    bool particles = true;    // tyre smoke, sparks and crash debris, the same
    bool shadows = true;      // the shadows under the cars, the same; the trees' and buildings', native/vulkan.cpp
    bool road_reflections = true;  // the wet road's reflection, the same
    bool rain = true;         // the falling rain, the per-game rain hook
    bool rain_drops = true;   // the drops on the screen in the rain, the same
    bool yellow_tint = false;  // the game's yellow colour grading, Most Wanted only (src/game/game.cpp greys it when off); Carbon has no tint hook, the row is hidden there
    bool shader_preload = false;  // native/vulkan.cpp makes earlier runs' shaders and pipelines at the start
    bool motion_blur = true;   // the blur by speed and the nitrous' radial blur, the per-game motion-blur hook skips them when off
    bool light_flares = false;  // the headlight, street light and neon flares, src/carbon/carbon.cpp skips eRenderLightFlares when off
    // cheats, the per-game ApplyCheats
    bool cheat_nos = false;           // the nitrous never runs out
    bool cheat_speedbreaker = false;  // the speedbreaker never runs out
    bool cheat_cash = false;          // the career's cash kept at kCheatCash
    bool cheat_no_police = false;     // no new cops or pursuits
    bool skip_movies = false;     // currently does nothing: the 0x46D7A0 flag it used to write is unproven as the skip-movies flag, so the write was removed (src/carbon/carbon.cpp)
    uint32_t audio_output = 0;  // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    std::string language = "int";  // one of kLanguages
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
    std::map<std::string, std::string> pad;   // action id to a controller's input, absent is SDL's mapping
};
Options& Opt();
// the language setting as this run started, the language the game boots in
const std::string& BootLanguage();
bool LanguageAvailable(const std::string& code);  // src/game/game.cpp: the LANGUAGE row offers it
inline std::atomic<uint32_t> g_road_reflection{0};  // the road reflection target's data, virtual; game.cpp, each present
inline std::atomic<uint32_t> g_env_cube{0};         // the cars' reflection cube's data, the same
inline std::atomic<uint32_t> g_env_cube_tex{0};     // the cars' reflection cube's texture object, per game; 0 unknown
inline std::atomic<uint32_t> g_static_env_tex{0};  // the static env map's texture object (STATICENVMAP), per game; 0 unknown
inline std::atomic<uint32_t> g_garage_env_tex{0};  // the garage/frontend env map's texture object, per game; 0 unknown
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits while the app is in the background
inline std::atomic<uint32_t> g_focus_pauses{0};  // focus pauses ended so far
inline std::atomic<bool> g_flare_pass{false};  // the eRenderLightFlares pass is running; d3d logs its draws
void WaitWhileUnfocused();  // native/game_host.cpp: game thread, before the present
void WidenAspect();         // the per-game hooks: game thread, at each present
void ApplyGraphicsSwitches();  // the per-game hooks: game thread, at each present
void ApplyCheats();            // the per-game hooks: game thread, at each present
bool Racing();    // the per-game hooks, any thread: the game flow is in a race (free roam too), not loading or the front end

}  // namespace game
