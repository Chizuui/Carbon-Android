// The HUD's speedometer, minimap, race information and EA Trax banner moved where the touch layout editor puts them,
// out from under the touch controls. The groups are found by their names' hashes (FEHashUpper: -1, then times 33 plus
// each letter in capitals), the packages walked from cFEng's FEngine. An object's place is its data's x and y, FEng
// units from its group's middle, read as it is drawn: a group moved moves what is in it. The game writes some of those
// places each frame (the map scrolls its track) and the package's scripts others, so they are moved after the FEng
// is updated, the last writer before the drawing; and a place found other than as it was left is the game's new one,
// moved again from there
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "common/hud_layout.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

namespace {

constexpr uint32_t kCFEng = 0x46A7C8;  // cFEng's instance, its first field the FEngine
constexpr uint32_t kFirstPackage = 0xE4;  // FEngine's, each one's next at +8
constexpr uint32_t kPackageNext = 0x8, kPackageName = 0xC, kPackageObjects = 0x4C;
constexpr uint32_t kObjectNext = 0x4, kObjectHash = 0x10, kObjectType = 0x18, kObjectData = 0x2C;
constexpr uint32_t kGroupCount = 0x60, kGroupObjects = 0x64;
constexpr uint32_t kTypeGroup = 5;
constexpr uint32_t kDataX = 0x1C, kDataY = 0x20;
constexpr int kMaxPackages = 64, kMaxObjects = 1500;

struct Piece {
    uint32_t hash;
    HudItem item;
};
constexpr Piece kPieces[] = {
    {0xAB8A0643, kHudSpeedometer},  // the cluster: the dial and SpeedometerGroup's digits
    {0xE44B5D54, kHudSpeedometer},  // Speedbreaker meter group (icon + SPEEDBREAKER_METER)
    {0x28003CE1, kHudSpeedometer},  // X_MULTIPLIER_FLASHER
    {0x6762BD08, kHudSpeedometer},  // HEADERSECONDARY_DRIFT
    {0x53BA5E3A, kHudSpeedometer},  // HEADERSECONDARY_DRIFT2
    {0xAA6FB774, kHudSpeedometer},  // NOS_SHOW_HIDE_GROUP
    {0x3A163857, kHudSpeedometer},  // Nitrous gauge group
    {0x572181E6, kHudMinimap},      // sibling minimap container under FB3B0BD0 (-172, 308)
    {0x1603009E, kHudMinimap},      // the map: the ring, the rim, the roads, the waypoint
    {0x00A1A862, kHudMinimap},      // the map's icons: the player's arrow, the cops, the events
    {0x0F365871, kHudMinimap},      // TRACK_MAP: the four map tiles
    {0x6CD0C4B8, kHudMinimap},      // RADAR_RING
    {0x382D2FC9, kHudMinimap},      // TRACKMAPTARGETRING
    {0xB1C5ED0D, kHudRaceInfo},     // RaceInformationGroup: the position, the lap and the time
    {0x5ED609A1, kHudRaceInfo},     // LeaderBoardGroup: the racers under it
    {0x2CBBF14A, kHudRaceInfo},     // TOUGE_ZONE_GROUP
    {0x82308C63, kHudRaceInfo},     // PURSUITBOARD
    {0xABA3B16F, kHudRaceInfo},     // CANYON_METER_GROUP
    {0x9F569138, kHudWingman},      // WINGMAN_PIP_CONTROL (Crew / Wingman bar, child of 0x1603009E)
    {0x7FAC287B, kHudTrax},         // EA Trax's banner, in the race's package and in the front end's
};

struct Moved {
    float x, y;            // the place the game gave it
    float set_x, set_y;    // what was written over it
};
std::unordered_map<uint32_t, Moved> g_moved;  // by the object's data
std::mutex g_hud_mtx;

uint32_t Peek(uint32_t va) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + 3) ? kernel::R32(va) : 0; }

