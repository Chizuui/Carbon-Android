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

uint32_t g_minimap_obj = 0;  // Hud_Minimap game object, cached from 158520/165C00 hooks (ctx.ecx)

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

struct NoRouteRoadScope {
    X86Context ctx;
    uint8_t* base = nullptr;
    bool active = false;
    static constexpr uint32_t kRouteVar = 0xBCD;
    static constexpr uint32_t kNoRouteSentinel = 0x99C27E3B;
    static constexpr uint32_t kDummyRoute = 0x524F4144;  // "ROAD", not a real route hash

    explicit NoRouteRoadScope(const X86Context& c, uint8_t* b) : ctx(c), base(b) {
        uint32_t cur = ReadFeVar(ctx, base, kRouteVar, 0u);
        if (cur != kNoRouteSentinel && cur != 0u) return;  // real route: leave alone
        if (!RoadArrayPopulated()) return;                 // no road network loaded: nothing to draw
        WriteFeVar(ctx, base, kRouteVar, kDummyRoute);
        active = true;
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

// Free roam has no race route, so no road-graph node carries bit15 (node+0x0A).
// Temporarily lend bit15 to nodes near the player for sub_00158520, then restore original flags.
struct FreeRoamRoadScope {
    struct Saved {
        uint32_t node = 0;
        uint16_t flags = 0;
    };
    std::vector<Saved> saved;

    explicit FreeRoamRoadScope(const X86Context& ctx, uint8_t* base, uint32_t minimap) {
        if (!minimap) return;
        uint32_t cur = ReadFeVar(ctx, base, 0xBCDu, 0u);
        if (cur != 0u && cur != 0x99C27E3Bu) return;
        if (!NoRouteRoadScope::RoadArrayPopulated()) return;
        if (!mem::IsCommitted(minimap + 0xA0u) || !mem::IsCommitted(minimap + 0xA3u) ||
            !mem::IsCommitted(minimap + 0xACu) || !mem::IsCommitted(minimap + 0xAFu) ||
            !mem::IsCommitted(minimap + 0xB0u) || !mem::IsCommitted(minimap + 0xB3u)) return;
        float norm_u = Float(minimap + 0xACu);  // (player z + 7300) / 16000
        float norm_v = Float(minimap + 0xB0u);  // 1 - ((-player x) + 2000) / 16000
        if (!(norm_u >= 0.0f && norm_u <= 1.0f && norm_v >= 0.0f && norm_v <= 1.0f)) return;
        if (norm_u == 0.0f && norm_v == 0.0f) return;
        float a0 = Float(minimap + 0xA0u);
        if (!(a0 > 0.0f) || !std::isfinite(a0)) return;
        float u_p = norm_u * 16000.0f - 7300.0f;
        float v_p = (1.0f - norm_v) * 16000.0f - 2000.0f;
        float r = 64000.0f / a0;
        if (r < 300.0f) r = 300.0f;
        if (r > 4000.0f) r = 4000.0f;
        float r2 = r * r;

        uint32_t count = Peek(0x4B612Cu);
        uint32_t nodes = Peek(0x4B6174u);
        uint32_t pos = Peek(0x4B6170u);
        if (!count || !nodes || !pos) return;
        if (count > 20000u) return;

        constexpr int kMaxMark = 512;
        int marked = 0;
        for (uint32_t i = 0; i < count && marked < kMaxMark; i++) {
            uint32_t node = nodes + i * 0x16u;
            if (!mem::IsCommitted(node + 0x0Au) || !mem::IsCommitted(node + 0x0Bu)) continue;
            uint16_t flags = kernel::R16(node + 0x0Au);
            if (flags & 0x8000u) continue;
            if (flags & 0x0800u) continue;
            if (!mem::IsCommitted(node) || !mem::IsCommitted(node + 3)) continue;
            uint16_t e1 = kernel::R16(node);
            uint16_t e2 = kernel::R16(node + 2u);
            uint32_t p1 = pos + uint32_t(e1) * 32u;
            uint32_t p2 = pos + uint32_t(e2) * 32u;
            if (!mem::IsCommitted(p1) || !mem::IsCommitted(p1 + 3) ||
                !mem::IsCommitted(p1 + 8) || !mem::IsCommitted(p1 + 11) ||
                !mem::IsCommitted(p2) || !mem::IsCommitted(p2 + 3) ||
                !mem::IsCommitted(p2 + 8) || !mem::IsCommitted(p2 + 11)) continue;
            float um = (Float(p1 + 8u) + Float(p2 + 8u)) * 0.5f;
            float vm = (-Float(p1) + -Float(p2)) * 0.5f;
            float du = um - u_p, dv = vm - v_p;
            if (du * du + dv * dv > r2) continue;
            saved.push_back({node, flags});
            kernel::W16(node + 0x0Au, static_cast<uint16_t>(flags | 0x8000u));
            marked++;
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
X86_EXTERN_FUNC(__imp__sub_00158520);
X86_FUNC(sub_00158520) {
    game::g_minimap_obj = ctx.ecx;
    game::MinimapOriginScope scope(ctx.ecx);
    game::NoRouteRoadScope route(ctx, base);
    game::FreeRoamRoadScope mark(ctx, base, ctx.ecx);
    __imp__sub_00158520(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00158AE0);
X86_FUNC(sub_00158AE0) {
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_00158AE0(ctx, base);
}

// FeVar getter (sub_0007DD00) — report dummy route id when 158520's 0xBCD gate finds no FeVar in free roam.
X86_EXTERN_FUNC(__imp__sub_0007DD00);
X86_FUNC(sub_0007DD00) {
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
    X86Context sc = ctx;
    __imp__sub_0007DD00(sc, base);
    bool found = (sc.eax & 0xFFu) != 0;
    if (!found && game::NoRouteRoadScope::RoadArrayPopulated()) {
        uint32_t out_ptr = (mem::IsCommitted(ctx.esp + 8) && mem::IsCommitted(ctx.esp + 11)) ? kernel::R32(ctx.esp + 8) : 0;
        if (out_ptr && game::IsCommittedRange(out_ptr, 4)) {
            kernel::W32(out_ptr, game::NoRouteRoadScope::kDummyRoute);
            ctx.eax = 1;
            ctx.esp += 24;
            return;
        }
    }
    ctx.eax = sc.eax;
    ctx.esp += 24;
}

// Route-entry accessors (sub_0019C670/19C580): guard against NULL route entry from dummy route id.
X86_EXTERN_FUNC(__imp__sub_0019C670);
X86_FUNC(sub_0019C670) {
    if (!ctx.ecx) {
        uint32_t out_ptr = (mem::IsCommitted(ctx.esp + 4) && mem::IsCommitted(ctx.esp + 7)) ? kernel::R32(ctx.esp + 4) : 0;
        if (out_ptr && game::IsCommittedRange(out_ptr, 16)) {
            for (int i = 0; i < 4; i++) kernel::W32(out_ptr + uint32_t(i) * 4u, 0);
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
        ctx.esp += 8;
        return;
    }
    __imp__sub_0019C580(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001581D0);
X86_FUNC(sub_001581D0) {
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_001581D0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00165C00);
X86_FUNC(sub_00165C00) {
    game::g_minimap_obj = ctx.ecx;
    game::MinimapOriginScope scope(ctx.ecx);
    __imp__sub_00165C00(ctx, base);
}
