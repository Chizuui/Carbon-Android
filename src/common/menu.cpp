// the settings: their rows (app::rows, which the game's options menu shows, src/game/options_menu.cpp) and the panel on F10
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // ClearActiveID, the drag scroll's

#include "app/app.h"
#include "audio/output.h"
#include "common/aspect.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/pad_binds.h"
#include "common/translations.h"
#include "core/log.h"

// native/gyro.cpp: steering by tilting the phone
namespace gyro {
uint32_t TiltAngle();  // degrees for a full lock
void StepTiltAngle(int step);
}  // namespace gyro

// native/touch.cpp: the touch controls' layout
namespace touch {
uint32_t ControlsOpacity();
void StepOpacity(int step);
uint32_t ControlsSize();
void StepSize(int step);
const char* StickModeName();
void StepStickMode(int step);
const char* SteeringName();
void StepSteering(int step);
void EditLayout();
}  // namespace touch

// native/update.cpp: the activity asks GitHub for a newer release and says what it found
namespace host {
void CheckForUpdates();
// native/stats.cpp: the performance overlay's numbers, percent and degrees, -1 where the device does not tell
void SamplePerf();
float CpuLoad();
float GpuLoad();
float CpuTemp();
float GpuTemp();
float BatteryTemp();
}  // namespace host

namespace app {

Settings DefaultSettings();  // native/main.cpp: a phone's first run, unlimited frame rate and vsync off

namespace {

std::atomic<bool> g_open{false};

// EXPORT LOG: the current run's log, copied where the player can send it. The log already lives in the shared
// logs folder, but a tester has to know which of its files is this run's; the export is one clearly named copy
// in Download, where a file manager shows it first. If Download is not writable (no all-files access yet), the
// copy lands beside the log itself instead. The previous session's log, kept at startup so a crash survives a
// relaunch, goes along with it under its own name when there is one. The row shows EXPORT, then SAVED or
// FAILED; the full paths are in the log
std::string g_export_log_status = "EXPORT";

void ExportLog() {
    logx::Flush();
    std::filesystem::path src = logx::Path();
    if (src.empty()) {
        g_export_log_status = "FAILED";
        LOGE("export log: no log file");
        return;
    }
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    // one copy, in Download when writable, else beside the log itself; empty when neither works
    auto copy = [&](const std::filesystem::path& from, const std::string& stem) {
        char name[64];
        std::snprintf(name, sizeof(name), "%s-%04d%02d%02d-%02d%02d%02d.log", stem.c_str(), tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        std::error_code ec;
        // the log sits in <shared>/logs, so its parent's parent's parent is the storage root, and Download
        // is beside it
        std::filesystem::path download = from.parent_path().parent_path().parent_path() / "Download" / name;
        std::filesystem::create_directories(download.parent_path(), ec);
        ec.clear();
        std::filesystem::copy_file(from, download, ec);
        if (!ec) return download;
        ec.clear();
        std::filesystem::path dst = from.parent_path() / name;
        std::filesystem::copy_file(from, dst, ec);
        if (ec) return std::filesystem::path{};
        return dst;
    };
    std::filesystem::path dst = copy(src, std::string(kGame.log) + "-log");
    if (dst.empty()) {
        g_export_log_status = "FAILED";
        LOGE("export log: cannot copy %s", src.string().c_str());
        return;
    }
    g_export_log_status = "SAVED";
    LOGR("export log: %s -> %s", src.string().c_str(), dst.string().c_str());
    std::string prev = logx::PrevPath();
    if (!prev.empty()) {
        std::filesystem::path pdst = copy(std::filesystem::path(prev), std::string(kGame.log) + "-prev-log");
        if (pdst.empty()) {
            LOGE("export log: cannot copy %s", prev.c_str());
        } else {
            LOGR("export log: %s -> %s", prev.c_str(), pdst.string().c_str());
        }
    }
}

// near-black warm panel, gold on dark red kept from the old panel, red only for the active nav and selections
constexpr ImVec4 kPanelBg{0.055f, 0.05f, 0.045f, 0.97f};
constexpr ImVec4 kBorder{0.80f, 0.62f, 0.22f, 0.85f};
constexpr ImVec4 kText{0.92f, 0.88f, 0.82f, 1.0f};
constexpr ImVec4 kHighlight{0.42f, 0.07f, 0.05f, 1.0f};
constexpr ImVec4 kHeading{0.92f, 0.72f, 0.28f, 1.0f};
constexpr ImVec4 kDim{0.64f, 0.56f, 0.50f, 1.0f};
constexpr ImVec4 kTab{0.10f, 0.09f, 0.08f, 1.0f};
constexpr ImVec4 kTabHovered{0.16f, 0.13f, 0.10f, 1.0f};
constexpr ImVec4 kButton{0.16f, 0.13f, 0.11f, 1.0f};
// lighter than the highlight, so it shows on a selected row
constexpr ImVec4 kButtonHovered{0.35f, 0.22f, 0.10f, 1.0f};
constexpr ImVec4 kLine{0.92f, 0.72f, 0.28f, 0.35f};
// gold fill, for the active sidebar page and lit test inputs; the cards' own background
constexpr ImVec4 kGold{0.85f, 0.63f, 0.20f, 1.0f};
constexpr ImVec4 kCardBg{0.09f, 0.08f, 0.07f, 1.0f};
constexpr ImVec4 kDarkText{0.10f, 0.08f, 0.05f, 1.0f};
constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kTab}, {ImGuiCol_FrameBgHovered, kTabHovered}, {ImGuiCol_FrameBgActive, kHighlight},
    {ImGuiCol_ScrollbarGrab, kHeading}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
    {ImGuiCol_ChildBg, kCardBg}, {ImGuiCol_SliderGrab, kGold}, {ImGuiCol_SliderGrabActive, kHeading},
    {ImGuiCol_CheckMark, kGold},
};

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kScreenFilters[] = {0, 1, 2, 3, 4, 5};
const char* const kScreenFilterNames[] = {"OFF", "SHARPEN", "VIVID", "SCANLINES", "CRT", "HDR"};
constexpr uint32_t kAntiAliasings[] = {game::kAaOff, game::kAaFxaa, game::kAaSsaa};
const char* const kAntiAliasingNames[] = {"OFF", "FXAA", "SSAA 2X"};
// Options::reflections: how often the cars' reflection is drawn
constexpr uint32_t kReflectionRates[] = {0, 1, 2, 3};
const char* const kReflectionRateNames[] = {"FULL", "HALF", "QUARTER", "OFF"};
// the counter's place on the screen, Options::fps_position; top centre is under the settings gear
enum FpsPosition : uint32_t { kFpsTop, kFpsTopLeft, kFpsTopRight, kFpsBottomLeft, kFpsBottom, kFpsBottomRight };
constexpr uint32_t kFpsPositions[] = {kFpsTop, kFpsTopLeft, kFpsTopRight, kFpsBottomLeft, kFpsBottom, kFpsBottomRight};
const char* const kFpsPositionNames[] = {"TOP CENTER", "TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM CENTER",
                                         "BOTTOM RIGHT"};
// the overlay's shape, Options::overlay_layout; vertical stacks the numbers, horizontal puts them on one line
enum OverlayLayout : uint32_t { kOverlayVertical, kOverlayHorizontal };
constexpr uint32_t kOverlayLayouts[] = {kOverlayVertical, kOverlayHorizontal};
const char* const kOverlayLayoutNames[] = {"VERTICAL", "HORIZONTAL"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
const char* const kLanguageNames[] = {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH",
                                      "INDONESIAN"};  // as game::kLanguages
constexpr const char* kDonateUrl = "https://sociabuzz.com/chizuui/tribe";
constexpr const char* kGithubUrl = "https://github.com/Chizuui/Carbon-Android";
constexpr const char* kYoutubeUrl = "https://youtube.com/@sashachan1?si=hCetsT_IV_9tSrpa";
constexpr const char* kDiscordUrl = "https://discord.gg/4DTA7nVfh7";
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, kFpsCapRefresh, 0};

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// index nearest value, stepped first
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// value clamped to lo to hi, stepped by inc through the values StepIndex wraps over
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

// a section's card: a bordered, rounded child with a gold heading; rows drawn inside size to the card
void CardBegin(const char* label) {
    float em = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCardBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, em * 0.35f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.9f, em * 0.6f));
    ImGui::BeginChild(label, ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(Ui(label).c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.15f));
}

void CardEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
}

constexpr float kValueEms = 12.0f;  // the value column's width

// a row's place and size: the label left, the value block (an arrow each side of the value) right
struct RowBox {
    float em;
    float row_w;
    float row_h;
    float value_w;
    float block_w;
    float row_x;
    float row_y;
};

RowBox MeasureRow() {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box;
    box.em = ImGui::GetFontSize();
    box.row_w = ImGui::GetContentRegionAvail().x;
    box.row_h = ImGui::GetFrameHeight();
    box.value_w = box.em * kValueEms;
    box.block_w = box.row_h * 2 + box.value_w + style.ItemSpacing.x * 2;
    box.row_x = ImGui::GetCursorPosX();
    box.row_y = ImGui::GetCursorPosY();
    return box;
}

void RowLabel(const RowBox& box, const char* label) {
    ImGui::SetCursorPos(ImVec2(box.row_x + ImGui::GetStyle().ItemInnerSpacing.x, box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Ui(label).c_str());
}

// the cursor back to the row's start, then past the whole row
void EndRow(const RowBox& box) {
    ImGui::SetCursorPos(ImVec2(box.row_x, box.row_y));
    ImGui::Dummy(ImVec2(box.row_w, box.row_h));
}

void FillItem() {
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(ImGuiCol_Header));
}

// label left, arrows and value right, the row is the only nav stop
int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h));
    if (ImGui::IsItemFocused()) {
        FillItem();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    RowLabel(box, label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);  // held arrows keep stepping
    float block_x = box.row_x + box.row_w - box.block_w;
    ImGui::SetCursorPos(ImVec2(block_x, box.row_y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + box.row_h + style.ItemSpacing.x;
    float pad = (box.value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(value_x + box.value_w + style.ItemSpacing.x, box.row_y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::PopID();
    EndRow(box);
    return step;
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

void Flip(bool& on, int step) {
    if (step) on = !on;
}

// applies and saves
void Changed() {
    audio::SetOutputMode(game::Opt().audio_output);
    Save();
}

// label left, a slider right, laid out as the option rows are; true when the value changed
bool SliderValueRow(const char* label, int* value, int lo, int hi, const char* format) {
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    RowLabel(box, label);
    float block_x = box.row_x + box.row_w - box.block_w;
    ImGui::SetCursorPos(ImVec2(block_x, box.row_y));
    ImGui::SetNextItemWidth(box.block_w);
    bool changed = ImGui::SliderInt("##slider", value, lo, hi, format);
    ImGui::PopID();
    EndRow(box);
    return changed;
}

// one tap for a weak phone (0), balanced (1), or everything on (2); the brightness stays as the player set it
void ApplyPreset(int which) {
    game::Options& o = game::Opt();
    if (which == 0) {
        // half the render pixels, no bloom or motion blur, the car reflections only every fourth frame
        o.bloom = false;
        o.motion_blur = false;
        o.reflections = 2;
        o.screen_filter = 0;
        Get().render_percent = 50;
    } else if (which == 1) {
        // two thirds of the render pixels, bloom on, no motion blur, the car reflections every second frame
        o.bloom = true;
        o.motion_blur = false;
        o.reflections = 1;
        o.screen_filter = 0;
        Get().render_percent = 67;
    } else {
        // full render pixels, bloom and motion blur on, the car reflections every frame
        o.bloom = true;
        o.motion_blur = true;
        o.reflections = 0;
        o.screen_filter = 0;
        Get().render_percent = 100;
    }
    Changed();
}

enum RowId { kRenderResolution, kMaxFps, kVsync, kFpsCounter, kAniso, kAudioOutput, kLanguage, kGyro,
             kGyroSensitivity, kTouchOpacity, kTouchSize, kTouchEdit, kCheckUpdates, kAspectRatio, kScreenFilter,
             kAntiAliasing, kFpsPosition, kFpsTemps, kStickMode, kBloom, kFog, kDonate, kGithub, kYoutube, kFpsUsage,
             kReflections, kParticles, kRain, kRainDrops, kShadows, kRoadReflections,
             kCheatNos, kCheatSpeedbreaker, kCheatCash, kCheatNoPolice, kCheatUnlockAll,
             kSkipMovies, kYellowTint, kMotionBlur, kLightFlares, kShaderPreload, kBrightness, kLowEndPreset,
             kMidPreset, kHighPreset, kExportLog, kDiscord, kOverlayLayout };

using rows::Kind;
using rows::Row;

// the recomp's settings in pages of their own, grouped within; the game's options menu shows the headings too
constexpr Row kGraphicsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION"},
    {Kind::Value, kAspectRatio, "ASPECT RATIO"},
    {Kind::Value, kBrightness, "BRIGHTNESS"},
    {Kind::Heading, -1, "PERFORMANCE"},
    {Kind::Link, kLowEndPreset, "LOW-END PRESET"},
    {Kind::Link, kMidPreset, "MID PRESET"},
    {Kind::Link, kHighPreset, "HIGH PRESET"},
    {Kind::Value, kVsync, "VERTICAL SYNC"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE"},
    {Kind::Value, kShaderPreload, "SHADER PRELOADING"},
    {Kind::Heading, -1, "OVERLAY PERFORMA"},
    {Kind::Value, kFpsCounter, "FPS COUNTER"},
    {Kind::Value, kOverlayLayout, "OVERLAY LAYOUT"},
    {Kind::Value, kFpsPosition, "FPS COUNTER POSITION"},
    {Kind::Value, kFpsUsage, "SHOW CPU / GPU USAGE"},
    {Kind::Value, kFpsTemps, "SHOW TEMPERATURES"},
    {Kind::Heading, -1, "QUALITY"},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING"},
    {Kind::Value, kAntiAliasing, "ANTI-ALIASING"},
    {Kind::Value, kBloom, "BLOOM"},
    {Kind::Value, kFog, "FOG"},
    {Kind::Value, kReflections, "CAR REFLECTIONS"},
    {Kind::Value, kRoadReflections, "ROAD REFLECTIONS"},
    {Kind::Value, kShadows, "SHADOWS"},
    {Kind::Value, kParticles, "PARTICLE EFFECTS"},
    {Kind::Value, kRain, "FALLING RAIN"},
    {Kind::Value, kRainDrops, "SCREEN RAINDROPS"},
    {Kind::Value, kYellowTint, "YELLOW TINT"},
    {Kind::Value, kMotionBlur, "MOTION BLUR"},
    {Kind::Value, kLightFlares, "LIGHT FLARES"},
    {Kind::Value, kScreenFilter, "SCREEN FILTER"},
};

constexpr Row kTouchRows[] = {
    {Kind::Heading, -1, "TOUCH"},
    {Kind::Value, kGyro, "STEERING"},
    {Kind::Value, kGyroSensitivity, "TILT ANGLE"},
    {Kind::Value, kStickMode, "STICK MODE"},
    {Kind::Value, kTouchOpacity, "CONTROLS OPACITY"},
    {Kind::Value, kTouchSize, "CONTROLS SIZE"},
    {Kind::Link, kTouchEdit, "EDIT CONTROLS POSITION"},
};

// LANGUAGE is the settings panel's: the game takes its own from the console's language
constexpr Row kGeneralRows[] = {
    {Kind::Value, kLanguage, "LANGUAGE"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT"},
    {Kind::Value, kSkipMovies, "SKIP MOVIES"},
    {Kind::Link, kExportLog, "EXPORT LOG"},
    {Kind::Link, kDiscord, "JOIN DISCORD"},
};

// the panel's alone: the game's options menu (src/game/options_menu.cpp) shows no section of it
constexpr Row kCheatRows[] = {
    {Kind::Value, kCheatNos, "INFINITE NITROUS"},
    {Kind::Value, kCheatSpeedbreaker, "INFINITE SPEEDBREAKER"},
    {Kind::Value, kCheatCash, "MAX CASH"},
    {Kind::Value, kCheatNoPolice, "NO POLICE"},
    {Kind::Value, kCheatUnlockAll, "UNLOCK ALL"},
};

// the credits' names, an info row's id; as they are, untranslated
const char* const kCreditNames[] = {"Rangga Casper", "Zukiru", "bagasromadon", "TGP482",
                                    "SDL, glslang, toml++, Dear ImGui", "xemu, Cxbx-Reloaded"};

// a page of its own: who made the port, and the links to support it
constexpr Row kCreditRows[] = {
    {Kind::Heading, -1, "CREDITS"},
    {Kind::Info, 0, "ANDROID PORT"},
    {Kind::Info, 1, "HDR"},
    {Kind::Info, 2, "BUTTON DESIGN"},
    {Kind::Info, 3, "RECOMPILER AND RUNTIME"},
    {Kind::Info, 4, "LIBRARIES"},
    {Kind::Info, 5, "XBOX REFERENCES"},
    {Kind::Heading, -1, "SUPPORT"},
    {Kind::Link, kDonate, "DONATE"},
    {Kind::Link, kGithub, "GITHUB"},
    {Kind::Link, kYoutube, "YOUTUBE"},
};

const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "KEYBOARD"}};
        size_t count = 0;
        const input::Action* actions = input::Actions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

const std::vector<Row>& PadRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "CONTROLLER"}};
        size_t count = 0;
        const input::Action* actions = input::PadActions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::PadBind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

// the setting an ON / OFF row flips, nullptr for any other row
bool* ToggleFlag(Settings& s, int id) {
    game::Options& o = game::Opt();
    switch (id) {
        case kVsync: return &s.vsync;
        case kFpsCounter: return &o.fps_counter;
        case kFpsTemps: return &o.fps_temps;
        case kFpsUsage: return &o.fps_usage;
        case kBloom: return &o.bloom;
        case kFog: return &o.fog;
        case kParticles: return &o.particles;
        case kShadows: return &o.shadows;
        case kRoadReflections: return &o.road_reflections;
        case kRain: return &o.rain;
        case kRainDrops: return &o.rain_drops;
        case kYellowTint: return &o.yellow_tint;
        case kMotionBlur: return &o.motion_blur;
        case kLightFlares: return &o.light_flares;
        case kShaderPreload: return &o.shader_preload;
        case kCheatNos: return &o.cheat_nos;
        case kCheatSpeedbreaker: return &o.cheat_speedbreaker;
        case kCheatCash: return &o.cheat_cash;
        case kCheatNoPolice: return &o.cheat_no_police;
        case kCheatUnlockAll: return &o.cheat_unlock_all;
        case kSkipMovies: return &o.skip_movies;
        default: return nullptr;
    }
}

// the display rows that are no ON / OFF, stepped first; false for another row
bool StepDisplayRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kRenderResolution: {
            // 20% to 200% in steps of 5
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
            Mode out = Output();
            // the 3D render is the 640x480 guest scaled, not the display scaled: the height is the display
            // height times the percent, the width is that height times the guest's 4:3 aspect
            uint32_t h = out.height * s.render_percent / 100;
            uint32_t w = h * 4 / 3;
            std::snprintf(text, sizeof(text), "%u%% (%ux%u)", s.render_percent, w, h);
            return true;
        }
        case kAspectRatio: {
            // 1:1 to the whole screen, an unknown saved value the whole screen's
            constexpr int count = int(std::size(game::kAspects));
            int i = count - 1;
            for (int j = 0; j < count; j++)
                if (game::kAspects[j].code == o.aspect_ratio) i = j;
            if (step) {
                i = StepIndex(i, step, count);
                o.aspect_ratio = game::kAspects[i].code;
            }
            std::snprintf(text, sizeof(text), "%s", game::kAspects[i].name);
            return true;
        }
        case kMaxFps: {
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            if (cap == kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), cap == 60 ? "%u (DEFAULT)" : "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED");
            return true;
        }
        default:
            return false;
    }
}