float Float(uint32_t va) {
    uint32_t b = kernel::R32(va);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
void SetFloat(uint32_t va, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    kernel::W32(va, b);
}

void Move(uint32_t data, float dx, float dy, std::unordered_map<uint32_t, Moved>& seen) {
    if (!data || !mem::IsCommitted(data) || !mem::IsCommitted(data + kDataY + 3)) return;
    float x = Float(data + kDataX), y = Float(data + kDataY);
    Moved m{x, y, x, y};
    if (auto it = g_moved.find(data); it != g_moved.end() && it->second.set_x == x && it->second.set_y == y) m = it->second;
    m.set_x = m.x + dx;
    m.set_y = m.y + dy;
    if (m.set_x != x) SetFloat(data + kDataX, m.set_x);
    if (m.set_y != y) SetFloat(data + kDataY, m.set_y);
    seen[data] = m;
}

void Walk(uint32_t first, int count, int& budget, std::unordered_map<uint32_t, Moved>& seen,
          float parent_dx = 0.0f, float parent_dy = 0.0f) {
    for (uint32_t o = first; o && budget-- > 0 && count != 0; o = Peek(o + kObjectNext), count--) {
        uint32_t hash = Peek(o + kObjectHash);
        bool stop = false;
        float child_dx = parent_dx, child_dy = parent_dy;
        for (const Piece& p : kPieces) {
            if (p.hash == hash) {
                float target_dx = g_hud_x[p.item].load();
                float target_dy = g_hud_y[p.item].load();
                Move(Peek(o + kObjectData), target_dx - parent_dx, target_dy - parent_dy, seen);
                // 0x1603009E parents WINGMAN_PIP_CONTROL (0x9F569138), so keep walking its children
                // with the minimap offset so Crew Bar can cancel it out and move independently.
                if (hash == 0x1603009Eu) {
                    child_dx = target_dx;
                    child_dy = target_dy;
                } else {
                    stop = true;
                }
                break;
            }
        }
        if (!stop && Peek(o + kObjectType) == kTypeGroup)
            Walk(Peek(o + kGroupObjects), int(Peek(o + kGroupCount)), budget, seen, child_dx, child_dy);
    }
}

bool HudPackage(uint32_t p) {
    uint32_t name = Peek(p + kPackageName);
    char head[4] = {};
    for (uint32_t i = 0; name && i < 4 && mem::IsCommitted(name + i); i++) head[i] = char(kernel::G<uint8_t>(name + i));
    return std::memcmp(head, "HUD_", 4) == 0 || std::memcmp(head, "EA_T", 4) == 0;  // EA_TRAX.fng
}

}  // namespace

void ApplyHudLayout() {
    std::lock_guard<std::mutex> lock(g_hud_mtx);
    uint32_t cfeng = Peek(kCFEng), fe = cfeng ? Peek(cfeng) : 0;
    std::unordered_map<uint32_t, Moved> seen;
    int n = 0;
    for (uint32_t p = fe ? Peek(fe + kFirstPackage) : 0; p && n < kMaxPackages; p = Peek(p + kPackageNext), n++) {
        if (!HudPackage(p)) continue;
        int budget = kMaxObjects;
        Walk(Peek(p + kPackageObjects), -1, budget, seen);
    }
    g_moved.swap(seen);  // a package gone forgets its objects
}

// The yellow route polyline ([minimap+0x158]) is rendered directly in screen space by sub_00158AE0
// (and its clip rect initialized in sub_00158520) using [minimap+0xB4/+0xB8], bypassing FEng group
// transforms. Shift +0xB4/+0xB8 ONLY for those two calls — NOT for sub_001581D0 or sub_00165C00,
// whose FEng child objects already inherit the parent group shift from ApplyHudLayout().
struct MinimapRouteOriginScope {
    uint32_t obj = 0;
    float orig_x = 0.0f, orig_y = 0.0f;
    bool active = false;

    explicit MinimapRouteOriginScope(uint32_t minimap) : obj(minimap) {
        constexpr uint32_t kMinimapOriginX = 0xB4, kMinimapOriginY = 0xB8;
        if (!obj || !mem::IsCommitted(obj + kMinimapOriginX) || !mem::IsCommitted(obj + kMinimapOriginY + 3)) return;
        float dx = g_hud_x[kHudMinimap].load(std::memory_order_relaxed);
        float dy = g_hud_y[kHudMinimap].load(std::memory_order_relaxed);
        if (dx == 0.0f && dy == 0.0f) return;
        orig_x = Float(obj + kMinimapOriginX);
        orig_y = Float(obj + kMinimapOriginY);
        SetFloat(obj + kMinimapOriginX, orig_x + dx);
        SetFloat(obj + kMinimapOriginY, orig_y + dy);
        active = true;
    }

    ~MinimapRouteOriginScope() {
        constexpr uint32_t kMinimapOriginX = 0xB4, kMinimapOriginY = 0xB8;
        if (!active) return;
        SetFloat(obj + kMinimapOriginX, orig_x);
        SetFloat(obj + kMinimapOriginY, orig_y);
    }
};

}  // namespace game

// FEngine::UpdateAllPackages (thiscall, 0 args): called by cFEng::Service (0x14D990) after HUD elements write
// their positions and before FEng renders, so moving here catches per-frame updates before drawing.
X86_EXTERN_FUNC(__imp__sub_0014D8B0);
X86_FUNC(sub_0014D8B0) {
    __imp__sub_0014D8B0(ctx, base);
    game::ApplyHudLayout();
}

X86_EXTERN_FUNC(__imp__sub_00158520);
X86_FUNC(sub_00158520) {
    game::MinimapRouteOriginScope origin_scope(ctx.ecx);
    __imp__sub_00158520(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00158AE0);
X86_FUNC(sub_00158AE0) {
    game::MinimapRouteOriginScope origin_scope(ctx.ecx);
    __imp__sub_00158AE0(ctx, base);
}
