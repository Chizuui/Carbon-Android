// The Android host: SDL's window and event loop, the settings and folders, the app's lifecycle; the game is described
// by app::kGame (src/common/game_info.cpp)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/system_properties.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "common/aspect.h"
#include "common/translations.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

SDL_Window* g_window = nullptr;
std::atomic<bool> g_foreground{true};
std::atomic<uint32_t> g_surface_epoch{0};
std::mutex g_foreground_lock;
std::condition_variable g_foreground_cv;
char g_crash_log_path[512] = {};

void CrashAppendStr(char* dst, size_t& pos, size_t cap, const char* s) {
    if (!s) return;
    while (*s && pos + 1 < cap) dst[pos++] = *s++;
}

void CrashAppendHex(char* dst, size_t& pos, size_t cap, uintptr_t v) {
    CrashAppendStr(dst, pos, cap, "0x");
    char tmp[2 * sizeof(uintptr_t)];
    int n = 0;
    do {
        unsigned d = unsigned(v & 0xFu);
        tmp[n++] = char(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    } while (v && n < int(sizeof(tmp)));
    while (n-- > 0 && pos + 1 < cap) dst[pos++] = tmp[n];
}

void CrashAppendInt(char* dst, size_t& pos, size_t cap, int v) {
    if (v < 0) {
        CrashAppendStr(dst, pos, cap, "-");
        v = -v;
    }
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = char('0' + (v % 10));
        v /= 10;
    } while (v && n < int(sizeof(tmp)));
    while (n-- > 0 && pos + 1 < cap) dst[pos++] = tmp[n];
}

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        default: return "SIG";
    }
}

void WriteAll(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) break;
        buf += n;
        len -= size_t(n);
    }
}

void CrashWriteModule(int fd, char* buf, size_t cap, const char* tag, uintptr_t addr) {
    Dl_info dl{};
    if (!addr || !dladdr(reinterpret_cast<const void*>(addr), &dl)) return;
    size_t pos = 0;
    CrashAppendStr(buf, pos, cap, "error: crash ");
    CrashAppendStr(buf, pos, cap, tag);
    CrashAppendStr(buf, pos, cap, " module=");
    CrashAppendStr(buf, pos, cap, dl.dli_fname ? dl.dli_fname : "?");
    if (dl.dli_fbase) {
        CrashAppendStr(buf, pos, cap, "+");
        CrashAppendHex(buf, pos, cap, addr - uintptr_t(dl.dli_fbase));
    }
    if (dl.dli_sname) {
        CrashAppendStr(buf, pos, cap, " sym=");
        CrashAppendStr(buf, pos, cap, dl.dli_sname);
        if (dl.dli_saddr) {
            CrashAppendStr(buf, pos, cap, "+");
            CrashAppendHex(buf, pos, cap, addr - uintptr_t(dl.dli_saddr));
        }
    }
    CrashAppendStr(buf, pos, cap, "\n");
    if (fd >= 0) WriteAll(fd, buf, pos);
    WriteAll(STDERR_FILENO, buf, pos);
}

constexpr int kCrashSigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
struct sigaction g_prev_sa[5]{};

void CrashHandler(int sig, siginfo_t* info, void* ctx) {
    uintptr_t pc = 0, lr = 0, sp = 0;
#if defined(__aarch64__)
    if (ctx) {
        const auto* uc = static_cast<const ucontext_t*>(ctx);
        pc = uintptr_t(uc->uc_mcontext.pc);
        lr = uintptr_t(uc->uc_mcontext.regs[30]);
        sp = uintptr_t(uc->uc_mcontext.sp);
    }
#elif defined(__x86_64__)
    if (ctx) {
        const auto* uc = static_cast<const ucontext_t*>(ctx);
        pc = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
        sp = uintptr_t(uc->uc_mcontext.gregs[REG_RSP]);
    }
#endif
    char buf[512];
    size_t pos = 0;
    CrashAppendStr(buf, pos, sizeof(buf), "error: crash ");
    CrashAppendStr(buf, pos, sizeof(buf), SignalName(sig));
    CrashAppendStr(buf, pos, sizeof(buf), " sig=");
    CrashAppendInt(buf, pos, sizeof(buf), sig);
    if (info) {
        CrashAppendStr(buf, pos, sizeof(buf), " code=");
        CrashAppendInt(buf, pos, sizeof(buf), info->si_code);
        CrashAppendStr(buf, pos, sizeof(buf), " addr=");
        CrashAppendHex(buf, pos, sizeof(buf), uintptr_t(info->si_addr));
    }
    CrashAppendStr(buf, pos, sizeof(buf), " pc=");
    CrashAppendHex(buf, pos, sizeof(buf), pc);
    CrashAppendStr(buf, pos, sizeof(buf), " lr=");
    CrashAppendHex(buf, pos, sizeof(buf), lr);
    CrashAppendStr(buf, pos, sizeof(buf), " sp=");
    CrashAppendHex(buf, pos, sizeof(buf), sp);
    CrashAppendStr(buf, pos, sizeof(buf), "\n");

    int fd = g_crash_log_path[0] ? open(g_crash_log_path, O_WRONLY | O_APPEND) : -1;
    if (fd >= 0) WriteAll(fd, buf, pos);
    WriteAll(STDERR_FILENO, buf, pos);

    CrashWriteModule(fd, buf, sizeof(buf), "pc", pc);
    if (lr && lr != pc) CrashWriteModule(fd, buf, sizeof(buf), "lr", lr);
    if (fd >= 0) close(fd);
    for (size_t i = 0; i < sizeof(kCrashSigs) / sizeof(kCrashSigs[0]); i++) {
        if (kCrashSigs[i] == sig) {
            sigaction(sig, &g_prev_sa[i], nullptr);
            break;
        }
    }
    raise(sig);
}

