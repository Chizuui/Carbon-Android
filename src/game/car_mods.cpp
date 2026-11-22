// Add-on cars from tools/car_mod.py. Its cars.txt, in the mods folder, has a line for each: the pvehicle's key, the
// frontend's key, the hash of its CARNAME string, its name texture's hash, width, height and NV2A format, and the name.
// The career's car list (career+0x234, 200 entries of 20 bytes, filled by sub_000A38D0 when the profile is new) is
// saved with the profile: a car newer than the profile is added here, as sub_000A1360 adds a bought car, and is saved
// with it from then on. The car select's name is a texture no front end pack has: GetTextureInfo answers its hash with
// a copy of a TextureInfo reading its name_<hash>.dxt (the PC tool's one car: car_name.dxt). A car taken out again (the
// app's ModInstaller lists it in cars_removed.txt) stays in the saves that kept it, its key naming nothing now: it is
// taken out of the list here, with the customization object and career record of its slot.
//
// Carbon port of upstream src/game/car_mods.cpp (Most Wanted). Every MW address in it needed a Carbon equivalent;
// what was found is listed below with its evidence, what was not is marked OPEN and left out (never guessed).
//
// VERIFIED Carbon addresses (all read in generated/carbon/ unless noted):
// - Career car list: career+0x234, 0xC8 slots x 0x14. Slot [+0]=handle, [+4]=frontend key, [+8]=vehicle key,
//   [+0xC]=flags, [+0x10]=0x470-object index, [+0x11]=0x34-record index.
// - sub_0007B720: pure car-db lookup (thiscall ecx=car_db, handle) -> slot or 0. 7B7D0: index byte -> 0x470 object
//   (car_db+0xFA0+index*0x470, 76 entries). 7B800: index byte -> 0x34 record (car_db+0x15C70+index*0x34, 11 entries).
// - sub_0009CE70: deleter (thiscall ecx=car_db, handle, mask, flag); sub_000A1D90 calls it with mask 0x80, flag 0.
//   Bit 0x80 at slot+0x0C is NOT proven to mean "owned" (other callers use masks 0x02/0x04), so removal here does
//   not use a mask: it frees the slot's object/record the way 9CE70's tail does.
// - sub_000918D0: free-slot finder (thiscall ecx=car_db) -> slot; it runs 88530 on the slot (which, for an empty
//   slot, only clears +0x10/+0x11 and +0x0C) and writes [slot]=index.
// - sub_00088530: SetUpEntry equivalent (thiscall ecx=slot); derives type/class from the keys when [slot] != -1.
//   The add path below does NOT call it after writing the keys, because sub_000A1360 (add preset car by name,
//   the game's own bought-car path) does not either: it writes the keys, allocates the object, and ends with
//   flags 0xF0010. The add here mirrors A1360's effects exactly, without needing the preset name.
// - sub_0009CB70: allocate a free 0x470 object (thiscall ecx=car_db) -> object or 0; its +0xB8 is the index.
// - sub_00095180: reset a 0x470 object (thiscall ecx=object). sub_00088620: register a slot's type
//   (thiscall ecx=slot; 1AB80 on [slot+8], then the 0x4B4A50 table); its return is ignored, as A1360 ignores it.
// - sub_00087EF0: counts slots with handle != -1, [slot+0x10] != 0xFF, (flags & 0xF0000) and (flags & 0x138);
//   A1360 refuses when it returns >= 45 (0x2D). The add path enforces the same cap host-side (CountAddedCars).
// - Known-type gate: the linked list at 0x46D920 (value +0, next +8), the same walk carbon.cpp's IsKnownCarType
//   does. An add-on whose vehicle key is not in it has no type record for 1AB80/88620, so it is skipped.
// - GetTextureInfo: sub_00102310, cdecl (hash, required, ?). Its TextureInfo has the hash at +0x0C (the fast
//   path compares [entry+0x0C]) and the XDK D3DTexture header at [info]+0x14: the flare (sub_0010EC90) passes
//   [info]+0x14 to its draw helper, and sub_00361550 reads the format byte at [arg+0x0D] and Data at [arg+4].
//   Template: WHITE16X16_NOALPHA's TextureInfo, stored by the boot init (sub_00103220) at [0x45D31C].
//   The host's Describe() (sdk/runtime/gpu/texture.cpp) reads the dimensions from the header's Format log2 bits
//   when Size is 0, so MW's Size=0 is kept and no wrapper size field is written (see ModCarTexture).
// - FeVars: get sub_0007DD00 / set sub_0007DD50, ecx=0x456830; 0x1B58 is the current car (index 0 here; the
//   242FD0 guard in carbon.cpp repairs the other index afterwards). GameFlowState 0x46D280, 3 = career/frontend.
// - Career root: kCareerRoot 0x46A7CC; sys+0xD4 career list, sys+0xD8 count (carbon.cpp).
//
// OPEN (not in this file; see the report for the next RE step of each):
// - Race modes: MW kept the race settings' car handles at FEDatabase+0x1C (11 x 0x18, handle +0x10). Carbon's
//   equivalent table (cFrontendDatabase is [0x46A8E4]) is unverified, so the race-mode fixup is omitted.
// - TypeOfName and the type hooks (MW 244FE0/245030/263F10/263FD0/1C9AB0): Carbon's car-type table is the flat
//   0xD0-stride table at [0x4B3160] (sub_00283B30: entry = [0x4B3160]+index*0xD0), but the name-string and type-id
//   offsets inside an entry and the entry count are unverified, so no hash->type lookup is implemented.
// - AI stand-in hook (MW 172730) and ride-builder hook (MW 140BE0): the Carbon analogs are unfound.
//   StockStandIn and ListedHandle ARE implemented here, ready to be hooked once the addresses are found.
// - Pool growth (MW 54340, main pool 0x421BB8, car pools 0x433A00/0x433A04): the Carbon pool size constants were
//   not found in the recompile (0x75F600/0x627000 absent), so no pool hook is registered.
// - 957A0 (the preset-parts copy 9CF10 does when [slot+0x10] <= 0x4B): the add path skips it, as there is no
//   preset record for a numeric-keys add-on; the customization object stays default-initialized (95180).
// - The TextureInfo wrapper's size field (MW wrote width|height<<16 at info+0x40): Carbon's offset is
//   unverified (its hash moved from +0x24 to +0x0C), so it is not written; see ModCarTexture.
//
// NOTE on building: CMakeLists.txt globs only src/${XBE_GAME_DIR}/*.cpp, so for Carbon (XBE_GAME_DIR=carbon)
// this file in src/game/ is NOT compiled. It builds only if the glob gains it or the file moves to src/carbon/
// (a parent decision; this file does not touch CMakeLists.txt).
// NOTE on strings: Carbon has no working SetOwnString (src/game/prompts.cpp is dead MW code). The equivalent is
// carbon.cpp's OwnString/GuestText: car names are registered here (ModOwnString) and carbon.cpp's OwnString
// consults it, so the existing string hooks (sub_0011F9A0/1FAB0) serve them.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(sub_0007DD00);
X86_EXTERN_FUNC(sub_0007DD50);

