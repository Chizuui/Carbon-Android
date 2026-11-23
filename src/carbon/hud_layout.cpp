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
#include <vector>

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

bool IsCommittedRange(uint32_t va, uint32_t size);  // defined in carbon.cpp

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
//
// RECT LAYOUT — STILL UNPINNED, and the 81fb86c shift-all-four experiment PROVED shifting dwords 2,3
// is wrong: on 81fb86c half the yellow road network vanished (screenshots + logs 2026-11-23 13:34-13:37,
// tester dx=29 dy=-207 and dx=-5 dy=-268), while 07fddb5 (shift dwords 0,1 only) showed the full
// network. The first-sight diagnostic logged orig=(65,255,155,255) for the first 4 rects — all
// identical, so those are template/placeholder values, not the real per-road rects (d1==d3==255 fits
// a horizontal line (x1,y1,x2,y2) OR (x,y,w,h) with h=255; (l,t,r,b) is ruled out — height would be 0).
// So this reverts to shifting dwords 0,1 only (the 07fddb5 behaviour, full yellow network). The white
// grid itself still never draws under either shift, so its cause is elsewhere (the road objects may
// draw yellow, not white, or may not draw at all) — the rewrite diagnostic below logs the REAL rect
// values when the game sets them, which will pin the layout. NOTE on the 164-clearing (8d4fdfd): it
// never fires in any log, because the clearing code reads ctx.ecx AFTER the original 158520 returns
// and 158520 clobbers ecx — it must use game::g_minimap_obj instead. Do NOT fix that yet: until the
// white grid is proven to draw, firing the clearing would leave free roam with an EMPTY map (yellow
// suppressed, no white grid) — worse than yellow roads.
struct RoadRect {
    int32_t orig0 = 0, orig1 = 0;
    int32_t last_dx = 0, last_dy = 0;
    bool seen = false;
};
std::unordered_map<uint32_t, RoadRect> g_road_rects;  // by the object's data pointer
uint32_t g_minimap_obj = 0;  // Hud_Minimap game object, cached from the 158520/165C00 hooks (ctx.ecx)
int g_last_logged_rects = -1;
int g_rect_diag_logged = 0;  // rate-limit for the rect-values diagnostic below
int g_rect_rewrite_logged = 0;  // rate-limit for the rewrite (real values) diagnostic below

void ShiftOneRect(uint32_t data, int32_t sdx, int32_t sdy, int& shifted) {
    // Guard the whole rect (4 dwords, 16 bytes): data comes from a chained guest pointer (road
    // array -> obj -> data), and a corrupt entry yielding a committed-but-wrong data would make
    // the writes below corrupt arbitrary committed memory every present. A single-page check on
    // data+7 misses a page straddle, so validate the full 16-byte range.
    if (!data || !game::IsCommittedRange(data, 16)) return;
    int32_t cur0 = static_cast<int32_t>(kernel::R32(data));
    int32_t cur1 = static_cast<int32_t>(kernel::R32(data + 4));
    RoadRect& r = g_road_rects[data];
    if (!r.seen) {
        r.orig0 = cur0; r.orig1 = cur1; r.seen = true;
        // Diagnostic: log the first road object's original rect + shift (first 4). The first-sight
        // values are template/placeholder (all identical so far), so the rewrite log below is the
        // one that pins the layout: if the real d2>d0 and d3>d1 it is (l,t,r,b)/(x1,y1,x2,y2); if
        // the real d2,d3 are small while d0,d1 are large it is (x,y,w,h).
        if (g_rect_diag_logged < 4) {
            int32_t cur2 = mem::IsCommitted(data + 15) ? static_cast<int32_t>(kernel::R32(data + 8)) : 0;
            int32_t cur3 = mem::IsCommitted(data + 15) ? static_cast<int32_t>(kernel::R32(data + 12)) : 0;
            LOGR("minimap: road rect orig=(%d,%d,%d,%d) shift=(%d,%d)", r.orig0, r.orig1, cur2, cur3, sdx, sdy);
            g_rect_diag_logged++;
        }
    } else if (cur0 != r.orig0 + r.last_dx || cur1 != r.orig1 + r.last_dy) {
        // The game rewrote the rect (new road network / real values replacing the template) — take
        // the new values as the original, and log the real rect so the layout can be pinned.
        if (g_rect_rewrite_logged < 4) {
            int32_t cur2 = mem::IsCommitted(data + 15) ? static_cast<int32_t>(kernel::R32(data + 8)) : 0;
            int32_t cur3 = mem::IsCommitted(data + 15) ? static_cast<int32_t>(kernel::R32(data + 12)) : 0;
            LOGR("minimap: road rect rewritten to=(%d,%d,%d,%d) shift=(%d,%d)", cur0, cur1, cur2, cur3, sdx, sdy);
            g_rect_rewrite_logged++;
        }
        r.orig0 = cur0; r.orig1 = cur1; r.last_dx = 0; r.last_dy = 0;
    }
    if (sdx != r.last_dx || sdy != r.last_dy) {
        kernel::W32(data, static_cast<uint32_t>(r.orig0 + sdx));
        kernel::W32(data + 4, static_cast<uint32_t>(r.orig1 + sdy));
        r.last_dx = sdx; r.last_dy = sdy;
    }
    shifted++;
}