// the graphics rows that are no ON / OFF, stepped first; false for another row
bool StepGraphicsRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            return true;
        case kAntiAliasing:
            std::snprintf(text, sizeof(text), "%s", kAntiAliasingNames[StepTable(kAntiAliasings, o.anti_aliasing, step)]);
            return true;
        case kScreenFilter:
            std::snprintf(text, sizeof(text), "%s", kScreenFilterNames[StepTable(kScreenFilters, o.screen_filter, step)]);
            return true;
        case kBrightness: {
            // the frame's gamma, 50% to 200%: the game is dark on a phone's screen
            if (step) o.brightness = uint32_t(StepRange(int(o.brightness), 50, 200, 5, step));
            std::snprintf(text, sizeof(text), "%u%%", o.brightness);
            return true;
        }
        case kReflections:
            std::snprintf(text, sizeof(text), "%s", kReflectionRateNames[StepTable(kReflectionRates, o.reflections, step)]);
            return true;
        case kFpsPosition:
            std::snprintf(text, sizeof(text), "%s", kFpsPositionNames[StepTable(kFpsPositions, o.fps_position, step)]);
            return true;
        case kOverlayLayout:
            std::snprintf(text, sizeof(text), "%s", kOverlayLayoutNames[StepTable(kOverlayLayouts, o.overlay_layout, step)]);
            return true;
        default:
            return false;
    }
}

// the general and touch rows that are no ON / OFF, stepped first
void StepInputRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, o.audio_output, step)]);
            break;
        case kGyro:
            // the RACING layout's arrows, its wheel, or the phone tilted
            if (step) touch::StepSteering(step);
            std::snprintf(text, sizeof(text), "%s", touch::SteeringName());
            break;
        case kStickMode:
            // floating sticks come where the thumb lands, fixed ones stay where they are drawn
            if (step) touch::StepStickMode(step);
            std::snprintf(text, sizeof(text), "%s", touch::StickModeName());
            break;
        case kTouchOpacity:
            if (step) touch::StepOpacity(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsOpacity());
            break;
        case kTouchSize:
            if (step) touch::StepSize(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsSize());
            break;
        case kCheckUpdates:
            // any step asks; the answer comes in a dialog over the game
            if (step) host::CheckForUpdates();
            std::snprintf(text, sizeof(text), "CHECK");
            break;
        case kGyroSensitivity:
            // the phone's tilt for a full lock, 10 to 60 degrees: less turns quicker
            if (step) gyro::StepTiltAngle(step);
            std::snprintf(text, sizeof(text), "%u\xC2\xB0", gyro::TiltAngle());
            break;
        case kLanguage: {
            int count = int(std::size(game::kLanguages));
            int i = 0;
            while (i < count - 1 && o.language != game::kLanguages[i]) i++;
            // past the languages this disc has no strings for; English and Indonesian are always there
            if (step) {
                do i = StepIndex(i, step, count);
                while (!game::LanguageAvailable(game::kLanguages[i]));
                o.language = game::kLanguages[i];
            }
            const char* format = o.language == game::BootLanguage() ? "%s" : "%s (RESTART)";
            std::snprintf(text, sizeof(text), format, kLanguageNames[i]);
            break;
        }
    }
}

