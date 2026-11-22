// Need for Speed: Carbon boots on the host's XDK like Most Wanted does, but every hook in src/game is cut for Most Wanted's
// addresses. Until Carbon's are found these are the calls the common code makes, doing nothing
#include <android/log.h>
#include <cctype>
#include <cstring>
#include <string>

#include "common/aspect.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
namespace dispatch {
uint32_t HostThunk(X86Func* fn);
}

#include "common/fe_mouse.h"
#include "common/game_font.h"
#include "common/hud_layout.h"
#include "common/options.h"

namespace game {

// Carbon's widescreen:
// 0x45D294: byte flag set when widescreen is enabled at boot (mov al, [0x45D294]; and al, 1; ret at 0x22C130)
// 0x3F98E0: float width transform (853.0f), read by front-end and HUD transform (0x10F094, 0x111749)
constexpr uint32_t kVideoFlags = 0x45D294, kWideWidth = 0x3F98E0;

// Check that an entire guest byte range [va, va + size) is non-null and backed by committed pages.
bool IsCommittedRange(uint32_t va, uint32_t size) {
    if (va < 0x10000 || !size) return false;
    uint64_t end = uint64_t(va) + size - 1;
    if (end > 0xFFFFFFFFull) return false;
    for (uint64_t p = va & ~0xFFFull; p <= (end & ~0xFFFull); p += 0x1000) {
        if (!mem::IsCommitted(uint32_t(p))) return false;
    }
    return true;
}

void WidenAspect() {
    if (!mem::IsCommitted(kVideoFlags) || !(kernel::G<uint8_t>(kVideoFlags) & 1)) return;
    if (!IsCommittedRange(kWideWidth, 4)) return;
    float w = 480.0f * Aspect();
    uint32_t bits;
    std::memcpy(&bits, &w, 4);
    kernel::W32(kWideWidth, bits);
}

// Carbon render switches:
// - 0x3F96B3: Particles - Main View switch (checked at 0x1125E3, 0x112A63)
// - 0x3F96BA: Car Reflections (EnvMap) switch (checked at 0x1121BB)
// - 0x3F972C: Road Reflection master switch (dword, checked at 0x111FCC before eView[4..5] loop)
// - 0x4084FC: Car underbody shadow switch (dword, checked at 0x29A6BC before sub_0027F640)
// - 0x46D7A0: NOT written anymore. It was assumed to be the skip-movies flag, but deeper RE could not prove it:
//   its only readers are frontend-flow functions (sub_0014C210 gates a career-load shortcut on it, sub_00130220
//   takes a shortcut path, sub_0023E210's test on it is dead) plus the options sync (sub_001EDA40), and none of
//   them connects to the movie player; the FEng package hash it switches to (0xC3960EB9) could not be resolved
//   to a name. Writing 0/1 to a game-owned option dword every present was the highest remaining crash risk,
//   so the write was removed; the SKIP MOVIES toggle stays in the menu but does nothing until the flag is proven.
// The world bloom switch (0x401D14, read at 0x112AF9) is left to the game: with it written 0 the scene broke
// (map off its track, crashes). BLOOM off is native/vulkan.cpp's skip of the glow draw
constexpr uint32_t kParticleSwitch = 0x3F96B3;
constexpr uint32_t kEnvMapSwitch = 0x3F96BA;
constexpr uint32_t kRoadReflectionSwitch = 0x3F972C;
constexpr uint32_t kRoadReflectionView = 0x492F80;  // eView[4], RenderTarget pointer at +0x50 (0x492FD0)
constexpr uint32_t kEnvMapFace0Target = 0x493750;   // EnvMap face 0 RenderTarget pointer (checked at 0x1121E4)
constexpr uint32_t kCarShadowSwitch = 0x4084FC;

// The colour data of a RenderTarget's D3DSurface ([target + 8] -> [surface + 4] physical addr).
uint32_t TargetData(uint32_t target) {
    uint32_t surface = IsCommittedRange(target + 8, 4) ? kernel::R32(target + 8) : 0;
    return IsCommittedRange(surface + 4, 4) ? mem::VirtualFromPhysical(kernel::R32(surface + 4)) : 0;
}

void ApplyGraphicsSwitches() {
    const Options& o = Opt();
    if (mem::IsCommitted(kParticleSwitch)) {
        kernel::G<uint8_t>(kParticleSwitch) = o.particles ? 1 : 0;
    }
    if (mem::IsCommitted(kEnvMapSwitch)) {
        static uint32_t frame = 0;
        uint32_t rate = o.reflections;
        uint32_t every = rate < 3 ? 1u << rate : 512;
        kernel::G<uint8_t>(kEnvMapSwitch) = (frame++ % every == 0) ? 1 : 0;
    }
    if (IsCommittedRange(kRoadReflectionSwitch, 4)) {
        kernel::W32(kRoadReflectionSwitch, o.road_reflections ? 1u : 0u);
    }
    if (IsCommittedRange(kCarShadowSwitch, 4)) {
        kernel::W32(kCarShadowSwitch, o.shadows ? 1u : 0u);
    }
    // 0x46D7A0 is deliberately NOT written here anymore (see the comment above): its identity as the
    // skip-movies flag is unproven, so the per-present write to that game-owned dword was removed.
    // Expose road reflection and envmap targets so Vulkan binds black when toggled OFF instead of stale frames.
    g_road_reflection = IsCommittedRange(kRoadReflectionView + 0x50, 4)
                            ? TargetData(kernel::R32(kRoadReflectionView + 0x50))
                            : 0;
    g_env_cube = IsCommittedRange(kEnvMapFace0Target, 4) ? TargetData(kernel::R32(kEnvMapFace0Target)) : 0;
}

// Carbon cheats:
// 0x46D944 holds AICopManager / ICopMgr singleton pointer
constexpr uint32_t kCopManager = 0x46D944;
constexpr uint32_t kNoNewCops = 0x5C;
constexpr uint32_t kUnlockAll = 0x46D784;
constexpr uint32_t kCareerRoot = 0x46A7CC, kCashField = 0x24310;

// Carbon's speedbreaker meter (update 0x23F150, amount [this + 0x7C], 0..1) skips its drain when this debug
// byte, which the game never writes, is set. Not the cheat table's check (0x863A0, id 0xB) it also asks:
// that is the game's own cheat codes
constexpr uint32_t kInfiniteSpeedbreaker = 0x497D5C;

void ApplyCheats() {
    const Options& o = Opt();
    if (mem::IsCommitted(kInfiniteSpeedbreaker))
        kernel::G<uint8_t>(kInfiniteSpeedbreaker) = o.cheat_speedbreaker ? 1 : 0;
    // the career's cash, [[[0x46A7CC] + 0xD4]] + 0x24310 (what the cash setter 0x9D3F0 adds to; the shop and the
    // HUD read it): held at 9,999,999 while on, and kept (spendable) when turned off
    uint32_t sys = IsCommittedRange(kCareerRoot, 4) ? kernel::R32(kCareerRoot) : 0;
    // [sys + 0xD4] is the career pointer array and [sys + 0xD8] is its element count (sub_0011B550).
    uint32_t count = IsCommittedRange(sys + 0xD8, 4) ? kernel::R32(sys + 0xD8) : 0;
    uint32_t list = (count > 0 && IsCommittedRange(sys + 0xD4, 4)) ? kernel::R32(sys + 0xD4) : 0;
    uint32_t career = IsCommittedRange(list, 4) ? kernel::R32(list) : 0;
    // No career loaded yet (career 0) must not write: career + 0x24310 is then 0x24310, which lies
    // inside the XBE image, so the write would land on dead code bytes instead of the cash field.
    if (o.cheat_cash && career >= 0x10000u && IsCommittedRange(career + kCashField, 4)) {
        kernel::W32(career + kCashField, 9999999);
    }
    // the byte 28 unlock tests or in (0x2C6A60 answers true at once, the thing lists' lock tests 0x8A500, ...);
    // the game sets it itself only for one cheat entry (0x8FBB9) and from its boot (0x1EDA24), so it is held
    // while on and given back as it was when turned off
    static bool unlock_held = false;
    static uint8_t unlock_saved = 0;
    if (mem::IsCommitted(kUnlockAll)) {
        if (o.cheat_unlock_all) {
            if (!unlock_held) unlock_saved = kernel::G<uint8_t>(kUnlockAll);
            unlock_held = true;
            kernel::G<uint8_t>(kUnlockAll) = 1;
        } else if (unlock_held) {
            kernel::G<uint8_t>(kUnlockAll) = unlock_saved;
            unlock_held = false;
        }
    }
    // the cop manager's "no new cops or pursuits" byte, the one its vtable's +0x34 sets and +0x38 clears and
    // its spawn check (0x1B810) reads. Written only while on and once more when turned off, so the game's own
    // use of it is left alone
    static bool held = false;
    uint32_t mgr = IsCommittedRange(kCopManager, 4) ? kernel::R32(kCopManager) : 0;
    if (mgr >= 0x10000 && mem::IsCommitted(mgr + kNoNewCops)) {
        if (o.cheat_no_police) kernel::G<uint8_t>(mgr + kNoNewCops) = 1;
        else if (held) kernel::G<uint8_t>(mgr + kNoNewCops) = 0;
        held = o.cheat_no_police;
    }
}

void CaptureFont() {}

bool TakeFont(FontImage&) { return false; }

// In Carbon: 0x46D280 is GameFlowState.
// State 6 is racing/driving. State 3 is the career/frontend state: the game itself only resolves career cars
// when the state is 3 (sub_0027D970: cmp [0x46D280],3; jnz skip). Any other state is menu/loading/FMV.
constexpr uint32_t kGameFlowState = 0x46D280;
constexpr uint32_t kGameFlowRacing = 6;
constexpr uint32_t kGameFlowCareer = 3;
// cFEng's instance: FEngine at its first field, the packages' list at +0xE4 (their next +8, name +0xC)
constexpr uint32_t kCFEng = 0x46A7C8;

// Non-menu FEng overlays that stay open during driving (HUD, subtitles, notifications, flashers).
bool StartsWithNoCase(const char* s, const char* prefix) {
    if (!s) return false;
    for (; *prefix; s++, prefix++) {
        if (std::tolower(static_cast<unsigned char>(*s)) != std::tolower(static_cast<unsigned char>(*prefix)))
            return false;
    }
    return true;
}

bool EqualsNoCase(const char* a, const char* b) {
    if (!a) return false;
    for (; *a || *b; a++, b++) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return true;
}

bool IsOverlayPackageName(const char* name) {
    if (!name || *name == '\0') return false;
    static constexpr const char* kPrefixes[] = {
        "HUD_", "FESubtitler", "Chyron_IG", "FeChyron", "FLASHERS",
        "Autosave_Overlay", "BUSTED_OVERLAY", "FeBlackScreen", "ScreenPrintf", "Loading",
    };
    for (const char* prefix : kPrefixes) {
        if (StartsWithNoCase(name, prefix)) return true;
    }
    // EA_TRAX.fng / EA_Trax.fng are song banners, while EA_Trax_Jukebox.fng is a menu.
    return EqualsNoCase(name, "EA_TRAX.fng") || EqualsNoCase(name, "EA_TRAX");
}

bool Racing() {
    return mem::IsCommitted(kGameFlowState) && kernel::R32(kGameFlowState) == kGameFlowRacing;
}
// Custom strings for button prompts
struct Prompt {
    uint32_t hash;
    const char* text;
};
const Prompt kPrompts[] = {
    // Carbon-specific title screen hashes (extracted from ZZDATA Global strings chunk):
    {0x8CD6BD6D, "Tap anywhere to begin"}, // Press START
    {0x8CD63CB8, "Tap anywhere to begin"}, // Press START button
    {0x0BE9C967, "Tap anywhere to begin"}, // Press START to begin
    {0x1BAD0AEA, "Tap anywhere to begin"}, // Press START
    // Fallback Most Wanted hashes:
    {0x9B580A55, "Tap anywhere to begin"},
    {0xDC64C04C, "Tap anywhere to begin"},
    {0xD18D4C4C, "Tap anywhere to begin"},
    {0x9D8F2513, "Tap or press"},
};

uint32_t GuestText(const char* text) {
    static std::mutex lock;
    static std::unordered_map<const char*, uint32_t> copies;
    std::lock_guard guard(lock);
    uint32_t& va = copies[text];
    if (!va) {
        size_t n = std::strlen(text) + 1;
        va = mem::AllocSystem(uint32_t(n));
        if (va) std::memcpy(kernel::HostPtr(va), text, n);
    }
    return va;
}

const char* OwnString(uint32_t hash) {
    for (const Prompt& p : kPrompts)
        if (p.hash == hash) return p.text;
    return nullptr;
}

// FEng Touch & Mouse Hook for Carbon:
// FEngine ([kCFEng]): +0x01 bMouseActive, +0xE0 first FEPackage (+0x04 next), +0x108 FEGameInterface (vtable 0x3B0000,
// slot +0x44 is GetMouseInfo, slot +0x48 calls 0x138E70 TestForIntersection). Never hook 0x46A8E4: that is
// cFrontendDatabase (vtable 0x3C4130), whose +0x44 (0x1233E0) is called with 0 as arg 1 when entering Career/CarSelect.
constexpr uint32_t kMouseActive = 0x01, kFirstPackage = 0xE0, kInterface = 0x108;
constexpr uint32_t kGetMouseInfoSlot = 0x44;
constexpr uint32_t kPkgNext = 0x04, kPkgName = 0x0C, kControlMask = 0x20, kInputEnabled = 0x28;
constexpr uint32_t kCurrentButton = 0x8C;
constexpr int kMaxPackages = 64;

std::atomic<bool> g_menu{true};
std::atomic<uint32_t> g_package{0}, g_current_button{0};
std::atomic<int32_t> g_x{-2000}, g_y{-2000};
std::atomic<bool> g_down{false};
bool g_reported_down = false;
std::atomic<uint32_t> g_presses{0};
constexpr int kMouseFrames = 30;
std::atomic<int> g_mouse_frames{0};
std::atomic<uint32_t> g_accept_package{0}, g_accept_button{0}, g_start_package{0};
std::atomic<bool> g_tap_anywhere{false}, g_press_start{false}, g_accept_start{false};
constexpr uint32_t kAnywhere = 1;

uint32_t FEngine() {
    if (!mem::IsCommitted(kCFEng)) return 0;
    uint32_t feng = kernel::R32(kCFEng);
    return feng && mem::IsCommitted(feng) ? kernel::R32(feng) : 0;
}

const char* PackageName(uint32_t p) {
    uint32_t name = p && mem::IsCommitted(p + kPkgName + 3) ? kernel::R32(p + kPkgName) : 0;
    return name && mem::IsCommitted(name) ? (const char*)kernel::HostPtr(name) : nullptr;
}

void GetMouseInfo(X86Context& ctx, uint8_t*) {
    constexpr int kHoldUpdates = 2, kStayUpdates = 4;
    constexpr int16_t kAway = -2000;
    static uint32_t seen = 0;
    static int hold = 0, stay = 0;
    uint32_t info = kernel::Args{ctx}[0];
    uint32_t presses = g_presses;
    if (presses != seen) {
        seen = presses;
        hold = kHoldUpdates;
    }
    bool down = g_down || hold > 0;
    g_reported_down = down;
    if (hold > 0) hold--;
    if (down) stay = kStayUpdates;
    else if (stay > 0) stay--;
    bool here = down || stay > 0;
    if (info && mem::IsCommitted(info + 7)) {
        kernel::W16(info, uint16_t(here ? int16_t(g_x.load()) : kAway));
        kernel::W16(info + 2, uint16_t(here ? int16_t(g_y.load()) : kAway));
        kernel::W16(info + 4, 0);
        kernel::W16(info + 6, down ? 1 : 0);  // Left button
    }
    kernel::Return(ctx, 0, 1);
}

void FeFrame() {
    uint32_t fe = FEngine();
    if (!fe) {
        g_package = 0;
        g_current_button = 0;
        g_press_start = false;
        g_tap_anywhere = false;
        g_menu = true;
        return;
    }
    static const uint32_t thunk = dispatch::HostThunk(GetMouseInfo);
    if (mem::IsCommitted(fe + kInterface + 3)) {
        uint32_t iface = kernel::R32(fe + kInterface);
        if (iface && mem::IsCommitted(iface + 3)) {
            uint32_t vtbl = kernel::R32(iface);
            uint32_t slot = vtbl + kGetMouseInfoSlot;
            if (vtbl && mem::IsCommitted(slot + 3) && kernel::R32(slot) != thunk) kernel::W32(slot, thunk);
        }
    }
    bool racing = Racing();
    uint32_t pkg = 0;
    bool press_start = false;
    int n = 0;
    for (uint32_t p = mem::IsCommitted(fe + kFirstPackage + 3) ? kernel::R32(fe + kFirstPackage) : 0;
         p && n < kMaxPackages && mem::IsCommitted(p + kCurrentButton + 3);
         p = kernel::R32(p + kPkgNext), n++) {
        if (!kernel::G<uint8_t>(p + kInputEnabled) || !(kernel::R32(p + kControlMask) & 1)) continue;
        const char* name = PackageName(p);
        if (racing && IsOverlayPackageName(name)) continue;
        if (!pkg) pkg = p;
        if (name && StartsWithNoCase(name, "FePressStart")) {
            pkg = p;
            press_start = true;
        }
    }
    bool menu = !racing || pkg != 0;
    int frames = g_mouse_frames;
    if (g_down) frames = kMouseFrames;
    else if (frames > 0) frames--;
    g_mouse_frames = frames;
    if (mem::IsCommitted(fe + kMouseActive))
        kernel::G<uint8_t>(fe + kMouseActive) = (menu && pkg != 0 && frames > 0) ? 1 : 0;
    static uint32_t last_race_menu = 0;
    if (racing && pkg != last_race_menu) {
        last_race_menu = pkg;
        if (pkg) LOGR("fepkg: race menu %s", PackageName(pkg) ? PackageName(pkg) : "?");
        else LOGR("fepkg: race resume");
    } else if (!racing) {
        last_race_menu = 0;
    }
    g_package = menu ? pkg : 0;
    g_current_button = (menu && pkg && mem::IsCommitted(pkg + kCurrentButton + 3)) ? kernel::R32(pkg + kCurrentButton) : 0;
    g_press_start = menu && press_start;
    g_tap_anywhere = menu && pkg != 0 && !press_start;
    g_menu = menu;
}

// FeMenuShown: true whenever we are not in a race OR when paused, so the pad buttons hide
// and give way to menu navigation (pills in the corner, swipe, tap anywhere)
bool FeMenuShown() {
    return g_menu.load();
}

bool FeMouseDown() { return g_reported_down; }
bool FeFinger(float& x, float& y) {
    x = float(g_x.load());
    y = float(g_y.load());
    return g_down;
}

// When in a menu, every pad button is a pill on the left of the screen: the screens' own prompts (Y sell, X modify,
// L and R the tabs) are not read from Carbon's FEng yet, so all are offered and the game takes the ones it uses
FePrompts CurrentFePrompts() {
    FePrompts p{};
    if (FeMenuShown()) {
        for (int i = 0; i < kFePromptCount; i++) p.shown[i] = true;
        p.label[kFePromptLT] = "L";  // no digits in a label: an action, not the map's "ZOOM 1.5X"
        p.label[kFePromptRT] = "R";
        p.right_stick = true;        // Enable right stick in menus (car showcase rotation, map, etc.)
    }
    return p;
}

void FeTouch(bool down, float u, float v) {
    float wide = 480.0f * Aspect();
    g_x = int32_t((u - 0.5f) * wide);
    g_y = int32_t((v - 0.5f) * 480.0f);
    if (down && !g_down) {
        g_presses++;
        uint32_t p = g_package.load();
        if (g_press_start) {
            g_start_package = p;
            g_accept_start = true;
            g_accept_package = 0;
            g_accept_button = 0;
        } else if (g_tap_anywhere && p) {
            uint32_t btn = g_current_button.load();
            g_accept_package = p;
            g_accept_button = btn ? btn : kAnywhere;
        }
    }
    g_down = down;
    g_mouse_frames = kMouseFrames;
}

bool FeTakeAccept() {
    uint32_t p = g_accept_package.exchange(0), button = g_accept_button.exchange(0);
    if (!button || p != g_package || !g_menu || !mem::IsCommitted(p + kInputEnabled)) return false;
    if (!kernel::G<uint8_t>(p + kInputEnabled)) return false;
    uint32_t cur = g_current_button.load();
    return button == kAnywhere ? (cur == 0) : (cur == button);
}

bool FeTakeStart() {
    uint32_t p = g_start_package.exchange(0);
    bool start = g_accept_start.exchange(false);
    if (!start || !p || p != g_package || !g_press_start) return false;
    return mem::IsCommitted(p + kInputEnabled) && kernel::G<uint8_t>(p + kInputEnabled) != 0;
}

bool LanguageAvailable(const std::string& code) { return code == "int" || code == "ind"; }

}  // namespace game