namespace game {

// carbon.cpp's OwnString/GuestText (non-static, namespace game): GuestText is used for the template texture's
// hash fallback in ModCarTexture; OwnString consults ModOwnString (below) for add-on car names.
uint32_t GuestText(const char* text);

namespace {

constexpr uint32_t kCareerRoot = 0x46A7CC;  // sys pointer; sys+0xD4 career list, sys+0xD8 count
constexpr uint32_t kGameFlowState = 0x46D280, kGameFlowCareer = 3;
constexpr uint32_t kCarDb = 0x234, kCarEntry = 0x14, kCarEntries = 0xC8;
constexpr uint32_t kSlotFrontend = 4, kSlotVehicle = 8, kSlotFlags = 0x0C, kSlotCustom = 0x10, kSlotCareer = 0x11;
constexpr uint32_t kEmpty = 0xFFFFFFFF, kNoSlot = 0xFF;
constexpr uint32_t kKnownTypes = 0x46D920;  // known-type linked list: value +0, next +8
constexpr uint32_t kFeVarBase = 0x456830, kFeVarCurrentCar = 0x1B58;
// Guest functions (thiscall on ecx unless noted). All verified in generated/carbon/.
constexpr uint32_t kFreeSlot = 0x918D0;      // car_db -> free slot (runs 88530, writes [slot]=index)
constexpr uint32_t kAllocCustom = 0x9CB70;   // car_db -> free 0x470 object (or 0); its +0xB8 is the index
constexpr uint32_t kResetCustom = 0x95180;   // 0x470 object -> reset
constexpr uint32_t kObjByIndex = 0x7B7D0;    // car_db, index byte -> 0x470 object
constexpr uint32_t kRecByIndex = 0x7B800;    // car_db, index byte -> 0x34 record
constexpr uint32_t kRegisterType = 0x88620;  // slot -> register its type (1AB80 + 0x4B4A50 table)
constexpr uint32_t kGetTextureInfo = 0x102310;  // cdecl (hash, required, ?) -> TextureInfo* or 0
constexpr uint32_t kHashString = 0x5E720;    // cdecl (char*) -> hash
constexpr uint32_t kAddFlags = 0xF0010;      // flags sub_000A1360 writes for an added car
constexpr uint32_t kAddCap = 45;             // sub_000A1360 refuses when sub_00087EF0 >= 45
constexpr uint32_t kWhiteTemplate = 0x45D31C;  // [global] = TextureInfo* of WHITE16X16_NOALPHA (boot init)

struct Car {
    uint32_t vehicle, frontend, name_texture, width, height, format;
};

// the add-on car names, by CARNAME hash: Cars() registers them, ModOwnString serves them
std::map<uint32_t, std::string>& ModNames() {
    static std::map<uint32_t, std::string> names;
    return names;
}

bool Committed(uint32_t va, uint32_t size) {
    if (va < 0x10000 || !size) return false;
    uint64_t end = uint64_t(va) + size - 1;
    if (end > 0xFFFFFFFFull) return false;
    for (uint64_t p = va & ~0xFFFull; p <= (end & ~0xFFFull); p += 0x1000) {
        if (!mem::IsCommitted(uint32_t(p))) return false;
    }
    return true;
}

// the career's car list, host-side (no guest call): 0 when no career is loaded
uint32_t CareerCarDb() {
    if (!mem::IsCommitted(kCareerRoot)) return 0;
    uint32_t sys = kernel::R32(kCareerRoot);
    if (!sys || !Committed(sys + 0xD4u, 8)) return 0;
    if (kernel::R32(sys + 0xD8u) == 0) return 0;
    uint32_t list = kernel::R32(sys + 0xD4u);
    if (!list || !Committed(list, 4)) return 0;
    uint32_t career = kernel::R32(list);
    if (career < 0x10000u || !Committed(career + kCarDb, kCarEntries * kCarEntry)) return 0;
    return career + kCarDb;
}

// the known-type list at 0x46D920 (the same walk carbon.cpp's IsKnownCarType does)
bool IsKnownCarType(uint32_t car_type) {
    if (!car_type || !Committed(kKnownTypes, 4)) return false;
    uint32_t n = kernel::R32(kKnownTypes);
    for (int i = 0; i < 512 && Committed(n, 12); i++) {
        if (kernel::R32(n) == car_type) return true;
        n = kernel::R32(n + 8u);
    }
    return false;
}

// sub_00087EF0's count, host-side: slots with a handle, a customization object, class bits (0xF0000)
// and the 0x138 bits. A1360-added cars (flags 0xF0010, object allocated) count, so added cars do too.
uint32_t CountAddedCars(uint32_t car_db) {
    if (!Committed(car_db, kCarEntries * kCarEntry)) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < kCarEntries; i++) {
        uint32_t slot = car_db + i * kCarEntry;
        if (kernel::R32(slot) == kEmpty) continue;
        if (*static_cast<uint8_t*>(kernel::HostPtr(slot + kSlotCustom)) == kNoSlot) continue;
        uint32_t flags = kernel::R32(slot + kSlotFlags);
        if ((flags & 0xF0000u) && (flags & 0x138u)) n++;
    }
    return n;
}

// carbon.cpp's QueryFeVar/SetFeVar (static there, duplicated here): FeVar get/set, ecx=0x456830.
// The scratch context keeps the hook's ctx untouched.
uint32_t QueryFeVar(const X86Context& base_ctx, uint8_t* base, uint32_t id, uint32_t idx, uint32_t def_val) {
    X86Context sc = base_ctx;
    uint32_t out_va = sc.esp - 16u;
    if (!Committed(out_va - 0x400u, 0x410u)) return def_val;
    kernel::W32(out_va, def_val);
    sc.esp = out_va;
    sc.ecx = kFeVarBase;
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

void SetFeVar(const X86Context& base_ctx, uint8_t* base, uint32_t id, uint32_t val, uint32_t idx) {
    X86Context sc = base_ctx;
    sc.esp -= 16u;
    if (!Committed(sc.esp - 0x400u, 0x400u)) return;
    sc.ecx = kFeVarBase;
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

const std::vector<Car>& Cars() {
    static const std::vector<Car> cars = [] {
        std::vector<Car> out;
        std::vector<uint8_t> file;
        if (!kernel::ReadGameFile("D:\\cars.txt", file)) return out;
        std::string text(file.begin(), file.end());
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            uint32_t vehicle, frontend, hash, texture, width, height, format;
            int name = 0;
            if (std::sscanf(line.c_str(), "%x %x %x %x %u %u %u %n", &vehicle, &frontend, &hash, &texture, &width,
                             &height, &format, &name) < 7 ||
                !name)
                continue;
            out.push_back({vehicle, frontend, texture, width, height, format});
            ModNames()[hash] = line.substr(name);
        }
        return out;
    }();
    return cars;
}

const std::vector<uint32_t>& RemovedCars() {
    static const std::vector<uint32_t> keys = [] {
        std::vector<uint32_t> out;
        std::vector<uint8_t> file;
        if (!kernel::ReadGameFile("D:\\cars_removed.txt", file)) return out;
        std::string text(file.begin(), file.end());
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            uint32_t key;
            if (std::sscanf(text.substr(at, end - at).c_str(), "%x", &key) == 1) out.push_back(key);
            at = end + 1;
        }
        return out;
    }();
    return keys;
}

}  // namespace