// steps the value first when step is not zero
std::string RowText(int id, int step, Settings& s = Get()) {
    char text[64] = "";
    if (bool* on = ToggleFlag(s, id)) {
        Flip(*on, step);
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (!StepDisplayRow(id, step, s, text) && !StepGraphicsRow(id, step, s, text)) {
        StepInputRow(id, step, text);
    }
    if (step && &s == &Get()) Changed();
    return text;
}

constexpr const char* kUnbound = "UNBOUND";

std::string KeyText(std::string keys) {
    if (keys.empty()) return kUnbound;
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

// as options_menu.cpp's captions
const char* const kPages[] = {"GRAPHICS", "TOUCH CONTROLS", "GENERAL", "KEYBOARD", "CONTROLLER", "CREDITS", "CHEATS"};
constexpr int kSettingsPage = 0;
constexpr int kTouchPage = 1;
constexpr int kGeneralPage = 2;
constexpr int kKeyboardPage = 3;
constexpr int kControllerPage = 4;
constexpr int kCreditsPage = 5;
constexpr int kCheatsPage = 6;

void RestoreDefaults(int page) {
    if (page == kCreditsPage) return;  // nothing to reset
    game::Options& o = game::Opt();
    if (page == kCheatsPage) {
        game::Options none;
        o.cheat_nos = none.cheat_nos;
        o.cheat_speedbreaker = none.cheat_speedbreaker;
        o.cheat_cash = none.cheat_cash;
        o.cheat_no_police = none.cheat_no_police;
        o.cheat_unlock_all = none.cheat_unlock_all;
        Changed();
        return;
    }
    if (page == kKeyboardPage) {
        input::ResetBindings();
        return;
    }
    if (page == kControllerPage) {
        input::ResetPadBindings();
        return;
    }
    game::Options keep = o;
    Get() = DefaultSettings();
    o = game::Options{};
    o.keys = keep.keys;
    o.pad = keep.pad;
    Changed();
}

// the row being rebound, -1 when idle; its keys must be released before the next press binds
std::atomic<int> g_capture{-1};
int g_capture_slot = 0;  // its box: 0 the key, 1 the alternate
int g_hovered_box = -1;  // the box under the mouse or focused with a key in it, this frame
bool g_capture_armed = false;
int g_capture_done = 0;  // frame of the last bind, the key that bound must not start another
// the controller row being rebound, -1 when idle: as the keys', its inputs let go before the next press binds; Escape
// or kPadCaptureMs with no press cancels, for a player with the controller alone
std::atomic<int> g_pad_capture{-1};
bool g_pad_capture_armed = false;
std::chrono::steady_clock::time_point g_pad_capture_since;
constexpr std::chrono::seconds kPadCaptureTimeout{5};
// in Most Wanted, settings were embedded in the in-game options menu;
// in Carbon, all pages (GRAPHICS, TOUCH CONTROLS, GENERAL, KEYBOARD, CONTROLLER, CREDITS, CHEATS)
// are fully accessible right here from the Gear panel via top tabs!
int g_page = kSettingsPage;
// the action's key and its alternate as their boxes show them, the alternate blank when there is none
std::vector<std::string> KeyBoxes(size_t action) {
    std::string keys = input::Binding(action);
    size_t comma = keys.find(',');
    std::string second;
    if (comma != std::string::npos) second = keys.substr(comma + 1);
    second = second.substr(0, second.find(','));
    if (!second.empty()) second = KeyText(second);
    return {KeyText(keys.substr(0, comma)), second};
}

constexpr float kKeyBoxEms = 10.0f;  // a key box's width when the row has two

// label left, a box per key right (the key and its alternate side by side), ending where the other rows' values do;
// Enter or click starts a box's capture, right click or Delete sets `cleared`; returns the box, -1 for none
int BindRow(const char* label, const std::vector<std::string>& keys, int capturing, int& cleared,
            const char* prompt = "PRESS A KEY") {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox row = MeasureRow();
    float value_end = row.row_x + row.row_w - row.block_w + row.row_h + style.ItemSpacing.x + row.value_w;
    int n = int(keys.size());
    int pressed = -1;
    float box_w = n > 1 ? row.em * kKeyBoxEms : row.value_w;
    ImGui::PushID(label);
    RowLabel(row, label);
    for (int i = 0; i < n; i++) {
        float x = value_end - float(n - i) * box_w - float(n - 1 - i) * style.ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, row.row_y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##key", ImVec2(box_w, row.row_h))) pressed = i;
        bool deleted = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || deleted) cleared = i;
        bool pointed = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (pointed && !keys[i].empty() && keys[i] != kUnbound) g_hovered_box = i;
        bool lit = capturing == i || ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg));
        // key names as they are
        std::string value = keys[i];
        if (capturing == i) value = Ui(prompt);
        else if (value == kUnbound) value = Ui(kUnbound);
        float pad = (box_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x + std::max(pad, 0.0f), row.row_y));
        ImGui::AlignTextToFramePadding();
        if (capturing == i) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(value.c_str());
        if (capturing == i) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
    EndRow(row);
    return pressed;
}

// Escape cancels, any other bindable key or mouse button binds
void PollCapture() {
    if (g_capture < 0) return;
    int vk = input::PressedKey();
    if (!g_capture_armed) {
        g_capture_armed = !vk;
        return;
    }
    if (!vk) return;
    if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.load()), vk, g_capture_slot);
    g_capture = -1;
    g_capture_done = ImGui::GetFrameCount();
}

// any input of a controller binds once those held when it began are let go
void PollPadCapture() {
    if (g_pad_capture < 0) return;
    std::string input = input::PressedPadInput();
    bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                  std::chrono::steady_clock::now() - g_pad_capture_since > kPadCaptureTimeout;
    if (!cancel && !g_pad_capture_armed) {
        g_pad_capture_armed = input.empty();
        return;
    }
    if (!cancel && input.empty()) return;
    if (!cancel) input::PadBind(size_t(g_pad_capture.load()), input);
    g_pad_capture = -1;
    g_capture_done = ImGui::GetFrameCount();
}

// not under a bind or on the button that just bound
bool CaptureFree() { return g_capture < 0 && g_pad_capture < 0 && ImGui::GetFrameCount() > g_capture_done + 1; }

void ValueRow(const Row& r) {
    int step = OptionRow(r.label, Ui(RowText(r.id, 0)).c_str());
    if (step) RowText(r.id, step);
}

const char* LinkUrl(int id) {
    switch (id) {
        case kDonate: return kDonateUrl;
        case kGithub: return kGithubUrl;
        case kYoutube: return kYoutubeUrl;
        case kDiscord: return kDiscordUrl;
        default: return nullptr;
    }
}

// label left, OPEN right; a tap, click, Enter or A anywhere on the row opens the page in the browser, or the touch
// layout's editor
void LinkRow(const Row& r) {
    RowBox box = MeasureRow();
    ImGui::PushID(r.label);
    bool open = ImGui::Selectable("##link", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h));
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) FillItem();
    RowLabel(box, r.label);
    std::string value = r.id == kExportLog ? g_export_log_status : Ui("OPEN");
    float value_x = box.row_x + box.row_w - box.block_w + box.row_h + ImGui::GetStyle().ItemSpacing.x;
    float pad = (box.value_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    ImGui::PopID();
    EndRow(box);
    if (!open) return;
    if (r.id == kExportLog) ExportLog();
    else if (r.id == kTouchEdit) touch::EditLayout();
    else if (const char* url = LinkUrl(r.id)) SDL_OpenURL(url);
}

// the role left, the name right-aligned at the row's end
void InfoRow(const Row& r) {
    RowBox box = MeasureRow();
    RowLabel(box, r.label);
    if (r.id >= 0 && r.id < int(std::size(kCreditNames))) {
        const char* name = kCreditNames[r.id];
        float x = box.row_x + box.row_w - ImGui::CalcTextSize(name).x - ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, box.row_y));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
    }
    EndRow(box);
}

void KeyRow(const Row& r) {
    int cleared = -1;
    int box = BindRow(r.label, KeyBoxes(size_t(r.id)), g_capture == r.id ? g_capture_slot : -1, cleared);
    // a Delete that answered a capture is no clear
    if (cleared >= 0 && CaptureFree()) input::Bind(size_t(r.id), 0, cleared);
    if (box < 0 || !CaptureFree()) return;
    g_capture_slot = box;
    g_capture = r.id;
    g_capture_armed = false;
}