namespace app {

// the console's XC_LANGUAGE the game reads at boot: English
uint32_t ConsoleLanguage() { return 1; }

}  // namespace app

// SearchForString overrides in Carbon:
static void CleanUnlocalized(X86Context& ctx) {
    if (!ctx.eax || !mem::IsCommitted(ctx.eax)) return;
    const char* str = (const char*)kernel::HostPtr(ctx.eax);
    if (!str) return;
    if (std::strcmp(str, "UNLOCALIZED") == 0 || std::strcmp(str, "Unlocalized") == 0 ||
        std::strcmp(str, "*UNLOCALIZED*") == 0 || std::strcmp(str, "*Unlocalized*") == 0) {
        ctx.eax = game::GuestText("");
    }
}

// 1. sub_0011F9A0 (low-level binary search, hash in edx)
X86_EXTERN_FUNC(__imp__sub_0011F9A0);
X86_FUNC(sub_0011F9A0) {
    uint32_t hash = ctx.edx;
    // the title screen's string is edited in place: a string of ours handed back crashed the title screen
    if (hash == 0x186AAECC) {
        __imp__sub_0011F9A0(ctx, base);
        if (ctx.eax && mem::IsCommitted(ctx.eax)) {
            char* s = (char*)kernel::HostPtr(ctx.eax);
            static const char kText[] = "TAP to begin"; // only letters the original has: the title font's glyph set is cut down
            if (std::strlen(s) >= sizeof(kText) - 1) std::memcpy(s, kText, sizeof(kText));
        }
        return;
    }
    if (const char* text = game::OwnString(hash)) {
        if (uint32_t va = game::GuestText(text)) {
            ctx.eax = va;
            ctx.esp += 4;
            return;
        }
    }
    __imp__sub_0011F9A0(ctx, base);
    CleanUnlocalized(ctx);
}