// the add-on car types, from car_types.txt: the total count (stock + add-ons). Carbon's stock-type count is
// unverified (the table is [0x4B3160], 0xD0 per entry, count unknown), so there is no stock fallback here and
// the function is currently unused: it is reserved for the type hooks (see the OPEN notes at the top).
uint32_t CarTypes() {
    static const uint32_t types = [] {
        std::vector<uint8_t> file;
        uint32_t n = 0;
        if (kernel::ReadGameFile("D:\\car_types.txt", file)) {
            file.push_back(0);
            std::sscanf(reinterpret_cast<const char*>(file.data()), "%u", &n);
        }
        return n;
    }();
    return types;
}

// an add-on car's name, by its CARNAME hash: carbon.cpp's OwnString consults this (Carbon has no SetOwnString).
// Calls Cars() first so the registry is populated whenever a name is asked for.
const char* ModOwnString(uint32_t hash) {
    (void)Cars();
    auto it = ModNames().find(hash);
    return it == ModNames().end() ? nullptr : it->second.c_str();
}

// GetTextureInfo's question, on the game thread: our TextureInfo for the name texture's hash, else 0
uint32_t ModCarTexture(X86Context& ctx, uint32_t hash) {
    static std::map<uint32_t, uint32_t> made;
    for (const Car& car : Cars()) {
        if (!car.name_texture || car.name_texture != hash) continue;
        if (uint32_t info = made[hash]) return info;
        // Carbon's TextureInfo: [0] is the platform struct, +0x0C is the hash (102310's fast path compares
        // [entry+0x0C]); the XDK D3DTexture header is at [info]+0x14 (see the top notes). The 0x80/0x40 sizes
        // are MW's: the fields this code touches are all verified Carbon offsets, and the memcpy only copies
        // the template, so the sizes only bound the allocation.
        constexpr uint32_t kInfoBytes = 0x80, kPlatBytes = 0x40, kD3D = 0x14, kInfoHash = 0x0C;
        std::vector<uint8_t> pixels;
        char file[32];
        std::snprintf(file, sizeof file, "D:\\name_%08x.dxt", hash);
        if (!kernel::ReadGameFile(file, pixels) && !kernel::ReadGameFile("D:\\car_name.dxt", pixels)) return 0;
        uint32_t tmpl = 0;
        if (mem::IsCommitted(kWhiteTemplate)) tmpl = kernel::R32(kWhiteTemplate);
        if (!tmpl) {
            // template not loaded yet (the boot init at 103220 stores it): ask GetTextureInfo by name.
            uint32_t name = GuestText("WHITE16X16_NOALPHA");
            uint32_t white = name ? kernel::CallGuest(ctx, kHashString, {name}) : 0;
            if (white) tmpl = kernel::CallGuest(ctx, kGetTextureInfo, {white, 0, 0});
        }
        if (!tmpl || !mem::IsCommitted(tmpl)) return 0;
        uint32_t tmpl_plat = kernel::R32(tmpl);
        if (!tmpl_plat || !mem::IsCommitted(tmpl_plat)) return 0;
        uint32_t info = mem::AllocSystem(kInfoBytes), plat = mem::AllocSystem(kPlatBytes);
        if (!info || !plat) return 0;
        std::memcpy(kernel::HostPtr(info), kernel::HostPtr(tmpl), kInfoBytes);
        std::memcpy(kernel::HostPtr(plat), kernel::HostPtr(tmpl_plat), kPlatBytes);
        kernel::W32(info, plat);
        kernel::W32(info + kInfoHash, hash);
        // MW also wrote the wrapper's size at info+0x40 (width|height<<16). Carbon's wrapper size-field
        // offset is unverified (its hash moved from +0x24 to +0x0C), so it is NOT written here. The host's
        // Describe() reads the dimensions from the header's Format log2 bits when Size is 0 (set below),
        // so the texture uploads at the right size; if the frontend is found to read a wrapper size field
        // for layout, that offset still needs RE (find the car-select name draw and see which TextureInfo
        // offset it reads for the width/height).
        uint32_t data = mem::AllocContiguous(uint32_t(pixels.size()), 0, 0xFFFFFFFF, 0x1000);
        if (!data) return 0;
        std::memcpy(kernel::HostPtr(data), pixels.data(), pixels.size());
        auto log2 = [](uint32_t v) {
            uint32_t n = 0;
            while ((1u << n) < v) n++;
            return n;
        };
        uint32_t d3d = plat + kD3D;
        kernel::W32(d3d + 4, mem::PhysicalFromVirtual(data));
        kernel::W32(d3d + 8, 0);
        kernel::W32(d3d + 12, (kernel::R32(d3d + 12) & 0xFF) | car.format << 8 | 1u << 16 | log2(car.width) << 20 |
                                  log2(car.height) << 24);
        kernel::W32(d3d + 16, 0);
        made[hash] = info;
        return info;
    }
    return 0;
}