// the bound input, else the mapping's; a clear goes back to the mapping. A bind captured as an axis
// (a trigger) is tagged, so the player sees why it reads as an axis and not a button
void PadRow(const Row& r) {
    size_t count = 0;
    const input::Action& action = input::PadActions(&count)[r.id];
    std::string bound = input::PadBinding(size_t(r.id));
    if (bound.empty()) bound = action.defaults;
    else if (input::PadBindingIsAxis(size_t(r.id))) bound += " (axis)";
    int cleared = -1;
    int box = BindRow(r.label, {bound}, g_pad_capture == r.id ? 0 : -1, cleared,
                      "PRESS A BUTTON");
    if (cleared >= 0 && CaptureFree()) input::PadBind(size_t(r.id), "");
    if (box < 0 || !CaptureFree()) return;
    g_pad_capture = r.id;
    g_pad_capture_armed = false;
    g_pad_capture_since = std::chrono::steady_clock::now();
}

// the preset cards: three buttons side by side, each applying its preset on a tap
void PresetCards() {
    float em = ImGui::GetFontSize();
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float w = (ImGui::GetContentRegionAvail().x - gap * 2) / 3;
    struct Preset {
        const char* name;
        const char* desc;
        int id;
    };
    constexpr Preset kPresets[] = {{"LOW-END", "50% - Bloom off", 0},
                                   {"MID", "67% - Bloom on", 1},
                                   {"HIGH", "100% - All on", 2}};
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, gap);
        std::string label = Ui(kPresets[i].name) + "\n" + kPresets[i].desc;
        if (ImGui::Button(label.c_str(), ImVec2(w, em * 2.8f))) ApplyPreset(kPresets[i].id);
    }
}

void DrawDisplayPage() {
    CardBegin("PERFORMANCE PRESETS");
    PresetCards();
    CardEnd();
    // the graphics rows, grouped by their headings into cards; the preset links live in the cards above
    bool in_card = false;
    for (const Row& r : kGraphicsRows) {
        // YELLOW TINT is a dead setting in Carbon (no Carbon hook applies it), so hide the row there; Most
        // Wanted keeps it, its tint hook lives in src/game/game.cpp
        if (r.id == kYellowTint && std::wcscmp(kGame.short_title, L"NFS Carbon") == 0) continue;
        if (r.id == kLowEndPreset || r.id == kMidPreset || r.id == kHighPreset) continue;
        if (r.kind == Kind::Heading) {
            if (in_card) CardEnd();
            CardBegin(r.label);
            in_card = true;
        } else if (r.kind == Kind::Value) {
            if (r.id == kBrightness) {
                // a slider, not arrows: the frame's gamma, 50% to 200%
                int b = int(game::Opt().brightness);
                if (SliderValueRow(r.label, &b, 50, 200, "%u%%")) {
                    game::Opt().brightness = uint32_t(b);
                    Changed();
                }
            } else {
                ValueRow(r);
            }
        } else if (r.kind == Kind::Link) {
            LinkRow(r);
        }
    }
    if (in_card) CardEnd();
}

// the live input test: a gamepad drawn with ImGui primitives, its buttons and sticks lighting up as pressed
void DrawPadTest() {
    input::PadLive live;
    if (!input::PadLiveState(live)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(Ui("No gamepad connected - plug in a controller to test it").c_str());
        ImGui::PopStyleColor();
        return;
    }
    float em = ImGui::GetFontSize();
    float w = ImGui::GetContentRegionAvail().x;
    float h = em * 12.0f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 lit = IM_COL32(220, 175, 60, 255);
    const ImU32 lit_text = IM_COL32(20, 15, 8, 255);
    const ImU32 idle = IM_COL32(38, 33, 28, 255);
    const ImU32 edge = IM_COL32(130, 112, 82, 255);
    const ImU32 dim_text = IM_COL32(200, 185, 160, 255);
    auto pill = [&](ImVec2 a, ImVec2 b, bool on, const char* text) {
        dl->AddRectFilled(a, b, on ? lit : idle, (b.y - a.y) * 0.5f);
        dl->AddRect(a, b, on ? lit : edge, (b.y - a.y) * 0.5f);
        ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2), on ? lit_text : dim_text, text);
    };
    auto circle = [&](ImVec2 c, float r, bool on, const char* text) {
        dl->AddCircleFilled(c, r, on ? lit : idle, 32);
        dl->AddCircle(c, r, on ? lit : edge, 32, 1.5f);
        ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), on ? lit_text : dim_text, text);
    };
    auto bar = [&](float x0, float x1, float y, float bh, float v, const char* text) {
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bh), idle, bh * 0.25f);
        if (v > 0.01f)
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * std::clamp(v, 0.0f, 1.0f), y + bh), lit,
                              bh * 0.25f);
        dl->AddRect(ImVec2(x0, y), ImVec2(x1, y + bh), edge, bh * 0.25f);
        ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(x0 - ts.x - em * 0.4f, y + (bh - ts.y) / 2), dim_text, text);
    };
    // the triggers' bars at the top, labelled
    float bar_y = o.y + em * 0.6f;
    float bar_h = em * 0.9f;
    bar(o.x + em * 2.4f, o.x + w * 0.46f, bar_y, bar_h, live.lt, "LT");
    bar(o.x + w * 0.54f, o.x + w - em * 2.4f, bar_y, bar_h, live.rt, "RT");
    // the shoulders, start and back under the bars
    float pill_y = bar_y + bar_h + em * 0.6f;
    float pill_h = em * 1.5f;
    pill(ImVec2(o.x + em * 2.4f, pill_y), ImVec2(o.x + em * 2.4f + w * 0.12f, pill_y + pill_h), live.lb, "LB");
    pill(ImVec2(o.x + w - em * 2.4f - w * 0.12f, pill_y), ImVec2(o.x + w - em * 2.4f, pill_y + pill_h), live.rb, "RB");
    pill(ImVec2(o.x + w * 0.5f - em * 3.4f, pill_y), ImVec2(o.x + w * 0.5f - em * 0.4f, pill_y + pill_h), live.back,
         "BACK");
    pill(ImVec2(o.x + w * 0.5f + em * 0.4f, pill_y), ImVec2(o.x + w * 0.5f + em * 3.4f, pill_y + pill_h), live.start,
         "START");
    // the d-pad left, ABXY right; the sticks below and inward, clear of both
    float mid_y = pill_y + pill_h + em * 3.4f;
    float r = em * 1.05f;
    float s = r * 2 + em * 0.25f;
    ImVec2 dpad(o.x + w * 0.16f, mid_y);
    auto dpad_button = [&](float dx, float dy, bool on) {
        ImVec2 a(dpad.x + dx * s - r, dpad.y + dy * s - r);
        dl->AddRectFilled(a, ImVec2(a.x + r * 2, a.y + r * 2), on ? lit : idle, r * 0.3f);
        dl->AddRect(a, ImVec2(a.x + r * 2, a.y + r * 2), on ? lit : edge, r * 0.3f);
    };
    dpad_button(0, -1, live.dpad_up);
    dpad_button(0, 1, live.dpad_down);
    dpad_button(-1, 0, live.dpad_left);
    dpad_button(1, 0, live.dpad_right);
    ImVec2 abxy(o.x + w * 0.84f, mid_y);
    circle(ImVec2(abxy.x, abxy.y - s), r, live.north, "Y");
    circle(ImVec2(abxy.x, abxy.y + s), r, live.south, "A");
    circle(ImVec2(abxy.x - s, abxy.y), r, live.west, "X");
    circle(ImVec2(abxy.x + s, abxy.y), r, live.east, "B");
    // the sticks: a ring with a dot for the deflection, gold when the thumb button is pressed
    auto stick = [&](ImVec2 c, float x, float y, bool pressed) {
        float R = em * 1.7f;
        dl->AddCircleFilled(c, R, idle, 40);
        dl->AddCircle(c, R, pressed ? lit : edge, 40, 1.5f);
        float dx = std::clamp(x, -1.0f, 1.0f) * R * 0.55f;
        float dy = std::clamp(-y, -1.0f, 1.0f) * R * 0.55f;  // y up positive
        dl->AddCircleFilled(ImVec2(c.x + dx, c.y + dy), R * 0.38f, pressed ? lit : IM_COL32(90, 78, 62, 255), 24);
    };
    float stick_y = mid_y + em * 3.2f;
    float stick_lx = std::max(w * 0.34f, dpad.x + s + r + em * 1.7f + em * 0.6f);
    float stick_rx = std::min(w * 0.66f, abxy.x - s - r - em * 1.7f - em * 0.6f);
    stick(ImVec2(o.x + stick_lx, stick_y), live.lx, live.ly, live.ls);
    stick(ImVec2(o.x + stick_rx, stick_y), live.rx, live.ry, live.rs);
}

