// The HUD's speedometer, minimap, race information and EA Trax banner moved where the touch layout editor puts them,
// out from under the touch controls. The groups are found by their names' hashes (FEHashUpper: -1, then times 33 plus
// each letter in capitals), the packages walked from cFEng's FEngine. An object's place is its data's x and y, FEng
// units from its group's middle, read as it is drawn: a group moved moves what is in it. The game writes some of those
// places each frame (the map scrolls its track) and the package's scripts others, so they are moved after the FEng
// is updated, the last writer before the drawing; and a place found other than as it was left is the game's new one,
// moved again from there
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

// the groups of each item, by their names' hashes: the speedometer's cluster (the dial, the speed, the needle, the
// nitrous and the speedbreaker gauges), the minimap's (its track, the icons the game places on it, the ring round the
// event it points to, the roads and the rim), the race's information at the top right (the position, the lap and the
// time, the racers under it) and EA Trax's banner
struct Piece {
    uint32_t hash;
    HudItem item;
};
constexpr Piece kPieces[] = {
    // HUD_SingleRace.fng as Carbon dumps it: FB3B0BD0 holds the minimap's container (1603009E, the ring, the rim, the
    // roads and the waypoint under it), the icons (00A1A862) and TRACK_MAP's tiles (0F365871); the cluster (5D0101F1)
    // holds the race's information (B1C5ED0D with the position's, the lap's and the time's digits under it, the racers
    // in 5ED609A1) and the speedometer's own cluster (AB8A0643, the dial and the digits' group 941FFF09). EA Trax's
    // banner is a root object of its package. Only containers are listed: a group found is not walked into, so its
    // children move with it and must not be moved again
    {0xAB8A0643, kHudSpeedometer},  // the cluster: the dial and SpeedometerGroup's digits
    {0xE44B5D54, kHudSpeedometer},  // Speedbreaker meter group (icon + SPEEDBREAKER_METER)
    {0x28003CE1, kHudSpeedometer},  // X_MULTIPLIER_FLASHER
    {0x6762BD08, kHudSpeedometer},  // HEADERSECONDARY_DRIFT
    {0x53BA5E3A, kHudSpeedometer},  // HEADERSECONDARY_DRIFT2
    {0xAA6FB774, kHudSpeedometer},  // NOS_SHOW_HIDE_GROUP
    {0x3A163857, kHudSpeedometer},  // Nitrous gauge group
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

void Move(uint32_t data, HudItem item, std::unordered_map<uint32_t, Moved>& seen) {
    if (!data || !mem::IsCommitted(data) || !mem::IsCommitted(data + kDataY + 3)) return;
    float x = Float(data + kDataX), y = Float(data + kDataY);
    Moved m{x, y, x, y};
    if (auto it = g_moved.find(data); it != g_moved.end() && it->second.set_x == x && it->second.set_y == y) m = it->second;
    m.set_x = m.x + g_hud_x[item].load();
    m.set_y = m.y + g_hud_y[item].load();
    if (m.set_x != x) SetFloat(data + kDataX, m.set_x);
    if (m.set_y != y) SetFloat(data + kDataY, m.set_y);
    seen[data] = m;
}

void Walk(uint32_t first, int count, int& budget, std::unordered_map<uint32_t, Moved>& seen) {
    for (uint32_t o = first; o && budget-- > 0 && count != 0; o = Peek(o + kObjectNext), count--) {
        uint32_t hash = Peek(o + kObjectHash);
        bool piece = false;
        for (const Piece& p : kPieces)
            if (p.hash == hash) {
                Move(Peek(o + kObjectData), p.item, seen);
                piece = true;
            }
        if (!piece && Peek(o + kObjectType) == kTypeGroup) Walk(Peek(o + kGroupObjects), int(Peek(o + kGroupCount)), budget, seen);
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

struct MinimapOriginScope {
    uint32_t obj = 0;
    float orig_x = 0.0f, orig_y = 0.0f;
    bool active = false;

    explicit MinimapOriginScope(uint32_t minimap) : obj(minimap) {
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

    ~MinimapOriginScope() {
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

// Hud_Minimap GPS route rebuild & render read cached TRACK_MAP coordinates at +0xB4/+0xB8 directly instead of
// FEng object positions; shift them only for those calls so helper methods reading +0xB4/+0xB8 aren't double-shifted.
// Diagnostic (tester bug: the minimap is empty in a pursuit — "map ilang"): count entries per function so a
// race vs pursuit log shows whether the route rebuild early-outs when there is no active race route. Rate-limited.
X86_EXTERN_FUNC(__imp__sub_00158520);
X86_FUNC(sub_00158520) {
    static int n_158520 = 0;
    if ((++n_158520 % 600) == 1) LOGR("minimap: sub_00158520 entry #%d", n_158520);
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_00158520(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00158AE0);
X86_FUNC(sub_00158AE0) {
    static int n_158ae0 = 0;
    if ((++n_158ae0 % 600) == 1) LOGR("minimap: sub_00158AE0 entry #%d", n_158ae0);
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_00158AE0(ctx, base);
}

// The same stale-origin defect in the remaining Hud_Minimap readers of +0xB4/+0xB8: the target ring
// (TRACKMAPTARGETRING) and the other map elements they position stay at the unmoved map's origin when the
// minimap is dragged, because only 158520/158AE0 above were shifted. All three take the minimap in ecx
// (esi is copied from ecx at entry, and the origin is read via esi) — 15870A's `this` is inferred from the
// same pattern, it has no direct callers in the recompiled code.
X86_EXTERN_FUNC(__imp__sub_001581D0);
X86_FUNC(sub_001581D0) {
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_001581D0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0015870A);
X86_FUNC(sub_0015870A) {
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_0015870A(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00165C00);
X86_FUNC(sub_00165C00) {
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_00165C00(ctx, base);
}