// A handle the list (the career's car_db) has, for one it has not: the career's current car (FeVar 0x1B58,
// passed in), else the first listed
uint32_t ListedHandle(uint32_t car_db, uint32_t handle, uint32_t current) {
    if (!Committed(car_db, kCarEntries * kCarEntry)) return handle;
    uint32_t first = kEmpty;
    for (uint32_t i = 0; i < kCarEntries; i++) {
        uint32_t h = kernel::R32(car_db + i * kCarEntry);
        if (h == handle) return handle;
        if (first == kEmpty && h != kEmpty) first = h;
    }
    uint32_t pick = first;
    if (current != kEmpty) {
        for (uint32_t i = 0; i < kCarEntries; i++)
            if (kernel::R32(car_db + i * kCarEntry) == current) {
                pick = current;
                break;
            }
    }
    if (pick == kEmpty) return handle;
    LOGR("mods: car %u is not in the career's list (an add-on taken out); %u instead", handle, pick);
    return pick;
}

// A stock car for an AI racer that would drive an add-on: their lower LODs, which an opponent is mostly seen in,
// are the converter's thinning of a PC model and often torn, so opponents keep to the game's own cars. Taken from
// the career's stock list, an add-on's key or a removed one's never; a different one for each add-on asked in a row.
// Implemented, but its hook (the Carbon analog of MW's AI vehicle creation, sub_00172730) is not yet found.
uint32_t StockStandIn(uint32_t vehicle) {
    auto addon = [](uint32_t key) {
        for (const Car& car : Cars())
            if (car.vehicle == key) return true;
        for (uint32_t removed : RemovedCars())
            if (removed == key) return true;
        return false;
    };
    if (!vehicle || !addon(vehicle)) return 0;
    uint32_t car_db = CareerCarDb();
    if (!car_db) return 0;
    std::vector<uint32_t> stock;
    for (uint32_t i = 0; i < kCarEntries; i++) {
        uint32_t entry = car_db + i * kCarEntry, key = kernel::R32(entry + kSlotVehicle);
        if (kernel::R32(entry) == kEmpty || !key || addon(key)) continue;
        if (std::find(stock.begin(), stock.end(), key) == stock.end()) stock.push_back(key);
    }
    if (stock.empty()) return 0;
    static uint32_t turn = 0;
    uint32_t pick = stock[(vehicle + turn++ * 7) % stock.size()];
    LOGR("mods: an AI racer's add-on %08X drives the stock %08X instead", vehicle, pick);
    return pick;
}