void DrawPadGroup(const char* title, std::initializer_list<int> actions) {
    CardBegin(title);
    size_t count = 0;
    const input::Action* acts = input::PadActions(&count);
    for (int idx : actions) {
        Row r{Kind::PadBind, idx, acts[idx].label};
        PadRow(r);
    }
    CardEnd();
}

void DrawControllerPage() {
    CardBegin("GAMEPAD");
    input::PadLive live;
    if (input::PadLiveState(live)) {
        ImGui::TextUnformatted(live.name.empty() ? "Controller" : live.name.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(Ui("CONNECTED").c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(Ui("No gamepad connected").c_str());
        ImGui::PopStyleColor();
    }
    CardEnd();
    CardBegin("LIVE INPUT TEST");
    DrawPadTest();
    CardEnd();
    // the rebinds, grouped by what they do in the game
    DrawPadGroup("DRIVING", {0, 1, 2, 3, 6, 7, 14, 15, 16, 17, 18, 19});
    DrawPadGroup("CAMERA", {4, 5, 20, 21, 22, 23});
    DrawPadGroup("MENU", {8, 9, 10, 11, 12, 13});
    CardBegin("STICKS");
    int dz = int(app::Get().deadzone);
    if (SliderValueRow("STICK DEADZONE", &dz, 0, 50, "%u%%")) {
        app::Get().deadzone = uint32_t(dz);
        app::Save();
    }
    if (ImGui::Button(Ui("RESET TO DEFAULTS").c_str())) input::ResetPadBindings();
    CardEnd();
}

void DrawTouchPage() {
    CardBegin("TOUCH");
    for (const Row& r : kTouchRows) {
        if (r.kind == Kind::Heading) continue;  // the card is the heading
        if (r.kind == Kind::Value) ValueRow(r);
        else if (r.kind == Kind::Link) LinkRow(r);
    }
    CardEnd();
}

void DrawRows(int page) {
    if (page == kSettingsPage) {
        DrawDisplayPage();
        return;
    }
    if (page == kControllerPage) {
        DrawControllerPage();
        return;
    }
    if (page == kTouchPage) {
        DrawTouchPage();
        return;
    }
    // the other pages: their rows inside cards, a heading starting a new card
    size_t count = 0;
    const Row* list = rows::Page(page, &count);
    bool in_card = false;
    for (size_t i = 0; i < count; i++) {
        const Row& r = list[i];
        if (r.kind == Kind::Heading) {
            if (in_card) CardEnd();
            CardBegin(r.label);
            in_card = true;
            continue;
        }
        if (!in_card) {
            CardBegin(kPages[page]);
            in_card = true;
        }
        if (r.kind == Kind::Value) ValueRow(r);
        else if (r.kind == Kind::Link) LinkRow(r);
        else if (r.kind == Kind::Info) InfoRow(r);
        else if (r.kind == Kind::PadBind) PadRow(r);
        else KeyRow(r);
    }
    if (in_card) CardEnd();
}

void Defaults() {
    g_capture = -1;
    g_pad_capture = -1;
    RestoreDefaults(g_page);
}

// the sidebar's pages, in order; the keyboard page stays, it is the only place to rebind keys in Carbon
constexpr int kSidebarPages[] = {kSettingsPage, kControllerPage, kTouchPage, kKeyboardPage, kGeneralPage,
                                 kCheatsPage, kCreditsPage};

void DrawNav() {
    float em = ImGui::GetFontSize();
    for (int p : kSidebarPages) {
        std::string name = Ui(kPages[p]);
        bool active = (g_page == p);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, kGold);
            ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
        }
        if (ImGui::Button(name.c_str(), ImVec2(-1, em * 2.2f))) g_page = p;
        if (active) ImGui::PopStyleColor(2);
    }
}

constexpr const char* kCredit = "Base by Rangga Casper, Ported by chizui";
constexpr const char* kFpsCredit = "Ported by chizui";

// the hint line, then the credit on the left and Back and Defaults on the right
void DrawFooter(float em) {
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    const char* hint = "";
    if (g_capture >= 0) hint = "Press a key or mouse button to bind, Escape cancels";
    else if (g_pad_capture >= 0) hint = "Press a button on the controller to bind, wait 5 seconds to cancel";
    ImGui::TextUnformatted(Ui(hint, false).c_str());
    ImGui::PopStyleColor();
    float bw = em * 8;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kDim, "%s", kCredit);
    ImGui::SameLine(0, 0);
    float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
    if (ImGui::Button(Ui("Back", false).c_str(), ImVec2(bw, 0))) ToggleMenu();
    ImGui::SameLine();
    if (ImGui::Button(Ui("Defaults", false).c_str(), ImVec2(bw, 0))) Defaults();
}

// The rows a finger drags up or down scroll with it, as a phone's lists do: the row it came down on is let go, so the
// drag presses nothing. A drag of the scrollbar itself stays the scrollbar's.
void DragScroll() {
    static bool dragging = false;
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging = false;
        return;
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!dragging) {
        bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;
        bool inside = window->Rect().Contains(io.MouseClickedPos[ImGuiMouseButton_Left]);
        bool scrollbar = ImGui::GetActiveID() == ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y);
        float moved = std::fabs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y);
        dragging = touch && inside && !scrollbar && moved > ImGui::GetFontSize() * 0.6f;
        if (!dragging) return;
    }
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    ImGui::ClearActiveID();
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // a sidebar needs room: wider than the old plain list
    float panel_w = std::min(vp->Size.x * 0.92f, em * 56.0f);
    float panel_h = std::min(vp->Size.y * 0.92f, em * 26.0f);
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, em * 0.35f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, em * 0.25f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.2f, em * 0.9f));
    // a scrollbar a thumb can take
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, em * 1.2f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, em * 2.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(Ui("SETTINGS").c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em * 0.3f));
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##body", ImVec2(0, -footer), ImGuiChildFlags_None);
        float nav_w = em * 11.0f;
        ImGui::BeginChild("##nav", ImVec2(nav_w, 0), ImGuiChildFlags_None);
        DrawNav();
        ImGui::EndChild();
        ImGui::SameLine(0, em * 0.8f);
        ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        g_hovered_box = -1;
        DrawRows(g_page);
        DragScroll();
        ImGui::EndChild();
        ImGui::EndChild();
        DrawFooter(em);
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

// the window's display's refresh rate, 0 when unreadable
uint32_t RefreshRate() {
    SDL_Window* w = static_cast<SDL_Window*>(Window());
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay());
    return m && m->refresh_rate > 1 ? uint32_t(std::lround(m->refresh_rate)) : 0;
}

// as app::Pace, at this cap
void PaceAt(uint32_t cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace

void PaceFrame() {
    if (Get().fps_cap != kFpsCapRefresh) {
        Pace();
        return;
    }
    // a few under, so a variable refresh display stays in its range and vsync never waits a whole refresh
    constexpr uint32_t kRefreshMargin = 3;
    uint32_t rate = RefreshRate();
    PaceAt(rate > kRefreshMargin ? rate - kRefreshMargin : 0);
}

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == kKeyboardPage) {
        *count = InputRows().size();
        return InputRows().data();
    }
    if (page == kControllerPage) {
        *count = PadRows().size();
        return PadRows().data();
    }
    if (page == kTouchPage) {
        *count = std::size(kTouchRows);
        return kTouchRows;
    }
    if (page == kGeneralPage) {
        *count = std::size(kGeneralRows);
        return kGeneralRows;
    }
    if (page == kCreditsPage) {
        *count = std::size(kCreditRows);
        return kCreditRows;
    }
    if (page == kCheatsPage) {
        *count = std::size(kCheatRows);
        return kCheatRows;
    }
    *count = std::size(kGraphicsRows);
    return kGraphicsRows;
}