void InstallCrashHandlers(const std::string& path) {
    std::snprintf(g_crash_log_path, sizeof(g_crash_log_path), "%s", path.c_str());
    struct sigaction sa{};
    sa.sa_sigaction = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (size_t i = 0; i < sizeof(kCrashSigs) / sizeof(kCrashSigs[0]); i++) {
        sigaction(kCrashSigs[i], &sa, &g_prev_sa[i]);
    }
}

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

// the log's lines to logcat as well: stderr is read on a thread of its own
void LogToLogcat() {
    int fds[2];
    if (pipe(fds)) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0]] {
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) {
            line.append(buf, size_t(n));
            for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1))
                __android_log_write(ANDROID_LOG_INFO, "BLACK", line.substr(0, nl).c_str());
        }
    }).detach();
}

std::string Property(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return v;
}

void SetForeground(bool on) {
    {
        std::lock_guard lock(g_foreground_lock);
        if (on && !g_foreground) g_surface_epoch++;
        g_foreground = on;
    }
    g_foreground_cv.notify_all();
    LOGI("app: %s", on ? "in front" : "in the background");
}

// SDL's display mode in landscape: the window is not up yet when the settings load
app::Mode ScreenMode() {
    if (g_window) {
        int w = 0, h = 0;
        host::WindowPixels(w, h);
        if (w > 0 && h > 0) return {uint32_t(w), uint32_t(h)};
    }
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m) return {1920, 1080};
    uint32_t w = uint32_t(m->w * m->pixel_density), h = uint32_t(m->h * m->pixel_density);
    return {std::max(w, h), std::min(w, h)};
}

float RefreshRate() {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d);
    return m && m->refresh_rate > 1 ? m->refresh_rate : 60.0f;
}

}  // namespace

namespace host {

SDL_Window* Window() { return g_window; }
bool Foreground() { return g_foreground; }
uint32_t SurfaceEpoch() { return g_surface_epoch; }

void WaitForeground() {
    std::unique_lock lock(g_foreground_lock);
    g_foreground_cv.wait(lock, [] { return g_foreground.load(); });
}

void WindowPixels(int& w, int& h) {
    w = h = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &w, &h);
}

}  // namespace host

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
// the defaults a settings file was written under: 3 has the frame rate capped at 60 and vsync off, which a file from
// before takes once, its player's own choices kept from then on
constexpr int kDefaults = 3;

std::filesystem::path File() { return ExeDir() / kGame.settings; }

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"deadzone", int64_t(s.deadzone)},
                  {"join_ip", s.join_ip},
                  {"defaults", int64_t(kDefaults)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

// a phone renders the console's frame at about 720p unless the settings say otherwise
constexpr uint32_t kFirstRunRenderPercent = 67;

// the console's 60: what a title steps once a frame (menus, their key repeat, films) runs at the console's speed
void PhoneFrameDefaults(Settings& s) {
    s.fps_cap = 60;
    s.vsync = false;
}

}  // namespace

// the settings as a phone starts with them, for a first run and the options' restore
Settings DefaultSettings() {
    Settings s;
    s.render_percent = kFirstRunRenderPercent;
    PhoneFrameDefaults(s);
    return s;
}

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.deadzone = t["deadzone"].value_or(s.deadzone);
    if (s.deadzone > 50) s.deadzone = 50;
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    if (t["defaults"].value_or(int64_t(0)) < kDefaults) PhoneFrameDefaults(s);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s", kGame.title, IMAGE_XBE, date, BUILD_COMMIT);
    LOGR("Path: %s", ExeDir().c_str());
    LOGR("Device: %s %s, Android %s (API %d)", Property("ro.product.manufacturer").c_str(),
         Property("ro.product.model").c_str(), Property("ro.build.version.release").c_str(), SDL_GetAndroidSDKVersion());
    LOGR("CPU: %s (%d threads)", Property("ro.soc.model").c_str(), SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Display: %ux%u, %.0f Hz", desk.width, desk.height, RefreshRate());
    LogSettings();
}