// 2. sub_0011FAB0 (high-level SearchForString, hash on stack [esp+4])
X86_EXTERN_FUNC(__imp__sub_0011FAB0);
X86_FUNC(sub_0011FAB0) {
    uint32_t hash = kernel::R32(ctx.esp + 4);
    if (const char* text = game::OwnString(hash)) {
        if (uint32_t va = game::GuestText(text)) {
            ctx.eax = va;
            ctx.esp += 4;
            return;
        }
    }
    __imp__sub_0011FAB0(ctx, base);
    CleanUnlocalized(ctx);
}

// Screen Raindrops in Carbon: sub_00108690 ("Rain Drop" pass, cdecl, 1 arg, plain ret).
X86_EXTERN_FUNC(__imp__sub_00108690);
X86_FUNC(sub_00108690) {
    if (game::Opt().rain_drops) return __imp__sub_00108690(ctx, base);
    ctx.esp += 4;
}

// Light Flares in Carbon: sub_0010EC90 ("eRenderLightFlares" pass, cdecl/0-arg, plain ret).
// Off unless light_flares is on: on Vulkan the pass draws giant green/white neon circles and connecting trails.
// The setting is the experiment for the missing headlights: the pass draws the headlight glow, and the d3d light
// log shows whether the title sets spot lights for the road's pool at all.
X86_EXTERN_FUNC(__imp__sub_0010EC90);
X86_FUNC(sub_0010EC90) {
    if (game::Opt().light_flares) {
        game::g_flare_pass = true;
        __imp__sub_0010EC90(ctx, base);
        game::g_flare_pass = false;
        return;
    }
    ctx.esp += 4;
}
// Falling Rain in Carbon: sub_00278460 (thiscall, 2 args, ret 8)
X86_EXTERN_FUNC(__imp__sub_00278460);
X86_FUNC(sub_00278460) {
    if (game::Opt().rain) return __imp__sub_00278460(ctx, base);
    ctx.esp += 12; // 4 bytes return address + 8 bytes arguments
}