// game thread, at each present (called from carbon.cpp's sub_00242FD0 hook, before its 0x1B58 guard)
void ModCarsFrame(X86Context& ctx, uint8_t* base) {
    const std::vector<Car>& cars = Cars();
    const std::vector<uint32_t>& removed = RemovedCars();
    if (cars.empty() && removed.empty()) return;
    // The game itself only resolves career cars when GameFlowState==3 (sub_0027D970: cmp [0x46D280],3).
    if (!mem::IsCommitted(kGameFlowState) || kernel::R32(kGameFlowState) != kGameFlowCareer) return;
    uint32_t car_db = CareerCarDb();
    if (!car_db) return;
    // filled: sub_000A38D0 empties every handle, then adds the default car first and gives it the handle
    // 0x12345678, then the stock cars. Older code took a first handle of 0 for filled and added no car to
    // such a profile
    if (kernel::R32(car_db) == kEmpty || !kernel::R32(car_db + kSlotVehicle)) return;
    for (uint32_t key : removed) {
        for (uint32_t i = 0; i < kCarEntries; i++) {
            uint32_t slot = car_db + i * kCarEntry;
            if (kernel::R32(slot) == kEmpty || kernel::R32(slot + kSlotVehicle) != key) continue;
            // Free the customization object and the career record the way sub_0009CE70's tail does, without
            // its mask: the 0x80 bit is not proven to mean "owned" and A1360-added cars do not have it set,
            // so a mask-based delete would miss them. 87DE0 (flag=1) is skipped, as A1D90 does (flag=0).
            uint8_t custom = *static_cast<uint8_t*>(kernel::HostPtr(slot + kSlotCustom));
            uint8_t career_idx = *static_cast<uint8_t*>(kernel::HostPtr(slot + kSlotCareer));
            if (custom != kNoSlot) {
                X86Context sc = ctx;
                sc.ecx = car_db;
                uint32_t obj = kernel::CallGuest(sc, kObjByIndex, {custom});
                if (obj) {
                    sc.ecx = obj;
                    kernel::CallGuest(sc, kResetCustom, {});
                    *static_cast<uint8_t*>(kernel::HostPtr(obj + 0xB8)) = kNoSlot;
                }
            }
            if (career_idx != kNoSlot) {
                X86Context sc = ctx;
                sc.ecx = car_db;
                uint32_t rec = kernel::CallGuest(sc, kRecByIndex, {career_idx});
                if (rec) *static_cast<uint8_t*>(kernel::HostPtr(rec)) = kNoSlot;
            }
            kernel::W32(slot, kEmpty);
            LOGR("mods: removed %08X from the career's cars", key);
        }
    }
    // the career's car (FeVar 0x1B58, index 0) gone with the add-on: another of the career's cars instead,
    // so the career goes on with what it has. Index 1 is left to carbon.cpp's 242FD0 guard, which runs
    // right after this in the same hook.
    uint32_t current = QueryFeVar(ctx, base, kFeVarCurrentCar, 0, kEmpty);
    bool found = false;
    uint32_t career_car = kEmpty;
    for (uint32_t i = 0; i < kCarEntries; i++) {
        uint32_t slot = car_db + i * kCarEntry, handle = kernel::R32(slot);
        if (handle == kEmpty) continue;
        if (handle == current) found = true;
        if (career_car == kEmpty &&
            *static_cast<uint8_t*>(kernel::HostPtr(slot + kSlotCareer)) != kNoSlot)
            career_car = handle;
    }
    // no other career car: the list's first handle instead
    if (!found && current != kEmpty) {
        uint32_t pick = career_car != kEmpty ? career_car : ListedHandle(car_db, current, current);
        if (pick != current && pick != kEmpty) {
            LOGR("mods: the career's car %u is gone with its add-on; the career goes on in %u", current, pick);
            SetFeVar(ctx, base, kFeVarCurrentCar, pick, 0);
        }
    }
    // NOTE: MW also fixed the race modes' settings here (their handles at FEDatabase+0x1C, 11 x 0x18,
    // handle +0x10). Carbon's race-mode table is unverified, so that fixup is omitted (see the top notes).
    static uint32_t frames = 0;
    if (++frames % 30) return;
    for (const Car& car : cars) {
        uint32_t free_slot = kEmpty;
        bool listed = false;
        for (uint32_t i = 0; i < kCarEntries && !listed; i++) {
            uint32_t slot = car_db + i * kCarEntry;
            if (kernel::R32(slot) == kEmpty) {
                if (free_slot == kEmpty) free_slot = i;
            } else if (kernel::R32(slot + kSlotVehicle) == car.vehicle) {
                listed = true;
            }
        }
        if (listed || free_slot == kEmpty) continue;
        // No type record, no add: 88620's 1AB80 must resolve the vehicle key, and the known-type list is
        // the only proven record of which keys resolve. (The converter must register the type record via
        // the data files + mods override for this gate to pass.)
        if (!IsKnownCarType(car.vehicle)) {
            LOGR("mods: %08X is not a known car type (no record for it); skipping", car.vehicle);
            continue;
        }
        // sub_000A1360 refuses when sub_00087EF0 >= 45; the same cap, counted host-side.
        if (CountAddedCars(car_db) >= kAddCap) {
            LOGR("mods: the career already has %u added cars; %08X skipped", kAddCap, car.vehicle);
            continue;
        }
        // 918D0 finds the first free slot, runs 88530 on it (which, for an empty slot, only clears
        // +0x10/+0x11 and +0x0C) and writes [slot]=index. Use the slot it returns, not free_slot.
        X86Context sc = ctx;
        sc.ecx = car_db;
        uint32_t slot = kernel::CallGuest(sc, kFreeSlot, {});
        if (!slot) continue;
        kernel::W32(slot + kSlotFrontend, car.frontend);
        kernel::W32(slot + kSlotVehicle, car.vehicle);
        // 9CB70 allocates a free 0x470 customization object and resets it (95180); its +0xB8 is the index.
        sc = ctx;
        sc.ecx = car_db;
        uint32_t obj = kernel::CallGuest(sc, kAllocCustom, {});
        if (!obj) {
            kernel::W32(slot, kEmpty);
            continue;
        }
        *static_cast<uint8_t*>(kernel::HostPtr(slot + kSlotCustom)) =
            *static_cast<uint8_t*>(kernel::HostPtr(obj + 0xB8));
        kernel::W32(slot + kSlotFlags, kAddFlags);
        // 88620 registers the type (1AB80 + the 0x4B4A50 table); its return is ignored, as A1360 ignores it.
        sc = ctx;
        sc.ecx = slot;
        kernel::CallGuest(sc, kRegisterType, {});
        LOGR("mods: %08X added to the career's cars (slot %u)", car.vehicle, (slot - car_db) / kCarEntry);
    }
}

}  // namespace game

// Hooks not yet ported (their Carbon addresses are unverified; see the top notes and the report).
// When an address is found, its hook goes here, following the pattern of the sub_00102310 hook in carbon.cpp:
// - AI vehicle creation (MW sub_00172730, thiscall on the pvehicle key): StockStandIn.
// - Ride builder (MW sub_00140BE0, thiscall on the profile's car list): ListedHandle.
// - Type lookups (MW sub_00244FE0/245030), render connections (MW sub_00263F10/263FD0), model ID
//   (MW sub_001C9AB0): need TypeOfName, which needs the name/id offsets in the [0x4B3160] table.
// - Pool growth (MW sub_00054340): needs the Carbon pool size constants.