void ShiftRoadRects() {
    float dxf = g_hud_x[kHudMinimap].load(std::memory_order_relaxed);
    float dyf = g_hud_y[kHudMinimap].load(std::memory_order_relaxed);
    int32_t sdx = static_cast<int32_t>(dxf + (dxf >= 0.0f ? 0.5f : -0.5f));
    int32_t sdy = static_cast<int32_t>(dyf + (dyf >= 0.0f ? 0.5f : -0.5f));
    constexpr uint32_t kRoadArray = 0x46AB28;
    int shifted = 0;
    for (int i = 0; i < 30; i++) {
        uint32_t obj = Peek(kRoadArray + uint32_t(i) * 0x14u);
        if (!obj) continue;
        ShiftOneRect(Peek(obj + kObjectData), sdx, sdy, shifted);
    }
    // The POI / GPS-route lines (race markers, Car Lot, Safe House, the white route line) are drawn by
    // sub_00158AE0 from a SECOND rect source the road-array shift does not cover: the FEng object at
    // minimap+0x30, whose rect sub_0011AB70 copies into [ebp-0x60] (guest 0x158B23) for the draws in the
    // 0x2DC2FF / 0x30B0D colours. That object is not in the 0x46AB28 array and is not reached by the
    // package walk, so when the minimap is dragged its lines stay at the original origin (top-left)
    // while the circle moves — the POIs/route appear detached from the minimap. Shift its rect the
    // same way, through the same tracking map (so if it ever coincides with a road object it is only
    // shifted once). The minimap game object is cached from the 158520/165C00 hooks (ctx.ecx).
    // The minimap game object is cached from the 158520/165C00 hooks (ctx.ecx). Validate the whole
    // object header (0x34 bytes, covers the [minimap+0x30] rect-object read) before trusting it —
    // a stale cached pointer would read garbage and shift a wrong object's rect.
    if (g_minimap_obj && game::IsCommittedRange(g_minimap_obj, 0x34)) {
        uint32_t rect_obj = kernel::R32(g_minimap_obj + 0x30u);
        if (rect_obj) ShiftOneRect(Peek(rect_obj + kObjectData), sdx, sdy, shifted);
    }
    if (shifted != g_last_logged_rects) {
        LOGR("minimap: road rects shifted %d (dx=%d dy=%d)", shifted, sdx, sdy);
        g_last_logged_rects = shifted;
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

// Free roam / pursuit have no race route, so sub_00158520's gate exits early and no road data is built,
// leaving [esi+0x164]==0 so 158AE0 draws nothing — the empty minimap. The rebuild itself does not need
// a route: it walks the world road graph (0x4B612C/0x4B6170/0x4B6174, never reads the route id in the
// loop) and only stores the route id into [esi+0x15C]. So when there is no route, lend the rebuild a
// dummy route id for the call only: set the 0xBCD FeVar to a non-sentinel value, call the original
// (which builds the roads), restore the sentinel. The dummy is a constant, so the rebuild behaves
// exactly like a race with a constant route id — the capacity check, not a route change, drives updates
// as the player drives. The FeVar is restored before the hook returns, so no other code ever sees the
// dummy. Only activates when the road-object array is populated (the world is loaded and there are
// roads to draw); otherwise the map stays empty as before. If the 0xBC3 gate (==0x0C) also fires, the
// bypass silently does nothing (map stays empty, no crash). RE 2026-11-23.
// CORRECTION 2026-11-23: the bypass never fired (0 "lending dummy route id" lines in every log) because
// the gate does NOT exit via the sentinel in free roam. The 0xBCD getter (sub_0007DD00) returns FALSE
// (xor al,al at 0x7DD3B) when the FeVar's hash bucket is NULL — i.e. the race-route FeVar does not exist
// at all in free roam — so the gate exits via the getter-false exit, and ReadFeVar returns the default 0,
// which the old `cur != sentinel` check treated as "real route, leave alone". Fire also when cur==0
// (no FeVar / no route). Caveat: the setter (sub_0007DD50) also returns false on a NULL bucket and does
// NOT create a missing FeVar, so if the FeVar truly does not exist the write fails silently and the
// bypass is a no-op — but if the FeVar exists and is 0, the write succeeds and the gate passes. The
// diagnostic below logs the actual FeVar value so the next log proves which case it is.
// CORRECTION 2026-11-23 (2): the diagnostic proved it — `0xBCD FeVar = 00000000` in free roam on two
// devices, so the FeVar genuinely does not exist and the WriteFeVar below is a silent no-op; the
// bypass fired 8x in the log but 158AE0 still never ran. The real bypass is now the sub_0007DD00
// getter hook below: when 158520's 0xBCD gate call finds no FeVar, the hook reports the dummy route
// id as found, so the gate passes and the original rebuilds the roads. This scope is kept for its
// diagnostic and its log lines (they prove the hook ran and the road array was populated); its FeVar
// write is best-effort only. The dummy id also flows to sub_001849F0 (route lookup), which returns
// NULL for it, so 19C670/19C580 get a NULL `this` — they have NULL guards below for that.
struct NoRouteRoadScope {
    X86Context ctx;
    uint8_t* base = nullptr;
    bool active = false;
    static constexpr uint32_t kRouteVar = 0xBCD;
    static constexpr uint32_t kNoRouteSentinel = 0x99C27E3B;
    static constexpr uint32_t kDummyRoute = 0x524F4144;  // "ROAD", not a real route hash

    explicit NoRouteRoadScope(const X86Context& c, uint8_t* b) : ctx(c), base(b) {
        uint32_t cur = ReadFeVar(ctx, base, kRouteVar, 0u);
        // Diagnostic: log the actual 0xBCD value the first few times 158520 runs, so the next log
        // proves whether free roam has the sentinel, no FeVar (0), or a real route id.
        static int probed = 0;
        if (probed < 4) {
            LOGR("minimap: 0xBCD FeVar = %08X", cur);
            probed++;
        }
        if (cur != kNoRouteSentinel && cur != 0u) return;  // real route: leave alone
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

// Free roam has no race route, so no road-graph node carries the "on route" flag (bit15 at
// node+0x0A) the rebuild's walk requires (guest 0x1586C6: bit15 set, bit11 clear), and the walk
// builds 0 segments even though the graph is loaded (5790 nodes, diagnostic 2026-11-23). The
// route-marking pass (sub_001A1910, from the GPS update sub_001B0660) is the only code that sets
// bit15 — up to 100 nodes along a computed route — and bit11 is never written by code (it comes
// from the RNsg disc data). So in free roam, temporarily lend bit15 to the nodes near the player
// for the synchronous rebuild call, then restore every node's original flags. The player position
// is the minimap's own centre: the per-frame driver (sub_00169270) has 0x15DA60 fill [esi+0xAC] =
// player z and [esi+0xB0] = -player x, then normalises them with the same constants the walk uses
// ([0x3B3FBC]=-7300, [0x3B3FB8]=1/16000, [0x3B3FB4]=-2000, [0x3A386C]=1.0), so at rebuild time they
// hold the previous frame's normalised (u,v) = (z,-x). The visible radius follows the walk's
// world-to-FEng scale (A0/1000 FEng per world unit, A0 = [esi+0xA0]) and the RADAR_RING radius
// (~64 FEng, measured from a tester screenshot and the 128x128 TRACK_MAP tile area): R = 64000/A0,
// clamped to [300,4000]. Only nodes whose segment midpoint is within R are marked (cap 512); the
// rest of the graph is untouched. The flags are restored in the destructor, so no other code — the
// walk's neighbour scan, the traffic AI, the route marking — ever sees a modified flag outside
// this call. Nodes that already had bit15 (a stale route marking; the game never clears it) keep
// it: the original u16 is saved and restored verbatim, never just cleared. RE 2026-11-23.
struct FreeRoamRoadScope {
    struct Saved {
        uint32_t node = 0;
        uint16_t flags = 0;
    };
    std::vector<Saved> saved;

    explicit FreeRoamRoadScope(const X86Context& ctx, uint8_t* base, uint32_t minimap) {
        if (!minimap) return;
        // Free roam only: a real route already marks its nodes, and races must not change.
        uint32_t cur = ReadFeVar(ctx, base, 0xBCDu, 0u);
        if (cur != 0u && cur != 0x99C27E3Bu) return;
        if (!NoRouteRoadScope::RoadArrayPopulated()) return;
        if (!mem::IsCommitted(minimap + 0xA0u) || !mem::IsCommitted(minimap + 0xA3u) ||
            !mem::IsCommitted(minimap + 0xACu) || !mem::IsCommitted(minimap + 0xAFu) ||
            !mem::IsCommitted(minimap + 0xB0u) || !mem::IsCommitted(minimap + 0xB3u)) return;
        float norm_u = Float(minimap + 0xACu);  // (player z + 7300) / 16000
        float norm_v = Float(minimap + 0xB0u);  // 1 - ((-player x) + 2000) / 16000
        if (!(norm_u >= 0.0f && norm_u <= 1.0f && norm_v >= 0.0f && norm_v <= 1.0f)) return;
        if (norm_u == 0.0f && norm_v == 0.0f) return;  // never set
        float a0 = Float(minimap + 0xA0u);
        if (!(a0 > 0.0f) || !std::isfinite(a0)) return;
        // Player centre in (u,v) = (z,-x) world units, inverting the driver's normalisation.
        float u_p = norm_u * 16000.0f - 7300.0f;
        float v_p = (1.0f - norm_v) * 16000.0f - 2000.0f;
        // Visible world radius: the walk maps world to FEng at A0/1000 per unit, and the
        // RADAR_RING radius is ~64 FEng, so R = 64000/A0. Clamp to a sane band.
        float r = 64000.0f / a0;
        if (r < 300.0f) r = 300.0f;
        if (r > 4000.0f) r = 4000.0f;
        float r2 = r * r;

        uint32_t count = Peek(0x4B612Cu);
        uint32_t nodes = Peek(0x4B6174u);
        uint32_t pos = Peek(0x4B6170u);
        if (!count || !nodes || !pos) return;
        if (count > 20000u) return;  // sanity: not a real graph

        constexpr int kMaxMark = 512;
        int marked = 0;
        for (uint32_t i = 0; i < count && marked < kMaxMark; i++) {
            uint32_t node = nodes + i * 0x16u;
            if (!mem::IsCommitted(node + 0x0Au) || !mem::IsCommitted(node + 0x0Bu)) continue;
            uint16_t flags = kernel::R16(node + 0x0Au);
            if (flags & 0x8000u) continue;  // already on a route (stale marking): leave it
            if (flags & 0x0800u) continue;  // bit11 set (disc data): the walk skips it anyway
            if (!mem::IsCommitted(node) || !mem::IsCommitted(node + 3)) continue;
            uint16_t e1 = kernel::R16(node);
            uint16_t e2 = kernel::R16(node + 2u);
            uint32_t p1 = pos + uint32_t(e1) * 32u;
            uint32_t p2 = pos + uint32_t(e2) * 32u;
            if (!mem::IsCommitted(p1) || !mem::IsCommitted(p1 + 3) ||
                !mem::IsCommitted(p1 + 8) || !mem::IsCommitted(p1 + 11) ||
                !mem::IsCommitted(p2) || !mem::IsCommitted(p2 + 3) ||
                !mem::IsCommitted(p2 + 8) || !mem::IsCommitted(p2 + 11)) continue;
            // (u,v) = (z,-x): u = pos z (+8), v = -pos x (+0). Midpoint of the segment.
            float um = (Float(p1 + 8u) + Float(p2 + 8u)) * 0.5f;
            float vm = (-Float(p1) + -Float(p2)) * 0.5f;
            float du = um - u_p, dv = vm - v_p;
            if (du * du + dv * dv > r2) continue;
            saved.push_back({node, flags});
            kernel::W16(node + 0x0Au, static_cast<uint16_t>(flags | 0x8000u));
            marked++;
        }
        static int logged = 0;
        if (logged < 4) {
            LOGR("minimap: free-roam marking: marked %d nodes near player (u=%.0f v=%.0f R=%.0f A0=%.1f)%s",
                 marked, u_p, v_p, r, a0, marked >= kMaxMark ? " (capped)" : "");
            logged++;
        }
    }

    ~FreeRoamRoadScope() {
        for (const Saved& s : saved) kernel::W16(s.node + 0x0Au, s.flags);
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
    game::g_minimap_obj = ctx.ecx;  // cache for ShiftRoadRects' minimap+0x30 rect object
    game::MinimapOriginScope scope(ctx.ecx);
    game::NoRouteRoadScope route(ctx, base);
    game::FreeRoamRoadScope mark(ctx, base, ctx.ecx);
    // Diagnostic (2026-11-23): the getter-hook bypass passes the gate (hook fired 8x in the log)
    // but 0 segments are built — 158AE0 never runs and 1849F0 is never called (the 19C670/19C580
    // NULL guards were never hit), so the tail's `segments>0` check at guest 0x1589A2 fails. The
    // post-gate walk is skipped entirely when the road-graph node count [0x4B612C]==0 (guest
    // 0x158698 `jbe 0x158ACB`), or builds 0 segments when every node fails the flag filter at
    // +0x0A (bit15 set, bit11 clear, guest 0x1586C6/0x1586D3). Log the graph state the walk sees,
    // plus the [esi+0x164] flag after the call (1 = segments were built, 0 = the walk produced
    // nothing). Free roam only (no route FeVar), first 4 activations. The diagnostic proved the
    // filter case (5790 nodes loaded, [esi+0x164]==0), and FreeRoamRoadScope above now lends bit15
    // to the nodes near the player, so [esi+0x164] should become 1 in the next log.
    static int probed = 0;
    bool probe = false;
    if (probed < 4) {
        uint32_t cur = game::ReadFeVar(ctx, base, 0xBCDu, 0u);
        probe = (cur == 0u || cur == 0x99C27E3Bu);
        if (probe) {
            LOGR("minimap: road graph nodes=%u nodes_ptr=%08X pos_ptr=%08X loaded=%08X",
                 game::Peek(0x4B612Cu), game::Peek(0x4B6174u), game::Peek(0x4B6170u), game::Peek(0x4B6168u));
        }
    }
    __imp__sub_00158520(ctx, base);
    if (probe) {
        uint32_t mm = game::g_minimap_obj;
        uint32_t flag = (mm && mem::IsCommitted(mm + 0x164u)) ? (game::Peek(mm + 0x164u) & 0xFFu) : 0xFFFFFFFFu;
        LOGR("minimap: after rebuild, [esi+0x164]=%u", flag);
        probed++;
    }
    // Free roam only (no race route FeVar): the walk built route segments from the lent bit-15
    // flags (FreeRoamRoadScope) or from stale route markings left by a previous race, and 158AE0
    // draws those segments as thick yellow route lines (guest 0x158DC0 `or edx, 0x30B0D` outline
    // and 0x158DE2 `or eax, 0x2DC2FF` fill — 0x2DC2FF as BGR is golden yellow RGB(255,194,45)).
    // The walk builds segments ONLY from bit-15 nodes, so there is no white-road segment path to
    // mark for instead. But the regular street grid is the 30 road objects (0x46AB28), drawn by
    // 158AE0's FIRST loop (guest 0x158B33-0x158B65, unconditional) — so suppress the route draw by
    // clearing [esi+0x164]: 158AE0's gate at guest 0x158B67 `test [esi+0x164]; jz 0x158F38`
    // then skips the whole yellow route block and returns after the white grid. Safe because
    // 158520 only WRITES [esi+0x164] (never reads it), the FEng object (0x572181E6) stays shown
    // so 158AE0 still runs, and the destination GPS route (pink) plus the POI icons are drawn by
    // a different function, not 158AE0 — they were visible before the marking fix when 158AE0
    // never ran in free roam. Races are untouched: a real route id means the yellow route is
    // correct there. The chunky look was the thick route lines obscuring the thin white grid;
    // with them gone the thin grid is the display again.
    if (ctx.ecx && mem::IsCommitted(ctx.ecx + 0x164u)) {
        uint32_t cur = game::ReadFeVar(ctx, base, 0xBCDu, 0u);
        if ((cur == 0u || cur == 0x99C27E3Bu) && (game::Peek(ctx.ecx + 0x164u) & 0xFFu) != 0u) {
            kernel::G<uint8_t>(ctx.ecx + 0x164u) = 0;
            static int cleared = 0;
            if (cleared < 4) {
                LOGR("minimap: free roam, cleared [esi+0x164] — yellow route draw suppressed, white grid only");
                cleared++;
            }
        }
    }
}

X86_EXTERN_FUNC(__imp__sub_00158AE0);
X86_FUNC(sub_00158AE0) {
    static int n_158ae0 = 0;
    if ((++n_158ae0 % 600) == 1) LOGR("minimap: sub_00158AE0 entry #%d", n_158ae0);
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_00158AE0(ctx, base);
}

// FeVar getter (sub_0007DD00) — the no-route bypass, done below the FeVar API. sub_00158520's gate
// calls this getter for the 0xBCD race-route var, and it returns FALSE when the FeVar does not exist,
// which is always the case in free roam (diagnostic: `0xBCD FeVar = 00000000`), so the gate exits
// early and no road data is built. The FeVar API cannot create the var (the setter fails the same
// NULL-bucket check), so lend the gate a route id one level lower: when this getter is called from
// 158520's 0xBCD site (return address 0x15859E) and the real getter would find nothing, report the
// dummy route id as found. The dummy is a constant, so the rebuild behaves like a race with a
// constant route id (the capacity check, not a route change, drives updates as the player drives);
// it is written only into 158520's stack local [ebp-0x30], so no other code ever sees it. When a real
// route exists the real getter's answer is used untouched (races are unaffected). The call site is
// identified by its return address, so every other FeVar read in the game passes through unchanged
// (ReadFeVar/QueryFeVar use a scratch stack with 0 at [esp], so they pass through too). The real
// getter is run on a copy of the context to decide found/not-found exactly — a bucket can exist while
// the hash is absent — and only its eax/esp effects are applied back to the real context. The getter
// is `ret 0x14`, so the override pops the return address (4) plus the 5 args (20) = 24. RE 2026-11-23.
X86_EXTERN_FUNC(__imp__sub_0007DD00);
X86_FUNC(sub_0007DD00) {
    // Fast path: only 158520's 0xBCD gate call is ever overridden. The return address is at [esp]
    // (the caller pushed it); anything else passes through immediately.
    uint32_t retaddr = (mem::IsCommitted(ctx.esp) && mem::IsCommitted(ctx.esp + 3)) ? kernel::R32(ctx.esp) : 0;
    if (retaddr != 0x15859Eu || ctx.ecx != 0x456830u) {
        __imp__sub_0007DD00(ctx, base);
        return;
    }
    uint32_t hash = (mem::IsCommitted(ctx.esp + 4) && mem::IsCommitted(ctx.esp + 7)) ? kernel::R32(ctx.esp + 4) : 0;
    if (hash != 0xBCDu) {
        __imp__sub_0007DD00(ctx, base);
        return;
    }
    // Run the real getter on a copy: it decides exactly whether the FeVar exists, without
    // disturbing the real context. Only eax (found) and esp (the ret 0x14 cleanup) are applied back.
    X86Context sc = ctx;
    __imp__sub_0007DD00(sc, base);
    bool found = (sc.eax & 0xFFu) != 0;
    if (!found && game::NoRouteRoadScope::RoadArrayPopulated()) {
        uint32_t out_ptr = (mem::IsCommitted(ctx.esp + 8) && mem::IsCommitted(ctx.esp + 11)) ? kernel::R32(ctx.esp + 8) : 0;
        if (out_ptr && game::IsCommittedRange(out_ptr, 4)) {
            kernel::W32(out_ptr, game::NoRouteRoadScope::kDummyRoute);
            static int logged = 0;
            if (logged < 8) {
                LOGR("minimap: getter hook: 0xBCD absent, reporting dummy route id");
                logged++;
            }
            ctx.eax = 1;
            ctx.esp += 24;  // ret 0x14: pop retaddr (4) + 5 args (20)
            return;
        }
    }
    ctx.eax = sc.eax;
    ctx.esp += 24;
}

// Route-entry accessors (sub_0019C670/19C580): 158520 calls them on the entry sub_001849F0 finds for
// the route id, to fill the route-highlight bounds ([esi+0x170]/[esi+0x16C]/[esi+0x174]/[esi+0x178]).
// With the dummy route id 1849F0 finds no entry and returns NULL, so these would run with a NULL
// `this` — their [esi+0x0C]/[esi+0x10] reads are absorbed by the null page, but the callees they
// invoke on the NULL object (180720/180770) are not verified safe. Return early instead and zero the
// 16-byte out struct, so the minimap gets a degenerate (empty) route highlight rather than garbage.
// Inert for every normal caller: `this` is never NULL outside the dummy-route path. Both are
// `ret 0x04` (pop retaddr + 1 arg = 8). RE 2026-11-23.
X86_EXTERN_FUNC(__imp__sub_0019C670);
X86_FUNC(sub_0019C670) {
    if (!ctx.ecx) {
        uint32_t out_ptr = (mem::IsCommitted(ctx.esp + 4) && mem::IsCommitted(ctx.esp + 7)) ? kernel::R32(ctx.esp + 4) : 0;
        if (out_ptr && game::IsCommittedRange(out_ptr, 16)) {
            for (int i = 0; i < 4; i++) kernel::W32(out_ptr + uint32_t(i) * 4u, 0);
        }
        static int logged = 0;
        if (logged < 4) {
            LOGR("minimap: 19C670 null route entry, skipping highlight bounds");
            logged++;
        }
        ctx.esp += 8;
        return;
    }
    __imp__sub_0019C670(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0019C580);
X86_FUNC(sub_0019C580) {
    if (!ctx.ecx) {
        uint32_t out_ptr = (mem::IsCommitted(ctx.esp + 4) && mem::IsCommitted(ctx.esp + 7)) ? kernel::R32(ctx.esp + 4) : 0;
        if (out_ptr && game::IsCommittedRange(out_ptr, 16)) {
            for (int i = 0; i < 4; i++) kernel::W32(out_ptr + uint32_t(i) * 4u, 0);
        }
        static int logged = 0;
        if (logged < 4) {
            LOGR("minimap: 19C580 null route entry, skipping highlight bounds");
            logged++;
        }
        ctx.esp += 8;
        return;
    }
    __imp__sub_0019C580(ctx, base);
}

// The same stale-origin defect in the remaining Hud_Minimap readers of +0xB4/+0xB8: the target ring
// (TRACKMAPTARGETRING) and the other map elements they position stay at the unmoved map's origin when the
// minimap is dragged, because only 158520/158AE0 above were shifted. Both take the minimap in ecx
// (esi is copied from ecx at entry, and the origin is read via esi). NOTE: sub_0015870A is NOT a
// function start — it is 2-byte NOP padding inside sub_00158520, so it has no direct callers and its
// hook never fires; the shift for that path happens via the 158520 wrapper. The dead override was
// removed (pointer audit 2026-11-23).
X86_EXTERN_FUNC(__imp__sub_001581D0);
X86_FUNC(sub_001581D0) {
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_001581D0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00165C00);
X86_FUNC(sub_00165C00) {
    game::g_minimap_obj = ctx.ecx;  // cache for ShiftRoadRects' minimap+0x30 rect object
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_00165C00(ctx, base);
}

