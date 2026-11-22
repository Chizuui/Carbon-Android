// Where the textures the packs replace are (src/common/texture_packs.h). Carbon's GetTextureInfo is sub_00102310
// (cdecl: hash, default, unloaded): the bucket array at 0x45AEF0 indexed by (hash & 0xFF), the hash at [object+0x0C],
// the loaded list rooted at 0x45FE38, then sub_001CA650 and the table at 0x45C3C8; 0 when none has it and no default was
// asked for (default 0 returns 0 on a miss; the game itself calls it as (hash, 1, 0)). The track streams its packs in
// and out as the car goes, an address freed and taken by another texture: asked again every few frames, and the size
// kept to tell them apart.
//
// UNPROVEN — the step from the lookup's object to the D3D texture (kPlatD3D below): Most Wanted's TextureInfo has its
// platform info first and the D3D texture embedded at [plat+0x10]; Carbon's object is proven to be a wrapper (its
// [object+0x0C] is the name hash, so it is not the D3D header itself) whose first field is a pointer (the flare pass
// dereferences it once, sub_0010EC90), and the cube loader (sub_001032F0) reads a byte size at [object+0x24] and pixel
// data at [object+0x50] — consistent with a 20-byte D3D container at [plat+0x10] (its Size at +0x20, a byte size just
// past it at +0x24), and with the flare's use of [plat]+0x14 as the Data field's address. But the container could sit
// at +0x14 instead, which would make this read the wrong header. gpu::Describe checks the header's Common type bits,
// so a wrong offset fails closed (the pack simply does not apply, the game's own texture draws) — it cannot misbind.
// The first sighting of each hash is logged with the wrapper's [object+0x50] data pointer, so one tester log settles
// which offset is right: if d.data there equals [object+0x50], +0x10 is proven; if the log shows Describe failing
// (0 of N replaced) while the game plainly has the texture, try +0x14. Next RE step if it fails: from a phone log,
// take one (hash, info, plat) triple, find that texture's D3D header address in a bind (native/vulkan.cpp logs the
// bound s.textures[stage] with -v), and compare it against plat+0x10 and plat+0x14.
#include <unordered_map>
#include <unordered_set>

#include "common/options.h"
#include "common/texture_packs.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

namespace {
constexpr uint32_t kGetTextureInfo = 0x102310, kPlatD3D = 0x10;  // kPlatD3D: UNPROVEN, see the header comment
constexpr uint32_t kEvery = 8;  // presents
}  // namespace

void TexturePacksFrame(X86Context& ctx) {
    const std::vector<uint32_t>& hashes = texpacks::Hashes();
    static uint32_t frames = 0;
    if (hashes.empty() || frames++ % kEvery) return;
    std::unordered_map<uint32_t, texpacks::Bound> bound;
    // No TEXTURE PACKS toggle in Carbon's Options yet (src/common/options.h has no texture_packs field): packs are
    // always drawn while installed. When the option exists, gate here like upstream: publish an empty bound while off,
    // and native/vulkan.cpp binds the game's own again at the next draw.
    uint32_t saved[] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    static std::unordered_set<uint32_t> seen;  // first sighting per hash, for the kPlatD3D check above
    for (uint32_t hash : hashes) {
        uint32_t info = kernel::CallGuest(ctx, kGetTextureInfo, {hash, 0, 0});
        if (!info || !mem::IsCommitted(info)) continue;
        uint32_t plat = kernel::R32(info);
        if (!plat || !mem::IsCommitted(plat + kPlatD3D + 0x14)) continue;
        gpu::TextureDesc d;
        if (!gpu::Describe(plat + kPlatD3D, d) || !d.data || d.cube || d.depth != 1 || d.linear) continue;
        if (seen.insert(hash).second) {
            uint32_t wrap_data = mem::IsCommitted(info + 0x50) ? kernel::R32(info + 0x50) : 0;
            LOGR("texpacks: %08X at info %08X plat %08X data %08X %ux%u (wrapper data %08X)", hash, info, plat,
                 d.data, d.width, d.height, wrap_data);
        }
        bound[d.data] = {hash, d.width, d.height};
    }
    ctx.eax = saved[0], ctx.ecx = saved[1], ctx.edx = saved[2], ctx.ebx = saved[3], ctx.esi = saved[4],
    ctx.edi = saved[5], ctx.ebp = saved[6];
    // the count as the track streams, every 10 s at most (75 times kEvery presents at 60 fps)
    static size_t shown = ~size_t(0);
    static uint32_t quiet = 0;
    if (bound.size() != shown && quiet == 0) {
        shown = bound.size();
        quiet = 75;
        LOGR("texpacks: %zu of %zu replaced textures loaded in the game", shown, hashes.size());
    }
    if (quiet) quiet--;
    texpacks::Publish(std::move(bound));
}

}  // namespace game