// Motion Blur in Carbon: sub_0010C5F0 (cdecl, 4 args, pops 4 bytes return address)
// Returns bool in al whether motion blur was drawn.
X86_EXTERN_FUNC(__imp__sub_0010C5F0);
X86_FUNC(sub_0010C5F0) {
    if (game::Opt().motion_blur) return __imp__sub_0010C5F0(ctx, base);
    ctx.eax &= ~0xFFu; // return false
    ctx.esp += 4;
}

// FEng SwitchPackage hook in Carbon: sub_0012FCD0
X86_EXTERN_FUNC(__imp__sub_0012FCD0);
X86_FUNC(sub_0012FCD0) {
    __imp__sub_0012FCD0(ctx, base);
}

// Unpause in Carbon: sub_0012DFE0
X86_EXTERN_FUNC(__imp__sub_0012DFE0);
X86_FUNC(sub_0012DFE0) {
    __imp__sub_0012DFE0(ctx, base);
}

// Speedbreaker update in Carbon: sub_0023F150 (thiscall, ret 8). Cheat on keeps the meter full across the
// update, which also ends the break on 0
X86_EXTERN_FUNC(__imp__sub_0023F150);
X86_FUNC(sub_0023F150) {
    const uint32_t self = ctx.ecx;
    const bool full = game::Opt().cheat_speedbreaker && mem::IsCommitted(self + 0x80);
    if (full) kernel::W32(self + 0x7C, 0x3F800000u);
    __imp__sub_0023F150(ctx, base);
    if (full) kernel::W32(self + 0x7C, 0x3F800000u);
}

