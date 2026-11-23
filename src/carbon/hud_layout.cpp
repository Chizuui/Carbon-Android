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

// The street grid's lines are positioned by the 30 road objects' own static rects (their data[0..15],
// integers — dword 3 is a shared scale), NOT by the FEng position (data+0x1C/+0x20) that Move() shifts.
// Those objects live in a global array (guest 0x46AB28, 30 entries, 0x14 stride) that the package walk
// never reaches, so when the minimap is dragged the grid stays at the original origin (top-left) while
// the route (drawn from the shifted origin) lands in the moved minimap — the "double map". Shift the
// rects' X/Y (dwords 0,1) by the minimap's offset, persistently: this runs in UpdateAllPackages, before
// FEng renders, unlike a hook scope that restores before render. Each object's original rect is tracked
// so a changed offset or a reloaded road network does not accumulate or corrupt it. RE 2026-11-23: the
// 73f7226 MinimapRectScope (shift data+0x1C/+0x20 in the 158AE0 hook) is ineffective — the rect is
// independent of the position and the scope restores before render — so it was removed.
struct RoadRect {
    int32_t orig0 = 0, orig1 = 0;
    int32_t last_dx = 0, last_dy = 0;
    bool seen = false;
};
void ShiftRoadRects() {
    float dxf = g_hud_x[kHudMinimap].load(std::memory_order_relaxed);
    float dyf = g_hud_y[kHudMinimap].load(std::memory_order_relaxed);
    int32_t sdx = static_cast<int32_t>(dxf + (dxf >= 0.0f ? 0.5f : -0.5f));
    int32_t sdy = static_cast<int32_t>(dyf + (dyf >= 0.0f ? 0.5f : -0.5f));
    static std::unordered_map<uint32_t, RoadRect> rects;
    static int last_logged = -1;
    constexpr uint32_t kRoadArray = 0x46AB28;
    int shifted = 0;
    for (int i = 0; i < 30; i++) {
        uint32_t obj = Peek(kRoadArray + uint32_t(i) * 0x14u);
        if (!obj) continue;
        uint32_t data = Peek(obj + kObjectData);
        if (!data || !mem::IsCommitted(data + 7)) continue;
        int32_t cur0 = static_cast<int32_t>(kernel::R32(data));
        int32_t cur1 = static_cast<int32_t>(kernel::R32(data + 4));
        RoadRect& r = rects[data];
        if (!r.seen) {
            r.orig0 = cur0; r.orig1 = cur1; r.seen = true;
        } else if (cur0 != r.orig0 + r.last_dx || cur1 != r.orig1 + r.last_dy) {
            // The game rewrote the rect (new road network) — take the new values as the original.
            r.orig0 = cur0; r.orig1 = cur1; r.last_dx = 0; r.last_dy = 0;
        }
        if (sdx != r.last_dx || sdy != r.last_dy) {
            kernel::W32(data, static_cast<uint32_t>(r.orig0 + sdx));
            kernel::W32(data + 4, static_cast<uint32_t>(r.orig1 + sdy));
            r.last_dx = sdx; r.last_dy = sdy;
        }
        shifted++;
    }
    if (shifted != last_logged) {
        LOGR("minimap: road rects shifted %d (dx=%d dy=%d)", shifted, sdx, sdy);
        last_logged = shifted;
    }
}

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
    ShiftRoadRects();
}

// FeVar access (mirrors carbon.cpp's QueryFeVar/SetFeVar): sub_0007DD00 reads a feature var,
// sub_0007DD50 writes one. this = 0x456830. Args pushed: 0, id, out/val, idx, -1, -1.
X86_EXTERN_FUNC(sub_0007DD00);
X86_EXTERN_FUNC(sub_0007DD50);
bool IsCommittedRange(uint32_t va, uint32_t size);  // defined in carbon.cpp

uint32_t ReadFeVar(const X86Context& base_ctx, uint8_t* base, uint32_t id, uint32_t def_val) {
    X86Context sc = base_ctx;
    uint32_t out_va = sc.esp - 16u;
    if (!IsCommittedRange(out_va - 0x400u, 0x410u)) return def_val;
    kernel::W32(out_va, def_val);
    sc.esp = out_va;
    sc.ecx = 0x456830u;
    auto push32 = [&](uint32_t v) {
        sc.esp -= 4u;
        kernel::W32(sc.esp, v);
    };
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(out_va);
    push32(id);
    push32(0u);
    sub_0007DD00(sc, base);
    return kernel::R32(out_va);
}

void WriteFeVar(const X86Context& base_ctx, uint8_t* base, uint32_t id, uint32_t val) {
    X86Context sc = base_ctx;
    sc.esp -= 16u;
    if (!IsCommittedRange(sc.esp - 0x400u, 0x400u)) return;
    sc.ecx = 0x456830u;
    auto push32 = [&](uint32_t v) {
        sc.esp -= 4u;
        kernel::W32(sc.esp, v);
    };
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(val);
    push32(id);
    push32(0u);
    sub_0007DD50(sc, base);
}

// Free roam / pursuit have no race route, so sub_00158520's gate exits early (the 0xBCD FeVar holds
// the "no route" sentinel 0x99C27E3B) and no road data is built, leaving [esi+0x164]==0 so 158AE0
// draws nothing — the empty minimap. The rebuild itself does not need a route: it walks the world
// road graph (0x4B612C/0x4B6170/0x4B6174, never reads the route id in the loop) and only stores the
// route id into [esi+0x15C]. So when there is no route, lend the rebuild a dummy route id for the
// call only: set the 0xBCD FeVar to a non-sentinel value, call the original (which builds the roads),
// restore the sentinel. The dummy is a constant, so the rebuild behaves exactly like a race with a
// constant route id — the capacity check, not a route change, drives updates as the player drives.
// The FeVar is restored before the hook returns, so no other code ever sees the dummy. Only activates
// when the road-object array is populated (the world is loaded and there are roads to draw); otherwise
// the map stays empty as before. If the 0xBC3 gate (==0x0C) also fires, the bypass silently does
// nothing (map stays empty, no crash). RE 2026-11-23.
struct NoRouteRoadScope {
    X86Context ctx;
    uint8_t* base = nullptr;
    bool active = false;
    static constexpr uint32_t kRouteVar = 0xBCD;
    static constexpr uint32_t kNoRouteSentinel = 0x99C27E3B;
    static constexpr uint32_t kDummyRoute = 0x524F4144;  // "ROAD", not a real route hash

    explicit NoRouteRoadScope(const X86Context& c, uint8_t* b) : ctx(c), base(b) {
        uint32_t cur = ReadFeVar(ctx, base, kRouteVar, 0u);
        if (cur != kNoRouteSentinel) return;  // real route (or unreadable): leave alone
        if (!RoadArrayPopulated()) return;    // no road network loaded: nothing to draw
        WriteFeVar(ctx, base, kRouteVar, kDummyRoute);
        active = true;
        static int logged = 0;
        if (logged < 8) {
            LOGR("minimap: no route, lending dummy route id for road build");
            logged++;
        }
    }

    ~NoRouteRoadScope() {
        if (!active) return;
        WriteFeVar(ctx, base, kRouteVar, kNoRouteSentinel);
    }

    static bool RoadArrayPopulated() {
        constexpr uint32_t kRoadArray = 0x46AB28;
        for (int i = 0; i < 30; i++) {
            if (Peek(kRoadArray + uint32_t(i) * 0x14u)) return true;
        }
        return false;
    }
};

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
    game::NoRouteRoadScope route(ctx, base);
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