int rows::PageCount() { return int(std::size(kPages)); }

const char* rows::PageName(int page) { return page >= 0 && page < rows::PageCount() ? kPages[page] : ""; }

std::string rows::Label(const Row& r) { return Ui(r.label); }

std::string rows::Value(const Row& r, int step) {
    switch (r.kind) {
        case Kind::Heading:
            return "";
        case Kind::Value:
            return Ui(RowText(r.id, step));
        case Kind::Link:
            if (r.id == kExportLog) return g_export_log_status;
            return Ui(r.id == kLowEndPreset || r.id == kMidPreset || r.id == kHighPreset ? "APPLY" : "OPEN");
        case Kind::Info:
            return r.id >= 0 && r.id < int(std::size(kCreditNames)) ? kCreditNames[r.id] : "";
        case Kind::Bind: {
            if (g_capture == r.id) return Ui("PRESS A KEY");
            std::vector<std::string> keys = KeyBoxes(size_t(r.id));
            std::string text = keys[0] == kUnbound ? Ui(kUnbound) : keys[0];
            if (!keys[1].empty()) text += " / " + keys[1];
            return text;
        }
        case Kind::PadBind: {
            if (g_pad_capture == r.id) return Ui("PRESS A BUTTON");
            size_t count = 0;
            std::string bound = input::PadBinding(size_t(r.id));
            return bound.empty() ? input::PadActions(&count)[r.id].defaults : bound;
        }
    }
    return "";
}

void rows::Activate(const Row& r) {
    if (r.kind == Kind::Link) {
        if (r.id == kExportLog) {
            ExportLog();
        } else if (r.id == kLowEndPreset) {
            ApplyPreset(0);
        } else if (r.id == kMidPreset) {
            ApplyPreset(1);
        } else if (r.id == kHighPreset) {
            ApplyPreset(2);
        } else if (r.id == kTouchEdit) touch::EditLayout();
        else if (const char* url = LinkUrl(r.id)) SDL_OpenURL(url);
    } else if (r.kind == Kind::Bind && g_capture < 0 && g_pad_capture < 0) {
        g_capture_slot = 0;
        g_capture_armed = false;
        g_capture = r.id;
    } else if (r.kind == Kind::PadBind && g_capture < 0 && g_pad_capture < 0) {
        g_pad_capture_armed = false;
        g_pad_capture_since = std::chrono::steady_clock::now();
        g_pad_capture = r.id;
    }
}

// as PollCapture and PollPadCapture without the panel: the key or button that started the wait comes up first, Escape
// or kPadCaptureTimeout gives up
bool rows::PollBinding() {
    if (g_capture >= 0) {
        int vk = input::PressedKey();
        if (!g_capture_armed) {
            g_capture_armed = !vk;
            return false;
        }
        if (!vk) return false;
        if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.load()), vk, g_capture_slot);
        g_capture = -1;
        return true;
    }
    if (g_pad_capture >= 0) {
        std::string input = input::PressedPadInput();
        bool cancel = std::chrono::steady_clock::now() - g_pad_capture_since > kPadCaptureTimeout;
        if (!cancel && !g_pad_capture_armed) {
            g_pad_capture_armed = input.empty();
            return false;
        }
        if (!cancel && input.empty()) return false;
        if (!cancel) input::PadBind(size_t(g_pad_capture.load()), input);
        g_pad_capture = -1;
        return true;
    }
    return false;
}

void rows::Defaults(int page) {
    g_capture = -1;
    g_pad_capture = -1;
    RestoreDefaults(page);
}

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

bool Capturing() { return g_capture >= 0 || g_pad_capture >= 0; }

void ToggleMenu() {
    bool open = !MenuOpen();
    g_open.store(open, std::memory_order_relaxed);
    g_capture = -1;
    g_pad_capture = -1;
    if (open) input::ReleaseCursor();
}

// the frame rate's colour: green at 55 and up, yellow from 30, red below
ImU32 FpsColor(double fps) {
    if (fps >= 55) return IM_COL32(60, 220, 110, 255);
    if (fps >= 30) return IM_COL32(255, 200, 0, 255);
    return IM_COL32(255, 70, 60, 255);
}

// a temperature's colour: white while cool, orange from 70, red from 80; -1 is the device not telling
ImU32 TempColor(float temp) {
    if (temp >= 80) return IM_COL32(255, 70, 60, 255);
    if (temp >= 70) return IM_COL32(255, 150, 40, 255);
    return IM_COL32(235, 235, 235, 255);
}

// the latency's colour: green at 16.7 ms and under, yellow to 33.3, red above
ImU32 LatencyColor(float ms) {
    if (ms <= 16.7f) return IM_COL32(60, 220, 110, 255);
    if (ms <= 33.3f) return IM_COL32(255, 200, 0, 255);
    return IM_COL32(255, 70, 60, 255);
}

// one number in the overlay: its label, its value, the value's colour, and the load bar's fill (-1, none)
struct OverlayMetric {
    const char* label;
    char value[16];
    ImU32 color;
    float load;
};

// the last frame times in milliseconds, for the GOverlay style's graph; DrawUi runs once a present
constexpr size_t kFrameTimeCount = 120;
float g_frame_times[kFrameTimeCount] = {};
size_t g_frame_time_i = 0, g_frame_time_n = 0;
std::chrono::steady_clock::time_point g_frame_time_last{};

// the graph, 0 to 33.3 ms, with the 60 fps line at 16.7 ms
void DrawFrameTimeGraph(ImDrawList* dl, ImVec2 at, float w, float h) {
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(0, 0, 0, 90));
    float y167 = at.y + h * (1.0f - 16.7f / 33.3f);
    dl->AddLine(ImVec2(at.x, y167), ImVec2(at.x + w, y167), IM_COL32(255, 80, 60, 180));
    size_t n = g_frame_time_n;
    if (n < 2) return;
    size_t base = (g_frame_time_i + kFrameTimeCount - n) % kFrameTimeCount;
    for (size_t j = 1; j < n; j++) {
        float t0 = std::min(g_frame_times[(base + j - 1) % kFrameTimeCount], 33.3f);
        float t1 = std::min(g_frame_times[(base + j) % kFrameTimeCount], 33.3f);
        float xa = at.x + w * float(j - 1) / float(n - 1);
        float xb = at.x + w * float(j) / float(n - 1);
        float ya = at.y + h * (1.0f - t0 / 33.3f);
        float yb = at.y + h * (1.0f - t1 / 33.3f);
        dl->AddLine(ImVec2(xa, ya), ImVec2(xb, yb), IM_COL32(255, 214, 90, 255));
    }
}