// Nitrous update in Carbon: sub_00207BD0 (thiscall, ret 0xC). The tank is [this + 0xFC], 0..1, drained
// there by the frame's time and floored at 0; cheat on keeps it full across the update
X86_EXTERN_FUNC(__imp__sub_00207BD0);
X86_FUNC(sub_00207BD0) {
    const uint32_t self = ctx.ecx;
    const bool full = game::Opt().cheat_nos && mem::IsCommitted(self + 0x100);
    if (full) kernel::W32(self + 0xFC, 0x3F800000u);
    __imp__sub_00207BD0(ctx, base);
    if (full) kernel::W32(self + 0xFC, 0x3F800000u);
}

// A view's projection (0x1040E0, thiscall, the matrices in ecx; the view first of 3 arguments, its type at +8, ret
// 0x10): with the widescreen flag the camera's vertical is 9/16 of its horizontal angle, so a frame wider than 16:9 was
// the 16:9 picture stretched across it. The x scale ([matrices + 0x40]) is made the frame's (Hor+, the vertical kept)
// just before the view's matrix times it (0x2EFCB4, out = +0x80, a = the matrices, b = +0x40) that the frustum's planes
// come from. Only the views the game gives the widescreen angle (types 1, 2, 4 to 7: the players', their reflections
// and glows), as Most Wanted's
static uint32_t g_widen_matrices = 0;  // the matrices while a wide view's projection is made, game thread

X86_EXTERN_FUNC(__imp__sub_001040E0);
X86_FUNC(sub_001040E0) {
    const uint32_t view = kernel::R32(ctx.esp + 4);
    const uint32_t type = view && mem::IsCommitted(view + 8) ? kernel::R32(view + 8) : 0;
    const bool wide = mem::IsCommitted(0x45D294) && (kernel::G<uint8_t>(0x45D294) & 1);
    g_widen_matrices = wide && (type == 1 || type == 2 || (type >= 4 && type <= 7)) ? ctx.ecx : 0;
    __imp__sub_001040E0(ctx, base);
    g_widen_matrices = 0;
}

X86_EXTERN_FUNC(__imp__sub_002EFCB4);
X86_FUNC(sub_002EFCB4) {
    const uint32_t m = g_widen_matrices;
    if (m && kernel::R32(ctx.esp + 8) == m && kernel::R32(ctx.esp + 12) == m + 0x40) {
        g_widen_matrices = 0;
        kernel::G<float>(m + 0x40) *= (16.0f / 9.0f) / game::Aspect();
    }
    __imp__sub_002EFCB4(ctx, base);
}

// Guard Career/Frontend car spawn (0x242FD0): if 0x1B58 holds 0/-1 or a handle missing from Career[0]+0x234,
// fall back to 0x1B60 or the first valid car in Career[0]+0x234 before 0x2432F5 dereferences [esi+0x08].
X86_EXTERN_FUNC(sub_0007DD00);
X86_EXTERN_FUNC(sub_0007DD50);
X86_EXTERN_FUNC(__imp__sub_00242FD0);

static uint32_t QueryFeVar(const X86Context& base_ctx, uint8_t* base, uint32_t id, uint32_t idx, uint32_t def_val) {
    X86Context sc = base_ctx;
    uint32_t out_va = sc.esp - 16u;
    if (!game::IsCommittedRange(out_va - 0x400u, 0x410u)) return def_val;
    kernel::W32(out_va, def_val);
    sc.esp = out_va;
    sc.ecx = 0x456830u;
    auto push32 = [&](uint32_t v) {
        sc.esp -= 4u;
        kernel::W32(sc.esp, v);
    };
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(idx);
    push32(out_va);
    push32(id);
    push32(0u);
    sub_0007DD00(sc, base);
    return kernel::R32(out_va);
}