Mode Desktop() { return ScreenMode(); }

Mode Output() {
    // the window is the screen: the resolution setting is the screen's own
    return Desktop();
}

bool Widescreen() {
    // the frame's shape as the aspect ratio setting has it: the game reads this at boot
    return game::Aspect() >= 1.5f;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    if (game::Opt().anti_aliasing == game::kAaSsaa) h *= 2.0f;  // the presenter averages it back down
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

// app-specific external storage: the player can reach the logs, saves and settings over USB
std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        const char* p = nullptr;
        if (SDL_GetAndroidExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) p = SDL_GetAndroidExternalStoragePath();
        if (!p) p = SDL_GetAndroidInternalStoragePath();
        return std::filesystem::path(p ? p : ".");
    }();
    return dir;
}

int Instance() { return 0; }

void* Window() { return g_window; }

// no splash and no relaunch into another executable on Android
void SplashFirstFrame() {}
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
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

}  // namespace app

namespace {

void InitUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_IsTouchScreen;
    io.BackendPlatformName = "android_sdl3";
    touch::LoadArt();
}

// The logs go in the MostWanted folder beside the game, where the player finds them without Android/data (which
// Android 11 on hides from file managers); without all files access yet (the first run asks for it), in the app's own
// folder as before
std::filesystem::path LogDir() {
    std::filesystem::path shared = std::filesystem::path(host::kShared) / "logs";
    std::error_code ec;
    std::filesystem::create_directories(shared, ec);
    std::filesystem::path probe = shared / ".write";
    if (std::FILE* f = std::fopen(probe.c_str(), "w")) {
        std::fclose(f);
        std::filesystem::remove(probe, ec);
        return shared;
    }
    return app::ExeDir() / "logs";
}

void Dispatch(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            logx::Flush();
            kernel::Shutdown(0);
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            SetForeground(false);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            SetForeground(true);
            break;
        case SDL_EVENT_LOW_MEMORY:
            LOGW("app: Android is low on memory");
            break;
        case SDL_EVENT_SENSOR_UPDATE:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            gyro::OnEvent(e);
            return;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnPadEvent(e);
            break;
        default:
            break;
    }
    if (touch::OnEvent(e)) return;
    std::lock_guard lock(app::UiLock());
    ui::OnEvent(e);
}

}  // namespace

int main(int, char**) {
    LogToLogcat();
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    // no poll sentinel: on Android each pump inside SDL_WaitEventTimeout pushes a new one, which wakes the wait at once,
    // and the event loop spun a whole core until the timeout (SDL 3.4)
    SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO | SDL_INIT_SENSOR)) {
        __android_log_print(ANDROID_LOG_ERROR, "BLACK", "SDL_Init: %s", SDL_GetError());
        return 1;
    }
    // a file named perf in the app's folder: the perf log lines, as XBE_PERF gives them on a PC
    if (std::error_code ec; std::filesystem::exists(app::ExeDir() / "perf", ec)) setenv("XBE_PERF", "1", 1);
    logx::Init(LogDir().string(), app::kGame.log);
    InstallCrashHandlers(logx::Path());
    LOGR("app: log %s", logx::Path().c_str());
    if (!logx::PrevPath().empty()) LOGR("app: previous log %s", logx::PrevPath().c_str());
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    std::string title = Utf8(app::kGame.title);
    g_window = SDL_CreateWindow(title.c_str(), 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        LOGE("app: no window: %s", SDL_GetError());
        return 1;
    }

    app::GameFiles game;
    if (!app::FindGame(game)) {
        logx::Flush();
        return 1;
    }

    InitUi();
    input::Init();
    if (app::kGame.window) app::kGame.window(g_window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Utf8(app::kGame.short_title).c_str(),
                                 app::Ui("The game failed to start, see the log in the logs folder.", false).c_str(),
                                 g_window);
        return 1;
    }

    for (;;) {
        SDL_Event e;
        // a few milliseconds: a HIDAPI controller is read in the pump, which events from Java do not wake
        if (SDL_WaitEventTimeout(&e, 4)) {
            Dispatch(e);
            while (SDL_PollEvent(&e)) Dispatch(e);
        }
        touch::Pump();
    }
}