// the frame rate, and under it or beside it the latency, the CPU's and GPU's load and temperatures and the
// battery's, over the game at the place Options::fps_position picks; the layout comes from
// Options::overlay_layout, the look is always the GOverlay panel
void DrawFpsCounter(double fps) {
    const game::Options& o = game::Opt();
    if (!o.fps_counter || MenuOpen()) return;
    host::SamplePerf();
    // the frame time for the graph, from the gap between presents
    auto now = std::chrono::steady_clock::now();
    if (g_frame_time_last != std::chrono::steady_clock::time_point{}) {
        float ms = std::chrono::duration<float, std::milli>(now - g_frame_time_last).count();
        g_frame_times[g_frame_time_i] = std::min(std::max(ms, 0.0f), 100.0f);
        g_frame_time_i = (g_frame_time_i + 1) % kFrameTimeCount;
        if (g_frame_time_n < kFrameTimeCount) g_frame_time_n++;
    }
    g_frame_time_last = now;
    // the latency: the frame times' average, smoothed over the ring so the number holds still
    float latency_ms = 0;
    if (g_frame_time_n > 0) {
        float sum = 0;
        for (size_t i = 0; i < g_frame_time_n; i++) sum += g_frame_times[i];
        latency_ms = sum / float(g_frame_time_n);
    }

    // the numbers to show, each with its own colour; the load bar's fill rides along
    OverlayMetric metrics[5];
    int count = 0;
    std::snprintf(metrics[count].value, sizeof(metrics[count].value), "%.0f", fps);
    metrics[count].label = "FPS";
    metrics[count].color = FpsColor(fps);
    metrics[count].load = -1;
    count++;
    std::snprintf(metrics[count].value, sizeof(metrics[count].value), "%.1f ms", latency_ms);
    metrics[count].label = "LATENCY";
    metrics[count].color = LatencyColor(latency_ms);
    metrics[count].load = -1;
    count++;
    auto add_perf = [&](const char* label, float load, float temp) {
        if (load < 0 && temp < 0) return;
        OverlayMetric& m = metrics[count];
        int n = 0;
        if (load >= 0) n += std::snprintf(m.value + n, sizeof(m.value) - n, "%.0f%%", load);
        if (temp >= 0) std::snprintf(m.value + n, sizeof(m.value) - n, " %.0f\xC2\xB0" "C", temp);
        m.label = label;
        m.color = TempColor(temp);
        m.load = load;
        count++;
    };
    add_perf("CPU", o.fps_usage ? host::CpuLoad() : -1, o.fps_temps ? host::CpuTemp() : -1);
    add_perf("GPU", o.fps_usage ? host::GpuLoad() : -1, o.fps_temps ? host::GpuTemp() : -1);
    if (o.fps_temps && host::BatteryTemp() >= 0) {
        OverlayMetric& m = metrics[count];
        std::snprintf(m.value, sizeof(m.value), "%.0f\xC2\xB0" "C", host::BatteryTemp());
        m.label = "BAT";
        m.color = TempColor(host::BatteryTemp());
        m.load = -1;
        count++;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    float big = ImGui::GetFontSize() * 1.5f, small = ImGui::GetFontSize();
    float gap = small * 0.5f;      // between a label and its value
    float seg_gap = small * 1.5f;  // between segments on one line
    bool horizontal = o.overlay_layout == kOverlayHorizontal;
    float pad = small * 0.5f;   // the panel's padding
    float graph_h = small * 2.0f;
    float graph_gap = small * 0.5f;
    float bar_w = small * 4.0f;  // the load bar beside a value
    float bar_h = small * 0.45f;

    // the label and value widths at the size each line uses; stacked, the values sit in one column
    float label_w[5] = {}, value_w[5] = {};
    float label_col = 0;
    for (int i = 0; i < count; i++) {
        float size = !horizontal && i == 0 ? big : small;
        label_w[i] = font->CalcTextSizeA(size, FLT_MAX, 0, metrics[i].label).x;
        value_w[i] = font->CalcTextSizeA(size, FLT_MAX, 0, metrics[i].value).x;
        if (!horizontal) label_col = std::max(label_col, label_w[i]);
    }
    float credit_w = font->CalcTextSizeA(small, FLT_MAX, 0, kFpsCredit).x;

    float block_w = 0, block_h = 0;
    if (horizontal) {
        for (int i = 0; i < count; i++) {
            block_w += label_w[i] + gap + value_w[i];
            if (metrics[i].load >= 0) block_w += gap + bar_w;
            if (i == 0) block_w += gap + credit_w;  // the credit beside the frame rate
            if (i + 1 < count) block_w += seg_gap;
        }
        block_h = small;
    } else {
        for (int i = 0; i < count; i++) {
            float w = label_col + gap + value_w[i];
            if (metrics[i].load >= 0) w += gap + bar_w;
            if (i == 0) w += gap + credit_w;
            block_w = std::max(block_w, w);
        }
        block_h = big + (count - 1) * small;
    }
    block_h += graph_gap + graph_h;

    float margin = screen.y * 0.03f;
    uint32_t pos = o.fps_position;
    bool bottom = pos == kFpsBottomLeft || pos == kFpsBottom || pos == kFpsBottomRight;
    // top centre sits under the settings gear, bottom centre over Back and Start, which the touch controls keep there
    float y = pos == kFpsTop ? screen.y * 0.14f
              : pos == kFpsBottom ? screen.y * 0.86f - block_h
              : bottom ? screen.y - margin - block_h
                       : margin;
    // left, centred or right aligned, each line on its own; every corner at the edge, as far in as it is from the
    // top or bottom
    float side = margin;
    auto x = [&](float w) {
        if (pos == kFpsTopLeft || pos == kFpsBottomLeft) return side;
        if (pos == kFpsTopRight || pos == kFpsBottomRight) return screen.x - side - w;
        return screen.x / 2 - w / 2;
    };
    float block_x = x(block_w);
    dl->AddRectFilled(ImVec2(block_x - pad, y - pad), ImVec2(block_x + block_w + pad, y + block_h + pad),
                      IM_COL32(8, 8, 10, 175), small * 0.4f);

    ImU32 label_color = IM_COL32(200, 200, 200, 255);
    // the panel gives the contrast, so the overlay draws plain text
    auto text = [&](float size, ImVec2 at, ImU32 color, const char* s) { dl->AddText(font, size, at, color, s); };
    auto bar = [&](float bx, float by, float load) {
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bar_w, by + bar_h), IM_COL32(255, 255, 255, 40));
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bar_w * load / 100.0f, by + bar_h),
                          IM_COL32(255, 214, 90, 255));
    };

    if (horizontal) {
        float cx = block_x;
        for (int i = 0; i < count; i++) {
            text(small, ImVec2(cx, y), label_color, metrics[i].label);
            cx += label_w[i] + gap;
            text(small, ImVec2(cx, y), metrics[i].color, metrics[i].value);
            cx += value_w[i];
            if (metrics[i].load >= 0) {
                bar(cx + gap, y + (small - bar_h) / 2, metrics[i].load);
                cx += gap + bar_w;
            }
            if (i == 0) {
                // the credit beside the frame rate, dimmer
                text(small, ImVec2(cx + gap, y), IM_COL32(200, 180, 150, 220), kFpsCredit);
                cx += gap + credit_w;
            }
            if (i + 1 < count) cx += seg_gap;
        }
    } else {
        float value_x = block_x + label_col + gap;
        for (int i = 0; i < count; i++) {
            float size = i == 0 ? big : small;
            float ly = y + (i == 0 ? 0 : big + (i - 1) * small);
            text(size, ImVec2(block_x, ly), label_color, metrics[i].label);
            text(size, ImVec2(value_x, ly), metrics[i].color, metrics[i].value);
            float after = value_x + value_w[i];
            if (metrics[i].load >= 0) {
                bar(after + gap, ly + (size - bar_h) / 2, metrics[i].load);
                after += gap + bar_w;
            }
            if (i == 0) text(small, ImVec2(after + gap, ly + size - small), IM_COL32(200, 180, 150, 220), kFpsCredit);
        }
    }
    DrawFrameTimeGraph(dl, ImVec2(block_x, y + block_h - graph_h), block_w, graph_h);
}

void DrawUi(double fps) {
    DrawFpsCounter(fps);
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second, ImGui's twenty is too fast for settings
    // scale the font with the output height: a phone's dense screen reads 13 px text at 540 rows to the height, the
    // game's font (native/ui.cpp, taller) brought to that size
    float base = io.Fonts->Fonts.empty() ? 13.0f : io.Fonts->Fonts[0]->FontSize;
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 540.0f) * 13.0f / base;
    if (MenuOpen()) {
        PollCapture();
        PollPadCapture();
        DrawPanel();
    }
}

}  // namespace app