static void SetFeVar(const X86Context& base_ctx, uint8_t* base, uint32_t id, uint32_t val, uint32_t idx) {
    X86Context sc = base_ctx;
    sc.esp -= 16u;
    if (!game::IsCommittedRange(sc.esp - 0x400u, 0x400u)) return;
    sc.ecx = 0x456830u;
    auto push32 = [&](uint32_t v) {
        sc.esp -= 4u;
        kernel::W32(sc.esp, v);
    };
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(idx);
    push32(val);
    push32(id);
    push32(0u);
    sub_0007DD50(sc, base);
}

static bool IsKnownCarType(uint32_t car_type) {
    if (!car_type || !game::IsCommittedRange(0x46D920u, 4)) return false;
    uint32_t n = kernel::R32(0x46D920u);
    for (int i = 0; i < 512 && game::IsCommittedRange(n, 12); i++) {
        if (kernel::R32(n) == car_type) return true;
        n = kernel::R32(n + 8u);
    }
    return false;
}

static uint32_t FindCarSlotInDb(uint32_t car_db, uint32_t handle) {
    if (!game::IsCommittedRange(car_db, 0xC8u * 0x14u)) return 0;
    for (uint32_t i = 0; i < 0xC8u; i++) {
        uint32_t slot = car_db + i * 0x14u;
        if (kernel::R32(slot) == handle) return slot;
    }
    return 0;
}

// Bit 0x80 at slot+0x0C is ONE bit of a multi-bit state field, not a proven "owned" flag: sub_000A1D90,
// once labelled "IsCarOwned", is actually a deleter — it calls the db lookup (0x9CE70) with mask 0x80, which
// on match sets [slot]=0xFFFFFFFF — and other callers use masks 0x02/0x04. No pure ownership predicate exists
// in the XBE. The bit is kept only as a preference hint (pass 0 below wants it set with a known type), never
// as proof of ownership on its own.
constexpr uint32_t kCarOwnedFlag = 0x80u;

static uint32_t PickFallbackCarHandle(uint32_t car_db, uint32_t h1b60) {
    if (!game::IsCommittedRange(car_db, 0xC8u * 0x14u)) return 0;
    if (h1b60 != 0 && h1b60 != 0xFFFFFFFFu) {
        uint32_t s = FindCarSlotInDb(car_db, h1b60);
        if (s && IsKnownCarType(kernel::R32(s + 8u)) && (kernel::R32(s + 0x0Cu) & kCarOwnedFlag) != 0) return h1b60;
    }
    // Prefer a slot with the 0x80 bit set and a known type, then any known type. There used to be a third pass
    // ("owned-only, type unknown"), but the 0x80 bit is not proven to mean "owned" (see above), so a slot is
    // never picked on that bit alone. Never a placeholder handle.
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < 0xC8u; i++) {
            uint32_t slot = car_db + i * 0x14u;
            uint32_t h = kernel::R32(slot);
            if (h == 0 || h == 0xFFFFFFFFu) continue;
            bool owned = (kernel::R32(slot + 0x0Cu) & kCarOwnedFlag) != 0;
            bool known = IsKnownCarType(kernel::R32(slot + 8u));
            if (pass == 0 && owned && known) return h;
            if (pass == 1 && known) return h;
        }
    }
    return 0;
}

X86_FUNC(sub_00242FD0) {
    uint32_t sys = game::IsCommittedRange(game::kCareerRoot, 4) ? kernel::R32(game::kCareerRoot) : 0;
    uint32_t count = game::IsCommittedRange(sys + 0xD8u, 4) ? kernel::R32(sys + 0xD8u) : 0;
    uint32_t list = (count > 0 && game::IsCommittedRange(sys + 0xD4u, 4)) ? kernel::R32(sys + 0xD4u) : 0;
    uint32_t career = game::IsCommittedRange(list, 4) ? kernel::R32(list) : 0;
    // The game itself only resolves career cars when GameFlowState==3 (sub_0027D970: cmp [0x46D280],3).
    // This guard used to run its repair on every 242FD0 call regardless of state, which is what produced the
    // "car_db empty" log spam: it ran while no career was loaded yet and found nothing to repair. Gate the
    // repair the same way the game does; when gated out, log once per state value and do not touch the FeVars.
    uint32_t flow = game::IsCommittedRange(game::kGameFlowState, 4) ? kernel::R32(game::kGameFlowState) : 0xFFFFFFFFu;
    if (flow != game::kGameFlowCareer) {
        static uint32_t last_gated_flow = 0xFFFFFFFFu;
        if (flow != last_gated_flow) {
            LOGR("career: 242FD0 guard gated out (flow=%u), skipping repair", flow);
            last_gated_flow = flow;
        }
    } else if (career >= 0x10000u && game::IsCommittedRange(career + 0x234u, 0xC8u * 0x14u)) {
        uint32_t car_db = career + 0x234u;
        uint32_t wing = QueryFeVar(ctx, base, 0x1771u, 0xFFFFFFFFu, 0u);
        uint32_t h1b60 = QueryFeVar(ctx, base, 0x1B60u, 0xFFFFFFFFu, 0xFFFFFFFFu);
        uint32_t max_idx = (game::IsCommittedRange(0x497D44u, 4) && kernel::R32(0x497D44u) == 1u) ? 2u : 1u;
        for (uint32_t idx = 0; idx < max_idx; idx++) {
            uint32_t h = QueryFeVar(ctx, base, 0x1B58u, idx, 0xFFFFFFFFu);
            uint32_t slot = FindCarSlotInDb(car_db, h);
            // Leave the current car alone when the game already counts it as owned
            // (owned flag set), even if its type is not in the known-type list.
            if (slot && ((kernel::R32(slot + 0x0Cu) & kCarOwnedFlag) != 0 || IsKnownCarType(kernel::R32(slot + 8u)))) continue;
            uint32_t fb = PickFallbackCarHandle(car_db, h1b60);
            LOGR("career: repair 1b58[%u]=%08X (wing=%08X 1b60=%08X) -> %08X", idx, h, wing, h1b60, fb);
            if (fb) {
                SetFeVar(ctx, base, 0x1B58u, fb, idx);
                uint32_t after = QueryFeVar(ctx, base, 0x1B58u, idx, 0xFFFFFFFFu);
                if (after != fb) LOGR("career: SetFeVar 1b58[%u] -> %08X (expected %08X)", idx, after, fb);
            } else {
                LOGR("career: car_db empty [0]=%08X/%08X/%08X [1]=%08X/%08X/%08X", kernel::R32(car_db),
                     kernel::R32(car_db + 8u), kernel::R32(car_db + 0x0Cu), kernel::R32(car_db + 0x14u),
                     kernel::R32(car_db + 0x1Cu), kernel::R32(car_db + 0x14u + 0x0Cu));
                // Diagnostic: the car handle must live somewhere in the career struct — if it is not
                // in car_db (career+0x234) the offset may be wrong for Carbon. Scan for it once.
                if (game::IsCommittedRange(career, 0x3000u)) {
                    int found = 0;
                    for (uint32_t off = 0; off < 0x3000u && found < 4; off += 4u) {
                        if (kernel::R32(career + off) == h) {
                            LOGR("career: handle %08X found at career+0x%X", h, off);
                            found++;
                        }
                    }
                    if (!found) LOGR("career: handle %08X not found in career+0..0x3000", h);
                }
            }
        }
    }
    __imp__sub_00242FD0(ctx, base);
}

// Guard the car-stat read (0x2881C0) against the garbage [esi+0x08] pointer left behind when the
// career's car_db is empty (see the 0x242FD0 guard above): without a valid car the game builds a
// car object whose +0x08 slot is 0x100000000, and [edx+0x1C] faults (SIGSEGV, addr 0x10000001c).
// Returning 0 is safe for the observed crash: the caller (0x288240) stores eax as the copy-source pointer
// and copies [inner+0x1C] bytes from it — with inner 0 the count is 0, so nothing is copied. But the
// caller re-dereferences [obj+0x08] itself (see the sub_00288240 hook below), so this guard alone does
// not cover a nonzero uncommitted [obj+0x08].
X86_EXTERN_FUNC(__imp__sub_002881C0);
X86_FUNC(sub_002881C0) {
    uint32_t obj = ctx.ecx;
    if (!game::IsCommittedRange(obj, 0x20u)) {
        LOGR("career: sub_002881C0 bad obj %08X", obj);
        ctx.eax = 0;
        ctx.esp += 12;  // ret 8: return address + 2 args
        return;
    }
    uint32_t inner = kernel::R32(obj + 0x08u);
    if (!game::IsCommittedRange(inner, 0x20u)) {
        LOGR("career: sub_002881C0 bad [obj+8] %08X (obj %08X)", inner, obj);
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    __imp__sub_002881C0(ctx, base);
}

// Harden the caller of the car-stat guard (0x288240): after sub_002881C0 returns, the caller re-dereferences
// the same object itself — 0x288277: mov esi,[esi+0x08]; 0x28827E: mov ecx,[esi+0x1C] — and again at 0x2882B3
// ([obj+0x08] -> +0x20) and 0x2882C5 ([obj+0x0C] indexed). The guard's return value is only the copy-source
// pointer, so it cannot protect the caller: a nonzero uncommitted [obj+0x08] would still fault there after
// the guard returned 0. Validate the object up front instead. Nulls pass through (the original null-checks
// them); only nonzero uncommitted pointers are rejected. Returning 0 is safe: the only caller (0x2883A5)
// ignores the return value — it stores its own args and returns.
X86_EXTERN_FUNC(__imp__sub_00288240);
X86_FUNC(sub_00288240) {
    uint32_t self = ctx.ecx;
    uint32_t obj = 0;
    bool bad = !game::IsCommittedRange(self, 0x20u);
    if (!bad) {
        obj = kernel::R32(self + 0x0Cu);
        if (obj && !game::IsCommittedRange(obj, 0x20u)) bad = true;
    }
    if (!bad && obj) {
        uint32_t inner = kernel::R32(obj + 0x08u);
        if (inner && !game::IsCommittedRange(inner, 0x24u)) bad = true;
        uint32_t p0c = kernel::R32(obj + 0x0Cu);
        if (!bad && p0c && !game::IsCommittedRange(p0c, 4u)) bad = true;
    }
    if (bad) {
        LOGR("career: sub_00288240 bad car object (this %08X obj %08X)", self, obj);
        ctx.eax = 0;
        ctx.esp += 16;  // ret 0x0C: return address + 3 args
        return;
    }
    __imp__sub_00288240(ctx, base);
}

// Guard the car-stat float read (0x27C6A0) — same bad-car object as 0x2881C0: when the career's
// car_db is empty the table pointer [[obj+0x0C]+0x10] is null, and [table+idx*4] (or fld [eax])
// faults on guest 0 (SIGSEGV, addr 0x100000000). Mirror the function's own fallback: push the
// default float from 0x3A3880 and return (ret 4: return address + 1 arg).
X86_EXTERN_FUNC(__imp__sub_0027C6A0);
X86_FUNC(sub_0027C6A0) {
    auto ret_default = [&]() {
        float def = game::IsCommittedRange(0x3A3880u, 4) ? X86_MEMF32(0x3A3880u) : 0.0f;
        x87_push(ctx, (double)def);
        ctx.esp += 8;  // ret 4
    };
    uint32_t obj = ctx.ecx;
    if (!game::IsCommittedRange(obj, 0x20u)) {
        LOGR("career: sub_0027C6A0 bad obj %08X", obj);
        ret_default();
        return;
    }
    uint32_t p0c = kernel::R32(obj + 0x0Cu);
    uint32_t p18 = kernel::R32(obj + 0x18u);
    if (!p0c || !p18) {
        ret_default();
        return;
    }
    if (!game::IsCommittedRange(p0c + 0x10u, 4)) {
        LOGR("career: sub_0027C6A0 bad [obj+0x0C]+0x10 (obj %08X p0c %08X)", obj, p0c);
        ret_default();
        return;
    }
    uint32_t table = kernel::R32(p0c + 0x10u);
    uint32_t idx = kernel::R32(ctx.esp + 4);
    if (!table || !game::IsCommittedRange(table + idx * 4u, 4)) {
        LOGR("career: sub_0027C6A0 bad table %08X idx %u (obj %08X)", table, idx, obj);
        ret_default();
        return;
    }
    uint32_t addr = kernel::R32(table + idx * 4u) + p18;
    if (!game::IsCommittedRange(addr, 4)) {
        LOGR("career: sub_0027C6A0 bad stat addr %08X (obj %08X)", addr, obj);
        ret_default();
        return;
    }
    __imp__sub_0027C6A0(ctx, base);
}

// Guard STRM ring-buffer walk (0xC1B30) against uncommitted guest pointers on the background streamer thread.
X86_EXTERN_FUNC(__imp__sub_000C1B30);
X86_FUNC(sub_000C1B30) {
    uint32_t strm = kernel::R32(ctx.esp + 4);
    if (!game::IsCommittedRange(strm, 0x10u)) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    uint32_t hdr = kernel::R32(strm);
    uint32_t avail = kernel::R32(strm + 8u);
    uint32_t cur = kernel::R32(strm + 0xCu);
    if (!game::IsCommittedRange(hdr, 0x44u) || kernel::R32(hdr) != 0x4D525453u || !avail ||
        !game::IsCommittedRange(cur, 8u)) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    uint32_t skip = kernel::R32(cur + 4u) & 0xFFFFFFu;
    int32_t rem = int32_t(avail - skip);
    if (rem > 0) {
        uint32_t p = cur + skip;
        uint32_t target = kernel::R32(strm + 4u) << 24;
        for (int step = 0; step < 4096; step++) {
            if (!game::IsCommittedRange(p, 8u)) {
                ctx.eax = 0;
                ctx.esp += 4;
                return;
            }
            uint32_t tag = kernel::R32(p + 4u);
            if ((tag & 0xFF000000u) == target) break;
            if (kernel::R32(p) == 0xFFFFFFFFu) p = kernel::R32(hdr + 0x40u);
            else p += (tag & 0xFFFFFFu);
        }
    }
    __imp__sub_000C1B30(ctx, base);
}

namespace game {
namespace {

using ::game::kCFEng;  // cFEng's instance: FEngine at its first field, the packages' list at +0xE4

// FEng's packages as they open: the name's hash (+0x10) and every object under it (the list at +0x4C, the next at +4,
// a group's children at +0x64 and +0x60), so the HUD's groups can be found by hash as Most Wanted's were
static void DumpObjects(uint32_t o, int depth, int& budget) {
    for (; IsCommittedRange(o, 0x68) && budget-- > 0; o = kernel::R32(o + 4)) {
        uint32_t hash = kernel::R32(o + 0x10), type = kernel::R32(o + 0x18);
        // the data's place too: the groups are told apart by where they are drawn as well as by hash
        uint32_t data = kernel::R32(o + 0x2C);
        float x = 0, y = 0;
        if (IsCommittedRange(data + 0x1C, 8)) {
            uint32_t bx = kernel::R32(data + 0x1C), by = kernel::R32(data + 0x20);
            std::memcpy(&x, &bx, 4);
            std::memcpy(&y, &by, 4);
        }
        LOGR("feobj: %*s%08X t%u %d,%d", depth * 2, "", hash, type, int(x), int(y));
        if (type == 5 && depth < 8) DumpObjects(kernel::R32(o + 0x64), depth + 1, budget);
    }
}
static void DumpOpenPackages(bool dump_objects) {
    static uint32_t dumped[64] = {};
    static int n = 0;
    if (!mem::IsCommitted(kCFEng)) return;
    uint32_t cfeng = kernel::R32(kCFEng);
    uint32_t fe = cfeng && mem::IsCommitted(cfeng) ? kernel::R32(cfeng) : 0;
    int count = 0;
    for (uint32_t p = fe && mem::IsCommitted(fe + 0xE0) ? kernel::R32(fe + 0xE0) : 0;
         p && count < 64 && mem::IsCommitted(p + 0xC + 3); p = kernel::R32(p + 4), count++) {
        uint32_t name = kernel::R32(p + 0xC);
        bool seen = false;
        for (int i = 0; i < n; i++) seen = seen || dumped[i] == p;
        if (seen) continue;
        if (n < 64) dumped[n++] = p;
        LOGR("fepkg: open %s", name && mem::IsCommitted(name) ? (const char*)kernel::HostPtr(name) : "?");
        if (dump_objects && name && mem::IsCommitted(name)) {
            int budget = 400;
            DumpObjects(kernel::R32(p + 0x4C), 1, budget);
        }
    }
}

// the HUD's groups: a race's package (HUD_*) dumped in full, the front end's only for the EA Trax banner
static void DumpRaceHud() {
    static bool done = false;
    if (done || !mem::IsCommitted(::game::kCFEng)) return;
    uint32_t cfeng = kernel::R32(::game::kCFEng);
    uint32_t fe = cfeng && mem::IsCommitted(cfeng) ? kernel::R32(cfeng) : 0;
    int count = 0;
    for (uint32_t p = fe && mem::IsCommitted(fe + 0xE0) ? kernel::R32(fe + 0xE0) : 0;
         p && count < 64 && mem::IsCommitted(p + 0xC + 3); p = kernel::R32(p + 4), count++) {
        uint32_t name = kernel::R32(p + 0xC);
        if (!name || !mem::IsCommitted(name)) continue;
        const char* s = (const char*)kernel::HostPtr(name);
        if (std::strncmp(s, "HUD_", 4) != 0) continue;
        int budget = 2000;
        LOGR("fepkg: race %s", s);
        DumpObjects(kernel::R32(p + 0x4C), 1, budget);
        done = true;
    }
}

}  // namespace

// called at each present: each newly opened package name, object tree on the first pass, and the race HUD once
void DumpPackages() {
    static bool first = true;
    DumpOpenPackages(first);
    first = false;
    DumpRaceHud();
}

}  // namespace game
