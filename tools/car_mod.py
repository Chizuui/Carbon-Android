# turns a PC add-on car (Binary and VltEd install files) into files the Xbox game reads, as an add-on car of its own:
#   car_mod.py <disc image> <mod folder> <out folder>
# A car named as one of the game's takes that car's place instead. The app (app/src/main/java/com/racas/mostwanted/
# CarConverter.java) reads the same formats and holds many cars: a change to a format goes in both. out/ goes in
# Carbon/mods on the phone (NFS/ZDIR.BIN, NFS/ZZDATA16.BIN, cars.txt, car_types.txt, name_<hash>.dxt); the kernel
# reads them over the disc's. One car at a time.
#
# Carbon, not Most Wanted: every format detail here was re-derived from Carbon's own disc, because the two games
# differ in the places marked CARBON below. Where a detail could not be proven from the disc it is NOT guessed: the
# code says so, does the safe thing, and names the evidence still needed. The open items, with their next
# reverse-engineering step each, are in CarConverter.java's header note.
import argparse, collections, glob, heapq, math, os, re, struct, sys

# CARBON: 95 stock types (0x34600 has 95 rows of 0xD0), 95 is the "no car" answer, add-ons start at 96. There is
# no BMWM3 in Carbon and no parts template car: patch_global copies no parts rows (see CarConverter.java).
STOCK_TYPES = 95
FIRST_ADDON_TYPE = 96
VLT_CLASSES_SKIPPED = ("emitterdata", "emittergroup")  # smoke with its place moved, not worth new nodes


def align(n, a):
    return (n + a - 1) & ~(a - 1)


# hashes

def bhash(s):
    h = 0xFFFFFFFF
    for c in s.encode("latin-1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def _mix(a, b, c):
    a = (a - b - c) & 0xFFFFFFFF; a ^= c >> 13
    b = (b - c - a) & 0xFFFFFFFF; b ^= (a << 8) & 0xFFFFFFFF
    c = (c - a - b) & 0xFFFFFFFF; c ^= b >> 13
    a = (a - b - c) & 0xFFFFFFFF; a ^= c >> 12
    b = (b - c - a) & 0xFFFFFFFF; b ^= (a << 16) & 0xFFFFFFFF
    c = (c - a - b) & 0xFFFFFFFF; c ^= b >> 5
    a = (a - b - c) & 0xFFFFFFFF; a ^= c >> 3
    b = (b - c - a) & 0xFFFFFFFF; b ^= (a << 10) & 0xFFFFFFFF
    c = (c - a - b) & 0xFFFFFFFF; c ^= b >> 15
    return a, b, c


def attrib_key(s):  # Attrib::StringToKey, Jenkins' lookup2 from 0xABCDEF00 (0x047520)
    k = s.encode("latin-1")
    a = b = 0x9E3779B9
    c = 0xABCDEF00
    i = 0
    while len(k) - i >= 12:
        a = (a + int.from_bytes(k[i:i + 4], "little")) & 0xFFFFFFFF
        b = (b + int.from_bytes(k[i + 4:i + 8], "little")) & 0xFFFFFFFF
        c = (c + int.from_bytes(k[i + 8:i + 12], "little")) & 0xFFFFFFFF
        a, b, c = _mix(a, b, c)
        i += 12
    c = (c + len(k)) & 0xFFFFFFFF
    r = k[i:]
    for j, s_ in ((10, 24), (9, 16), (8, 8)):
        if len(r) > j: c = (c + (r[j] << s_)) & 0xFFFFFFFF
    for j, s_ in ((7, 24), (6, 16), (5, 8), (4, 0)):
        if len(r) > j: b = (b + (r[j] << s_)) & 0xFFFFFFFF
    for j, s_ in ((3, 24), (2, 16), (1, 8), (0, 0)):
        if len(r) > j: a = (a + (r[j] << s_)) & 0xFFFFFFFF
    return _mix(a, b, c)[2]


# JDLZ

def jdlz_decode(b):
    # as the game's (0x1BAD20): it counts the input down and stops only at exactly 0, so a stream that ends anywhere else
    # runs on over the heap. That is an error here. The next flags are read right after the token that ran them out
    if b[:4] != b"JDLZ":
        raise ValueError("no JDLZ header")
    usize, csize = struct.unpack_from("<II", b, 8)
    out = bytearray(usize)
    f1, f2 = b[16] | 0x100, b[17] | 0x100
    ip, op, left = 18, 0, csize - 18
    while left:
        if left < 0:
            raise ValueError("JDLZ input does not end where its size says")
        if f1 & 1:
            if f2 & 1:
                n = (b[ip + 1] | ((b[ip] & 0xF0) << 4)) + 3
                t = (b[ip] & 0x0F) + 1
            else:
                t = (b[ip + 1] | ((b[ip] & 0xE0) << 3)) + 17
                n = (b[ip] & 0x1F) + 3
            if op + n > usize:
                raise ValueError("JDLZ output past its size")
            for i in range(n):
                out[op + i] = out[op + i - t]
            op += n
            ip += 2
            left -= 2
            f2 >>= 1
        else:
            if op < usize:
                out[op] = b[ip]
                op += 1
                ip += 1
            left -= 1
        f1 >>= 1
        if f1 == 1:
            f1 = (b[ip] if ip < len(b) else 0) | 0x100
            ip += 1
            left -= 1
        if f2 == 1:
            f2 = (b[ip] if ip < len(b) else 0) | 0x100
            ip += 1
            left -= 1
    if op != usize:
        raise ValueError("JDLZ output short of its size")
    return bytes(out)


def jdlz_encode(d):
    # the two flag bytes come where the decoder reads them: each is fetched when the last one ran out, before a token
    out = bytearray(b"JDLZ\x02\x10\0\0")
    out += struct.pack("<II", len(d), 0)
    table = {}
    f1_at = f2_at = -1
    f1_n = f2_n = 8
    f1 = f2 = 0
    i, n = 0, len(d)
    while i < n:
        if f1_n == 8:
            if f1_at >= 0: out[f1_at] = f1
            f1_at = len(out); out.append(0); f1 = 0; f1_n = 0
        if f2_n == 8:
            if f2_at >= 0: out[f2_at] = f2
            f2_at = len(out); out.append(0); f2 = 0; f2_n = 0
        best_len = best_dist = 0
        if i + 3 <= n:
            key = d[i:i + 3]
            cand = table.get(key)
            if cand is not None:
                for p in reversed(cand[-16:]):
                    dist = i - p
                    if dist > 2064:
                        break
                    limit = min(n - i, 4098 if dist <= 16 else 34)
                    m = 3
                    while m < limit and d[p + m] == d[i + m]:
                        m += 1
                    if m > best_len:
                        best_len, best_dist = m, dist
                        if m == limit:
                            break
            table.setdefault(key, []).append(i)
        if best_len >= 3:
            f1 |= 1 << f1_n
            if best_dist <= 16:
                f2 |= 1 << f2_n
                out.append(((best_len - 3) >> 8) << 4 | (best_dist - 1))
                out.append((best_len - 3) & 0xFF)
            else:
                t = best_dist - 17
                out.append(((t >> 3) & 0xE0) | (best_len - 3))
                out.append(t & 0xFF)
            f2_n += 1
            for j in range(i + 1, min(i + best_len, n - 2)):
                table.setdefault(d[j:j + 3], []).append(j)
            i += best_len
        else:
            out.append(d[i])
            i += 1
        f1_n += 1
    if f1_at >= 0: out[f1_at] = f1
    if f2_at >= 0: out[f2_at] = f2
    # the decoder fetches flags that ran out on the last token too, and those count
    if f1_n == 8: out.append(0)
    if f2_n == 8: out.append(0)
    struct.pack_into("<I", out, 12, len(out))
    return bytes(out)


# the disc

class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        for part in (0, 0x18300000, 0xFD90000, 0x2080000):
            self.f.seek(part + 32 * 2048)
            if self.f.read(20) == b"MICROSOFT*XBOX*MEDIA":
                self.base = part
                break
        else:
            sys.exit("not an Xbox disc image")
        sector, size = struct.unpack("<II", self.f.read(8))
        root = self._dir(sector, size)
        self.nfs = self._dir(*root["NFS"][:2])
        self.zdir = self._file(self.nfs["ZDIR.BIN"])
        self.recs = {}
        for i in range(0, len(self.zdir), 24):
            r = struct.unpack_from("<IIIIII", self.zdir, i)
            self.recs[r[0]] = r

    def _dir(self, sector, size):
        self.f.seek(self.base + sector * 2048)
        t = self.f.read(size)
        out = {}
        def walk(off):
            if off * 4 + 14 > len(t):
                return
            left, right, s, sz, attr, nl = struct.unpack_from("<HHIIBB", t, off * 4)
            if left == 0xFFFF:
                return
            out[t[off * 4 + 14:off * 4 + 14 + nl].decode("latin-1")] = (s, sz, attr)
            if left: walk(left)
            if right: walk(right)
        walk(0)
        return out

    def _file(self, e):
        self.f.seek(self.base + e[0] * 2048)
        return self.f.read(e[1])

    def load(self, path):
        r = self.recs[bhash(path)]
        arc = self.nfs["ZZDATA%d.BIN" % r[1]]
        self.f.seek(self.base + (arc[0] + r[2]) * 2048)
        return self.f.read(r[4])


# chunks

def chunks(b, off=0, end=None):
    end = len(b) if end is None else end
    while off + 8 <= end:
        c, s = struct.unpack_from("<II", b, off)
        if off + 8 + s > end:
            break
        yield c, s, off + 8
        off += 8 + s


def payload(b, o, s):  # chunk data is aligned with a fill of 0x11
    p = o
    while p + 4 <= o + s and b[p:p + 4] == b"\x11\x11\x11\x11":
        p += 4
    return p


def chunk(cid, data):
    return struct.pack("<II", cid, len(data)) + data


def chunk_aligned(cid, data, at, al):
    # the header at file offset at, its data on a multiple of al
    start = at + 8
    pad = align(start, al) - start
    return chunk(cid, b"\x11" * pad + data)


# geometry

LOD_UP = {"A": "B", "B": "C", "C": "D", "D": "E"}  # the Xbox cars' best is B; only placeholders have an A


class Solid:
    pass


def unpack(block, usize):
    # a block of a pack: EA's compressed forms have a 16 byte header, the size within at +8. RAWW only stores
    if block[:4] == b"JDLZ":
        return jdlz_decode(block)
    if block[:4] == b"RAWW":
        n = struct.unpack_from("<I", block, 8)[0]
        return block[16:16 + n]
    if block[:4] in (b"HUFF", b"COMP", b"LZMA"):
        raise ValueError("packed as " + block[:4].decode())
    return block[:usize]


CARBON_BLOCK = 0x55441122
CARBON_PART = 0x8000
CARBON_SPLIT = 0x8000


def unpack_carbon(span, usize):
    # Carbon's block run: [0x55441122][decompressed size][stored size][flags][cumulated offset][0] over a JDLZ
    # stream (or HUFF, which has no decoder here). Over 32768 decompressed bytes the solid is split into
    # 32768-byte pieces from its start, stored in reverse order, the first piece flagged 0x8000.
    parts = []
    p = total = 0
    while p + 24 <= len(span) and total < usize:
        magic, us, cs = struct.unpack_from("<III", span, p)
        if magic != CARBON_BLOCK:
            raise ValueError("a Carbon block's magic")
        if cs < 24 or p + cs > len(span):
            raise ValueError("a Carbon block's size")
        payload = span[p + 24:p + cs]
        if payload[:4] == b"JDLZ":
            parts.append(jdlz_decode(payload))
        elif payload[:4] == b"HUFF":
            raise ValueError("packed as HUFF")
        elif us <= len(payload):
            parts.append(payload[:us])
        else:
            raise ValueError("a Carbon block's packing")
        total += us
        p += cs
    if total != usize:
        raise ValueError("a Carbon block's size")
    return b"".join(reversed(parts))  # stored in reverse order


def carbon_block(flat):
    # a flat solid as Carbon stores it: one block, or 32768-byte pieces in reverse order when it is over that size
    out = bytearray()
    n = len(flat)
    pieces = max(1, (n + CARBON_SPLIT - 1) // CARBON_SPLIT)
    cum = 0
    for i in range(pieces - 1, -1, -1):
        part = flat[i * CARBON_SPLIT:min(n, (i + 1) * CARBON_SPLIT)]
        z = jdlz_encode(part)
        if jdlz_decode(z) != part:
            sys.exit("JDLZ encoder failed its round trip")
        stored = align(24 + len(z), 4)
        out += struct.pack("<6I", CARBON_BLOCK, len(part), stored, CARBON_PART if i == pieces - 1 else 0, cum, 0)
        out += z
        out += b"\0" * (stored - 24 - len(z))
        cum += stored
    return bytes(out)


def read_mod_solids(mg):
    index = next(mg[o:o + s] for c, s, o in walk_all(mg) if c == 0x134004)
    out = []
    for i in range(0, len(index), 24):
        h, off, cs, us, fl, _ = struct.unpack_from("<6I", index, i)
        # a Carbon block run, or MW's packing when the mod has none (the PC Carbon layout is unproven: no PC
        # Carbon car mod was on hand, so both are read)
        if struct.unpack_from("<I", mg, off)[0] == CARBON_BLOCK:
            out.append(unpack_carbon(mg[off:off + cs], us))
        else:
            out.append(unpack(mg[off:off + max(cs, us)], us))
    return out


def walk_all(b, off=0, end=None):
    for c, s, o in chunks(b, off, end):
        yield c, s, o
        if c & 0x80000000:
            yield from walk_all(b, o, o + s)


# a mesh chunk's data starts on 16 bytes, vertices on 0x80 (0x0C7490), the gap filled with 0x11 or, by some tools, with 0: the
# zeros are the gap if they lead to that place and what is left is whole records. Read as data, they shifted everything after
def mod_payload(d, o, size, alignment, record):
    p = payload(d, o, size)
    if p != o:
        return p
    at = align(o, alignment)
    if at == o or at > o + size or (o + size - at) % record:
        return o
    if any(d[o:at]):
        return o
    return at


MAX_GROUP_INDICES, MAX_GROUP_VERTICES = 0xFFFC, 0xFFF0
MAX_SOLID_VERTICES = 60000  # the solid's count is 16 bits too


# a group's flag 0x100 gives it the environment's reflection on the Xbox; the PC takes that from the shader, and mods' tools
# leave the bit off (every mod's paint was 0x14080, the disc's 0x14180: no reflection). It goes by the shader, as on every disc
# car's solids: these always have it, and 0x78743CA1 mostly
REFLECTIVE = {
    0x0000D916, 0x02A05578, 0x02DDDAD9, 0x02EE77E1, 0x05BC3A3C, 0x0FEDEE40, 0x12049251, 0x12C9453C, 0x1FBD2378, 0x1FF8C329,
    0x22719FA9, 0x2E65E067, 0x33A26CB6, 0x3ED70C43, 0x471A1DCA, 0x54949AFD, 0x721AFDDC, 0xA6348EE3, 0xB51B3ACF, 0xB57AB98E,
    0xD6640DFF, 0xD6D6080A, 0xD79597D6, 0xDCD01662, 0xF0A43B67, 0xFD102A92, 0x78743CA1,
}
# and these never (interior, plate, underside, ...), nor mostly 0x010CB64A
MATTE = {0x2787EDAB, 0x4C95C6F8, 0x52BF4C34, 0x5799E60B, 0x010CB64A}


def reflective(shader, flags):
    if shader in REFLECTIVE:
        return flags | 0x100
    if shader in MATTE:
        return flags & ~0x100
    return flags


def convert_solid(d, car):
    s = Solid()
    info = groups = idx = verts = None
    tex, sha = b"", b""
    for c, sz, o in walk_all(d):
        if c == 0x134011:
            info = d[o:o + sz]
        elif c == 0x134012:
            tex = d[o:o + sz]
        elif c == 0x134013:
            sha = d[o:o + sz]
        elif c == 0x134b02:
            p = mod_payload(d, o, sz, 16, 104); groups = d[p:o + sz]
        elif c == 0x134b03:
            p = mod_payload(d, o, sz, 16, 2); idx = d[p:o + sz]
        elif c == 0x134b01:
            p = mod_payload(d, o, sz, 0x80, 36); verts = d[p:o + sz]
    name = info[0xA0:].split(b"\0")[0].decode("latin-1")
    # a car made from another keeps that one's name inside (SUPRA_KIT00_BODY_A in BRIANSUPRA), with the hash of its own, which
    # is what the game looks a model up by: the name goes with the hash
    inner = struct.unpack_from("<I", info, 0x10)[0]
    underscore = name.find("_")
    if underscore > 0 and inner != bhash(name) and inner == bhash(car + name[underscore:]):
        name = car + name[underscore:]
    lod = LOD_UP.get(name[-1])
    if not name.endswith("_" + name[-1]) or lod is None or idx is None or verts is None:
        return None
    s.name = name[:-1] + lod
    s.hash = bhash(s.name)
    indices = struct.unpack("<%dH" % (len(idx) // 2), idx[:len(idx) & ~1])
    vb = []
    gr = bytearray()
    ib = []
    start = 0
    flags_or = 0
    total_v = 0
    s.ngroups = 0
    for g in range(len(groups) // 104):
        r = groups[g * 104:(g + 1) * 104]
        mn = struct.unpack_from("<3f", r, 0)
        mx = struct.unpack_from("<3f", r, 12)
        tex_i = r[0x18]
        sha_i = r[0x1D]
        flags, _vn, tris, first = struct.unpack_from("<IIII", r, 0x38)
        flags &= 0x1FFFF  # the disc's cars have no more
        if sha_i * 8 + 4 <= len(sha):
            flags = reflective(struct.unpack_from("<I", sha, sha_i * 8)[0], flags)
        count = tris * 3
        # the mod's groups share one buffer of pos, normal, colour, uv; the Xbox's have one each of pos, uv, normal
        # a group's counts and indices are 16 bits: a denser one goes on in another of the same material
        k = first
        end = min(first + count, len(indices))
        while True:
            remap = {}
            local = []
            b = bytearray()
            while k + 3 <= end and len(local) + 3 <= MAX_GROUP_INDICES and len(remap) + 3 <= MAX_GROUP_VERTICES:
                for i in indices[k:k + 3]:
                    if i not in remap:
                        remap[i] = len(remap)
                        v = verts[i * 36:(i + 1) * 36]
                        b += v[0:12] + v[28:36] + v[12:24]
                    local.append(remap[i])
                k += 3
            rec = bytearray(96)
            struct.pack_into("<3f", rec, 0, *mn)
            struct.pack_into("<3f", rec, 16, *mx)
            struct.pack_into("<HHIII", rec, 0x20, len(local), 3, tex_i, flags, start)
            struct.pack_into("<I", rec, 0x40, sha_i)
            struct.pack_into("<H", rec, 0x52, len(remap))  # CARBON: the group's vertex count
            gr += rec
            vb.append(bytes(b))
            ib += local
            start += len(local)
            total_v += len(remap)
            s.ngroups += 1
            if k + 3 > end:
                break
        flags_or |= flags
    if len(ib) % 2:
        ib.append(0)  # the disc's index chunks end on 4 bytes
    s.groups, s.vb, s.ib = bytes(gr), vb, struct.pack("<%dH" % len(ib), *ib)
    s.flags = flags_or & 0xFFFF
    s.verts = total_v
    s.tex, s.sha = tex, sha
    s.tris = start // 3
    # the Xbox's header: a flag more, its name, the radius and tris per radius the mod leaves 0
    head = bytearray(info[:0xA0])
    struct.pack_into("<I", head, 0xC, 0x400016)
    struct.pack_into("<I", head, 0x10, s.hash)
    struct.pack_into("<I", head, 0x14, s.tris)
    mn = struct.unpack_from("<3f", head, 0x20)
    mx = struct.unpack_from("<3f", head, 0x30)
    radius = max(0.001, sum((b_ - a_) ** 2 for a_, b_ in zip(mn, mx)) ** 0.5 / 2)
    struct.pack_into("<ff", head, 0x98, radius, s.tris / radius)
    nm = s.name.encode("latin-1") + b"\0"
    s.head = bytes(head) + nm + b"\0" * (align(len(nm), 4) - len(nm))
    return s


# the car pool holds every car of a race, and a car loads about its whole file. PC mods often repeat their best model
# at every LOD: those lower LODs are thinned to their share, and only a car over this limit has its best models cut.
# CARBON: the pool's size is not yet known (src/game/car_mods.cpp grows no pool: its size constants were not found
# in the recompile), so 12 MB is a working budget, about twice the Gallardo's 5.8 MB file; lower it if cars fail to load
LOD_SHARE = (1, 0.12, 0.04, 0.015)  # of the part's best, by LODs below it: the player sees the best
MAX_GEOMETRY = 12 << 20
VERTEX = 32  # pos, uv, normal


def solid_bytes(s):
    return len(s.ib) + sum(len(v) + 0x80 for v in s.vb)


# the most triangles a part may keep for them all to fit the budget, a part's bytes going with its triangles
def triangle_cap(tops, budget):
    low, high = 1, max([1] + [s.tris for s in tops])
    while low < high:
        mid = (low + high + 1) // 2
        total = sum(solid_bytes(s) * min(s.tris, mid) // max(s.tris, 1) for s in tops)
        if total <= budget:
            low = mid
        else:
            high = mid - 1
    return low


def reduce_lods(solids):
    best = {}
    for s in solids:
        part = s.name[:-1]
        if part not in best or s.name < best[part].name:
            best[part] = s
    before = sum(solid_bytes(s) for s in solids)
    for s in solids:
        if s.verts > MAX_SOLID_VERTICES:
            reduce_solid(s, int(s.tris * MAX_SOLID_VERTICES / s.verts), s is best[s.name[:-1]])
    tops = [best[part] for part in sorted(best)]
    budget = int(MAX_GEOMETRY / sum(LOD_SHARE))
    # what stands out is cut first, the small parts that show most (a wheel) last; a vertex costs more than was reckoned, so again
    for _ in range(4):
        if sum(solid_bytes(b) for b in tops) <= budget:
            break
        cap = triangle_cap(tops, budget)
        for b in tops:
            reduce_solid(b, cap, True)
    for s in solids:
        b = best[s.name[:-1]]
        below = ord(s.name[-1]) - ord(b.name[-1])
        if below > 0:
            reduce_solid(s, int(b.tris * LOD_SHARE[min(below, len(LOD_SHARE) - 1)]))
    # the lower LODs may have stopped above their share: the best ones pay for it
    for _ in range(4):
        total = sum(solid_bytes(s) for s in solids)
        if total <= MAX_GEOMETRY:
            break
        cap = triangle_cap(tops, sum(solid_bytes(b) for b in tops) - (total - MAX_GEOMETRY))
        for b in tops:
            reduce_solid(b, cap, True)
    print("geometry: %d KB, to %d KB" % (before // 1024, sum(solid_bytes(s) for s in solids) // 1024))


# a mesh far over its target, or with too many open edges for edge collapse to get there: the vertices of a grid cell facing
# the same octant become the first of them, each shading group alone, and triangles left with a side of no length go. The
# finest grid that gets to the goal: on a mesh this dense its cells are too small to see (coarse ones tore a low one's wheels)
def cluster_toward(s, goal):
    if s.tris <= goal:
        return
    found = None
    low, high = 2, 1024
    while low <= high:
        mid = (low + high) // 2
        r = cluster_solid(s, mid)
        if r[4] <= goal:
            found = r
            low = mid + 1
        else:
            high = mid - 1
    if found is None:
        found = cluster_solid(s, 2)
    apply_mesh(s, *found)


def apply_mesh(s, groups, vb, ib, verts, tris):
    s.groups, s.vb, s.ib, s.verts, s.tris = groups, vb, ib, verts, tris
    head = bytearray(s.head)
    struct.pack_into("<I", head, 0x14, s.tris)
    struct.pack_into("<f", head, 0x9C, s.tris / struct.unpack_from("<f", head, 0x98)[0])
    s.head = bytes(head)


def cluster_solid(s, grid):
    points = [struct.unpack_from("<3f", v, o) for v in s.vb for o in range(0, len(v) - VERTEX + 1, VERTEX)]
    low = [min(p[a] for p in points) for a in range(3)]
    high = [max(p[a] for p in points) for a in range(3)]
    extent = max(high[a] - low[a] for a in range(3))
    cell = extent / grid if extent > 0 else 1
    groups = bytearray(s.groups)
    ib = []
    vb = []
    start = verts = 0
    for g in range(s.ngroups):
        rec = g * 96
        count = struct.unpack_from("<H", s.groups, rec + 0x20)[0]
        first = struct.unpack_from("<I", s.groups, rec + 0x2C)[0]
        v = s.vb[g]
        ids = {}
        nv = bytearray()
        kept = 0
        for t in range(0, count - 2, 3):
            corners = struct.unpack_from("<3H", s.ib, (first + t) * 2)
            keys = []
            for i in corners:
                o = i * VERTEX
                pos = struct.unpack_from("<3f", v, o)
                normal = struct.unpack_from("<3f", v, o + 20)
                key = 0
                for a in range(3):
                    key |= int((pos[a] - low[a]) / cell) << (a * 20)
                    if normal[a] < 0:
                        key |= 1 << (60 + a)
                keys.append(key)
            if keys[0] == keys[1] or keys[1] == keys[2] or keys[0] == keys[2]:
                continue
            for key, i in zip(keys, corners):
                if key not in ids:
                    ids[key] = len(ids)
                    nv += v[i * VERTEX:(i + 1) * VERTEX]
                ib.append(ids[key])
            kept += 3
        # a group left empty would be a chunk of nothing: its first triangle stays
        if kept == 0 and count >= 3:
            for c, i in enumerate(struct.unpack_from("<3H", s.ib, first * 2)):
                nv += v[i * VERTEX:(i + 1) * VERTEX]
                ib.append(c)
            kept = 3
            verts += 3
        struct.pack_into("<H", groups, rec + 0x20, kept)
        struct.pack_into("<I", groups, rec + 0x2C, start)
        struct.pack_into("<H", groups, rec + 0x52, len(nv) // VERTEX)  # CARBON: the group's vertex count
        vb.append(bytes(nv))
        start += kept
        verts += len(ids)
    if start % 2:
        ib.append(0)  # the disc's index chunks end on 4 bytes
    return bytes(groups), vb, struct.pack("<%dH" % len(ib), *ib), verts, start // 3


# each shading group thinned by as much (quadric edge collapse), from a mesh brought near by clustering if it is far over,
# and to the target by clustering if collapse stops short. Clustering alone tore a low mesh's wheels and faceted its body;
# a best LOD, the one the player sees, is never clustered (a Grande Panda's body came out in shards): collapse only, and
# what it leaves over the target stays
def reduce_solid(s, target, best=False):
    target = max(target, 1)
    if s.tris <= target:
        return
    if best:
        collapse_toward(s, target)
        if s.tris > target + target // 4:
            collapse_toward(s, target)  # the first pass's seams are open edges of the second
        return
    cluster_toward(s, target * 2)
    collapse_toward(s, target)
    if s.tris > target + target // 4:
        cluster_toward(s, target)


def collapse_toward(s, target):
    if s.tris <= target:
        return
    share = target / s.tris
    groups = bytearray(s.groups)
    vb = []
    ib = []
    start = verts = 0
    for g in range(s.ngroups):
        rec = g * 96
        count = struct.unpack_from("<H", s.groups, rec + 0x20)[0]
        first = struct.unpack_from("<I", s.groups, rec + 0x2C)[0]
        n = count - count % 3
        tri = list(struct.unpack_from("<%dH" % n, s.ib, first * 2))
        d = Decimator(s.vb[g], tri)
        d.run(max(1, int(n // 3 * share)))
        nv, local = d.emit(s.vb[g])
        struct.pack_into("<H", groups, rec + 0x20, len(local))
        struct.pack_into("<I", groups, rec + 0x2C, start)
        struct.pack_into("<H", groups, rec + 0x52, len(nv) // VERTEX)  # CARBON: the group's vertex count
        vb.append(nv)
        ib += local
        verts += len(nv) // VERTEX
        start += len(local)
    if start % 2:
        ib.append(0)  # the disc's index chunks end on 4 bytes
    apply_mesh(s, bytes(groups), vb, struct.pack("<%dH" % len(ib), *ib), verts, start // 3)


def edge_key(a, b):
    return (a, b) if a < b else (b, a)


# one group's mesh. A vertex goes into a neighbour, the pair costing least by their planes' quadrics, so what is left keeps
# its own position, uv and normal. An open edge is a seam with the next group or a change of uv: its vertices stay until
# nothing else can go, then go only along it, at a price
class Decimator:
    def __init__(self, v, tri):
        n = self.n = len(v) // VERTEX
        self.tri = tri
        self.pos = [struct.unpack_from("<3f", v, i * VERTEX) for i in range(n)]
        self.uv = [struct.unpack_from("<2f", v, i * VERTEX + 12) for i in range(n)]
        self.normal = [struct.unpack_from("<3f", v, i * VERTEX + 20) for i in range(n)]
        self.q = [[0.0] * 10 for _ in range(n)]
        self.version = [0] * n
        self.dead_vert = [False] * n
        self.border = [False] * n
        self.live = len(tri) // 3
        self.dead_tri = [False] * self.live
        self.adj = [[] for _ in range(n)]
        self.border_edges = set()
        edges = {}
        area = edge = 0.0
        for t in range(self.live):
            a, b, c = tri[t * 3:t * 3 + 3]
            self.adj[a].append(t)
            self.adj[b].append(t)
            self.adj[c].append(t)
            for e in (edge_key(a, b), edge_key(b, c), edge_key(c, a)):
                edges[e] = edges.get(e, 0) + 1
            edge += self.dist2(a, b) + self.dist2(b, c) + self.dist2(c, a)
            m = self.cross(a, b, c)
            length = math.sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2])
            if length <= 0:
                continue
            area += length / 2
            x, y, z = m[0] / length, m[1] / length, m[2] / length
            pa = self.pos[a]
            w = -(x * pa[0] + y * pa[1] + z * pa[2])
            plane = (x * x, x * y, x * z, x * w, y * y, y * z, y * w, z * z, z * w, w * w)
            for k in range(10):
                add = plane[k] * (length / 2)
                self.q[a][k] += add
                self.q[b][k] += add
                self.q[c][k] += add
        for e, uses in edges.items():
            if uses == 1:
                self.border_edges.add(e)
                self.border[e[0]] = self.border[e[1]] = True
        count = max(self.live, 1)
        self.scale = area / count * (edge / (3.0 * count))
        if self.scale <= 0:
            self.scale = 1

    def dist2(self, a, b):
        pa, pb = self.pos[a], self.pos[b]
        x, y, z = pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]
        return x * x + y * y + z * z

    def cross(self, a, b, c):
        pa, pb, pc = self.pos[a], self.pos[b], self.pos[c]
        ux, uy, uz = pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]
        vx, vy, vz = pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]
        return (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)

    def cost(self, u, v):
        x, y, z = self.pos[v]
        a, b = self.q[u], self.q[v]
        e = ((a[0] + b[0]) * x * x + 2 * (a[1] + b[1]) * x * y + 2 * (a[2] + b[2]) * x * z
             + 2 * (a[3] + b[3]) * x + (a[4] + b[4]) * y * y + 2 * (a[5] + b[5]) * y * z
             + 2 * (a[6] + b[6]) * y + (a[7] + b[7]) * z * z + 2 * (a[8] + b[8]) * z + (a[9] + b[9]))
        du, dv = self.uv[u][0] - self.uv[v][0], self.uv[u][1] - self.uv[v][1]
        nu, nv = self.normal[u], self.normal[v]
        dot = nu[0] * nv[0] + nu[1] * nv[1] + nu[2] * nv[2]
        e += self.scale * (0.1 * min(du * du + dv * dv, 1.0) + 0.1 * max(1.0 - dot, 0.0))
        if self.border[u]:
            e += self.scale
        return e

    def neighbours(self, u):
        out = set()
        tri = self.tri
        for t in self.adj[u]:
            if not self.dead_tri[t]:
                out.update(tri[t * 3:t * 3 + 3])
        out.discard(u)
        return out

    # u may go into v: an open edge only along itself, no fold where the two share more than an edge's two neighbours,
    # no triangle turned over or left with no area
    def can(self, u, v, seams):
        if self.border[u] and (not seams or edge_key(u, v) not in self.border_edges):
            return False
        if len(self.neighbours(u) & self.neighbours(v)) > 2:
            return False
        tri = self.tri
        for t in self.adj[u]:
            if self.dead_tri[t]:
                continue
            a, b, c = tri[t * 3:t * 3 + 3]
            if a == v or b == v or c == v:
                continue
            before = self.cross(a, b, c)
            after = self.cross(v if a == u else a, v if b == u else b, v if c == u else c)
            bb = before[0] * before[0] + before[1] * before[1] + before[2] * before[2]
            aa = after[0] * after[0] + after[1] * after[1] + after[2] * after[2]
            if aa <= 1e-24:
                return False
            dot = before[0] * after[0] + before[1] * after[1] + before[2] * after[2]
            if dot <= 0 or dot * dot < 0.04 * aa * bb:
                return False
        return True

    def push(self, heap, u, v):
        heapq.heappush(heap, (self.cost(u, v), u, v, self.version[u], self.version[v]))

    def run(self, target):
        for seams in (False, True):
            if self.live <= target:
                break
            heap = []
            for u in range(self.n):
                for v in sorted(self.neighbours(u)):
                    self.push(heap, u, v)
            while self.live > target and heap:
                _, u, v, vu, vv = heapq.heappop(heap)
                if self.dead_vert[u] or self.dead_vert[v] or self.version[u] != vu or self.version[v] != vv or not self.can(u, v, seams):
                    continue
                self.collapse(u, v)
                for w in sorted(self.neighbours(v)):
                    self.push(heap, v, w)
                    self.push(heap, w, v)

    def collapse(self, u, v):
        for w in sorted(self.neighbours(u)):
            if w != v and edge_key(u, w) in self.border_edges:
                self.border_edges.add(edge_key(v, w))
        if self.border[u]:
            self.border[v] = True
        tri = self.tri
        for t in self.adj[u]:
            if self.dead_tri[t]:
                continue
            a, b, c = tri[t * 3:t * 3 + 3]
            if a == v or b == v or c == v:
                self.dead_tri[t] = True
                self.live -= 1
                continue
            for k in range(3):
                if tri[t * 3 + k] == u:
                    tri[t * 3 + k] = v
            self.adj[v].append(t)
        qu, qv = self.q[u], self.q[v]
        for k in range(10):
            qv[k] += qu[k]
        self.dead_vert[u] = True
        self.version[v] += 1

    # what is left, vertices in the order first used
    def emit(self, v):
        ids = {}
        nv = bytearray()
        local = []
        for t in range(len(self.dead_tri)):
            if self.dead_tri[t]:
                continue
            for i in self.tri[t * 3:t * 3 + 3]:
                if i not in ids:
                    ids[i] = len(ids)
                    nv += v[i * VERTEX:(i + 1) * VERTEX]
                local.append(ids[i])
        return bytes(nv), local


# a PC mod may keep its body in attachment slots (a kit's ATTACHMENT0 to 9), which no car of the disc uses and which
# the add-on's (empty) parts rows have no row for: they never drew, leaving a car's panels out. A kit's are its body,
# so they go
# into its BODY, LOD by LOD; a style's are options no menu here can pick, so they go
# CARBON: wide-body kits are KITW01..W04 (the Gallardo has KITW01..W04_BODY_E); the disc's kits are KIT00 and KITW
ATTACHMENT = re.compile(r"(.*_)(KITW?\d\d|STYLE\d\d)_ATTACHMENT\d_([A-E])$")


def merge_attachments(solids):
    by_name = {x.name: x for x in solids}
    merged = dropped = 0
    for part in sorted(solids, key=lambda x: x.name):
        m = ATTACHMENT.match(part.name)
        if not m:
            continue
        if m.group(2).startswith("STYLE"):
            solids.remove(part)
            dropped += 1
            continue
        target = m.group(1) + m.group(2) + "_BODY_" + m.group(3)
        body = by_name.get(target)
        if body is None:
            rename_solid(part, target)
            by_name[target] = part
            merged += 1
        elif merge_solid(body, part) or fit_merge(body, part):
            solids.remove(part)
            merged += 1
    if merged + dropped:
        print("geometry: %d attachments into bodies, %d left out" % (merged, dropped))
    return merged + dropped > 0


# a body and an attachment over a solid's 16 bit vertex count together: both thinned by as much till they fit. Left
# out, the attachment stayed in its slot, which never draws, and the car lost its panels
def fit_merge(body, part):
    for _ in range(4):
        total = body.verts + part.verts
        if total <= MAX_SOLID_VERTICES and merge_solid(body, part):
            return True
        share = MAX_SOLID_VERTICES / max(total, 1)
        for s in (body, part):
            reduce_solid(s, int(s.tris * share), True)
    return merge_solid(body, part)


def rename_solid(s, name):
    s.name = name
    s.hash = bhash(name)
    nm = name.encode("latin-1") + b"\0"
    head = bytearray(s.head[:0xA0] + nm + b"\0" * (align(len(nm), 4) - len(nm)))
    struct.pack_into("<I", head, 0x10, s.hash)
    s.head = bytes(head)


# an 8 byte entry's place in a table, added if it is new
def table_entry(table, entries, at):
    entry = entries[at:at + 8]
    for i in range(0, len(table) - 7, 8):
        if table[i:i + 8] == entry:
            return i // 8
    table += entry
    return len(table) // 8 - 1


def index_count(s):
    return sum(struct.unpack_from("<H", s.groups, g * 96 + 0x20)[0] for g in range(s.ngroups))


# false when the two will not fit one solid: vertices and groups are 16 bit counts, a group's table indices bytes
def merge_solid(body, part):
    tex, sha = bytearray(body.tex), bytearray(body.sha)
    tex_at = [table_entry(tex, part.tex, i * 8) for i in range(len(part.tex) // 8)]
    sha_at = [table_entry(sha, part.sha, i * 8) for i in range(len(part.sha) // 8)]
    if body.verts + part.verts > 0xFFFF or len(tex) // 8 > 255 or len(sha) // 8 > 255:
        return False
    body_indices, part_indices = index_count(body), index_count(part)
    groups = bytearray(part.groups)
    for g in range(part.ngroups):
        rec = g * 96
        struct.pack_into("<I", groups, rec + 0x24, tex_at[struct.unpack_from("<I", groups, rec + 0x24)[0] & 255])
        struct.pack_into("<I", groups, rec + 0x2C, struct.unpack_from("<I", groups, rec + 0x2C)[0] + body_indices)
        struct.pack_into("<I", groups, rec + 0x40, sha_at[struct.unpack_from("<I", groups, rec + 0x40)[0] & 255])
    ib = body.ib[:body_indices * 2] + part.ib[:part_indices * 2]
    if (body_indices + part_indices) & 1:
        ib += b"\0\0"  # the disc's index chunks end on 4 bytes
    body.ib = ib
    body.groups = body.groups + bytes(groups)
    body.vb = body.vb + part.vb
    body.ngroups += part.ngroups
    body.verts += part.verts
    body.tris += part.tris
    body.flags |= part.flags
    body.tex, body.sha = bytes(tex), bytes(sha)
    head = bytearray(body.head)
    head[0x19] = len(tex) // 8
    head[0x1A] = len(sha) // 8
    struct.pack_into("<I", head, 0x14, body.tris)
    total = 0
    for a in range(3):
        low = min(struct.unpack_from("<f", head, 0x20 + a * 4)[0], struct.unpack_from("<f", part.head, 0x20 + a * 4)[0])
        high = max(struct.unpack_from("<f", head, 0x30 + a * 4)[0], struct.unpack_from("<f", part.head, 0x30 + a * 4)[0])
        struct.pack_into("<f", head, 0x20 + a * 4, low)
        struct.pack_into("<f", head, 0x30 + a * 4, high)
        total += (high - low) ** 2
    radius = max(0.001, total ** 0.5 / 2)
    struct.pack_into("<ff", head, 0x98, radius, body.tris / radius)
    body.head = bytes(head)
    return True


# a mod with no script of its own had the Gallardo's wheel places, its tires out of the body. They are measured off the model:
# TireOffsets' x is the middle of an arch in the lower edge of the skin, y the skin's width there (a disc car's tire model
# runs inward from 0, a mod's may be centred on 0, so what is on the far side of 0 comes off). On the disc's cars and a
# scripted mod that gives x within 2.5 cm of their TireOffsets and y within 1.5 cm
WHEEL_BIN = 0.02
NOT_BODY = re.compile(r".*(TIRE|BRAKE|WINDOW|INTERIOR|DRIVER|DAMAGE|DECAL|MIRROR|SPOILER|HOOD|GLASS|LIGHT).*")


def solid_triangles(s):
    out = []
    for g in range(s.ngroups):
        count = struct.unpack_from("<H", s.groups, g * 96 + 0x20)[0]
        start = struct.unpack_from("<I", s.groups, g * 96 + 0x2C)[0]
        v = s.vb[g]
        for t in range(0, count - 2, 3):
            out.append([struct.unpack_from("<3f", v, i * VERTEX) for i in struct.unpack_from("<3H", s.ib, (start + t) * 2)])
    return out


# None when the model will not say: no arches at both ends, or no tire
def measure_wheels(solids):
    tris = []
    tire = None
    for s in sorted(solids, key=lambda x: x.name):
        if s.name.endswith("_FRONT_TIRE_B"):
            points = [p for t in solid_triangles(s) for p in t]
            tire = (min(p[1] for p in points), max(p[1] for p in points), min(p[2] for p in points), max(p[2] for p in points))
        if s.name.endswith("_B") and ("_BASE_" in s.name or "_KIT00_" in s.name) and not NOT_BODY.match(s.name):
            tris += solid_triangles(s)
    if not tris or tire is None:
        return None
    points = [p for t in tris for p in t]
    x0, x1 = min(p[0] for p in points), max(p[0] for p in points)
    half = max(abs(p[1]) for p in points)
    z_low, z_top = min(p[2] for p in points), max(p[2] for p in points)
    bins = int((x1 - x0) / WHEEL_BIN) + 2
    bottom = [None] * bins
    for t in tris:
        if min(abs(p[1]) for p in t) < half * 0.7:
            continue
        ax, ay, az = t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]
        bx, by, bz = t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]
        nx, ny, nz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
        nl = math.sqrt(nx * nx + ny * ny + nz * nz)
        if nl == 0 or abs(ny) / nl < 0.5:
            continue
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            steps = int(abs(a[0] - b[0]) / WHEEL_BIN) + 1
            for i in range(steps + 1):
                f = i / steps
                x, z = a[0] + (b[0] - a[0]) * f, a[2] + (b[2] - a[2]) * f
                k = int((x - x0) / WHEEL_BIN)
                if bottom[k] is None or z < bottom[k]:
                    bottom[k] = z
    length = x1 - x0
    known = sorted(z for z in bottom if z is not None)
    if not known:
        return None
    limit = known[len(known) // 4] + (z_top - z_low) * 0.12
    middle = (x0 + x1) / 2
    # the widest run of the lower edge above the sill, or of no skin at all, in each half of the car
    front = rear = None
    start = -1
    for k in range(bins + 1):
        x = x0 + k * WHEEL_BIN
        high = k < bins and x0 + length * 0.04 < x < x1 - length * 0.04 and (bottom[k] is None or bottom[k] > limit)
        if high and start < 0:
            start = k
        elif not high and start >= 0:
            if (k - start) * WHEEL_BIN > length * 0.08:
                run = (x0 + start * WHEEL_BIN, x0 + k * WHEEL_BIN)
                if (run[0] + run[1]) / 2 > middle:
                    if front is None or run[1] - run[0] > front[1] - front[0]:
                        front = run
                elif rear is None or run[1] - run[0] > rear[1] - rear[0]:
                    rear = run
            start = -1
    if front is None or rear is None:
        return None
    front_half = max(abs(p[1]) for p in points if front[0] <= p[0] <= front[1])
    rear_half = max(abs(p[1]) for p in points if rear[0] <= p[0] <= rear[1])
    w = dict(front_x=(front[0] + front[1]) / 2, rear_x=(rear[0] + rear[1]) / 2, front_y=front_half + min(tire[0], 0),
             rear_y=rear_half + min(tire[0], 0), radius=(tire[3] - tire[2]) / 2, width=tire[1] - tire[0])
    if w["front_y"] < 0.4 or w["rear_y"] < 0.4 or not 0.1 <= w["radius"] <= 0.7:
        return None
    return w


def build_solid(s, at):
    # at: the file offset of its header; vertices on 0x80, groups and indices on 16, as on the disc
    head = chunk(0x134011, s.head) + chunk(0x134012, s.tex) + chunk(0x134013, s.sha)
    p = at + 8 + len(head) + 8  # 80134010 header, then 80134100 header
    mesh = bytearray()
    hdr = struct.pack("<IHHHH", 0xF, s.flags, s.verts, s.ngroups, 0x20) + b"\0" * 40
    # each read at its header + 0x17 rounded down to 16 (0x0C7490), vertices + 0x87 rounded down to 0x80
    for cid, data, al in ((0x134B00, hdr, 16), (0x134B02, s.groups, 16), (0x134B03, s.ib, 16)):
        c = chunk_aligned(cid, data, p + len(mesh), al)
        mesh += c
    for v in s.vb:
        mesh += chunk_aligned(0x134B01, v, p + len(mesh), 0x80)
    return chunk(0x80134010, head + chunk(0x80134100, bytes(mesh)))


def build_geometry(solids, template):
    t = next(template[o:o + s] for c, s, o in walk_all(template) if c == 0x134002)
    table_size = 8 + 144 + 8 + len(solids) * 8 + 8 + len(solids) * 24 + 8
    first = 8 + 8 + 8 + table_size  # 80134000, an empty 0, 80134001
    at = align(first + 8, 0x80)
    body = bytearray()
    index = []
    for s in sorted(solids, key=lambda x: x.hash):
        if at + len(body) & 0x7F:
            pad = (0x80 - (at + len(body) + 8) % 0x80) % 0x80
            body += chunk(0, b"\0" * pad)
        off = at + len(body)
        # CARBON: each solid is stored as 0x55441122 blocks, and the table's entry is [hash, offset, stored size,
        # decompressed size, 0x200, 0]. build_solid is given 0, so the mesh chunks are aligned within the flat
        # solid (the flat form is what the game decompresses and reads), not against the file.
        flat = build_solid(s, 0)
        c = carbon_block(flat)
        index.append((s.hash, off, len(c), len(flat)))
        body += c
    size = at + len(body)
    size = align(size, 0x80)
    info = bytearray(t)
    struct.pack_into("<I", info, 0xC, len(solids))
    struct.pack_into("<I", info, 0x68, size)
    hashes = b"".join(struct.pack("<II", h, 0) for h, _, _, _ in index)
    offs = b"".join(struct.pack("<6I", h, o, cs, us, 0x200, 0) for h, o, cs, us in index)
    table = chunk(0x134002, bytes(info)) + chunk(0x134003, hashes) + chunk(0x134004, offs) + chunk(0x80134008, b"")
    head = chunk(0, b"") + chunk(0x80134001, table)
    gap = at - (8 + len(head)) - 8
    inner = head + chunk(0, b"\0" * gap) + bytes(body)
    out = chunk(0x80134000, inner)
    return out + b"\0" * (size - len(out)) if len(out) < size else out


# textures

XBOX_FORMAT = {b"DXT1": 0x0C, b"DXT3": 0x0E, b"DXT5": 0x0F}


def dxt_block_size(fmt):
    return 8 if fmt == b"DXT1" else 16


def _565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _to565(r, g, b):
    return (int(r) >> 3) << 11 | (int(g) >> 2) << 5 | int(b) >> 3


def _block_quarters(cb, four_colour):
    # a block's colours as the four 2x2 averages, each (r, g, b, opaque pixels)
    c0, c1, bits = struct.unpack_from("<HHI", cb, 0)
    a, b = _565(c0), _565(c1)
    if four_colour or c0 > c1:
        pal = (a, b, tuple((2 * x + y) / 3 for x, y in zip(a, b)), tuple((x + 2 * y) / 3 for x, y in zip(a, b)))
        solid = (1, 1, 1, 1)
    else:
        pal = (a, b, tuple((x + y) / 2 for x, y in zip(a, b)), (0, 0, 0))
        solid = (1, 1, 1, 0)
    out = []
    for qy in (0, 2):
        for qx in (0, 2):
            r = g = bl = n = 0
            for py, px in ((qy, qx), (qy, qx + 1), (qy + 1, qx), (qy + 1, qx + 1)):
                i = (bits >> (2 * (py * 4 + px))) & 3
                r += pal[i][0]; g += pal[i][1]; bl += pal[i][2]; n += solid[i]
            out.append((r / 4, g / 4, bl / 4, n))
    return out


def _alpha_quarters(ab):
    # DXT3's explicit alpha: 4 bits a pixel, averaged the same way
    rows = struct.unpack_from("<4H", ab, 0)
    v = [(rows[y] >> (4 * x)) & 15 for y in range(4) for x in range(4)]
    return [(v[qy * 4 + qx] + v[qy * 4 + qx + 1] + v[(qy + 1) * 4 + qx] + v[(qy + 1) * 4 + qx + 1]) // 4
            for qy in (0, 2) for qx in (0, 2)]


def shrink_dxt(px, w, h, fmt):
    # half across and down, a real average: each source block gives four pixels of the destination
    bs = dxt_block_size(fmt)
    ow, oh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    nw, nh = max(4, w // 2), max(4, h // 2)
    bw, bh = (nw + 3) // 4, (nh + 3) // 4
    out = bytearray(bw * bh * bs)
    cache = {}
    for y in range(bh):
        for x in range(bw):
            cols = [None] * 16
            alpha = [0] * 16
            for sy in range(2):
                for sx in range(2):
                    gy, gx = min(y * 2 + sy, oh - 1), min(x * 2 + sx, ow - 1)
                    b = px[(gy * ow + gx) * bs:(gy * ow + gx + 1) * bs]
                    key = b[bs - 8:]
                    q = cache.get(key)
                    if q is None:
                        q = cache[key] = _block_quarters(key, bs == 16)
                    aq = _alpha_quarters(b[:8]) if bs == 16 else None
                    for i in range(4):
                        at = (sy * 2 + (i >> 1)) * 4 + sx * 2 + (i & 1)
                        cols[at] = q[i]
                        if aq:
                            alpha[at] = aq[i]
            at = (y * bw + x) * bs
            if bs == 16:
                rows = [alpha[r * 4] | alpha[r * 4 + 1] << 4 | alpha[r * 4 + 2] << 8 | alpha[r * 4 + 3] << 12 for r in range(4)]
                struct.pack_into("<4H", out, at, *rows)
            cb = at + bs - 8
            # the ends along the block's brightness; pixels mostly transparent stay so in DXT1
            seen = [c for c in cols if c[3] >= 2 or bs == 16]
            clear = bs == 8 and len(seen) < 16
            if not seen:
                struct.pack_into("<HHI", out, cb, 0, 0, 0xFFFFFFFF)
                continue
            hi = max(seen, key=lambda c: c[0] + c[1] + c[2])
            lo = min(seen, key=lambda c: c[0] + c[1] + c[2])
            c_hi, c_lo = _to565(*hi[:3]), _to565(*lo[:3])
            if clear:
                # three colours and transparent: the smaller end first
                c0, c1 = min(c_hi, c_lo), max(c_hi, c_lo)
                a, b = _565(c0), _565(c1)
                pal = (a, b, tuple((p + q) / 2 for p, q in zip(a, b)))
            else:
                if c_hi < c_lo:
                    c_hi, c_lo = c_lo, c_hi
                if c_hi == c_lo:
                    struct.pack_into("<HHI", out, cb, c_hi, c_lo, 0)
                    continue
                c0, c1 = c_hi, c_lo
                a, b = _565(c0), _565(c1)
                pal = (a, b, tuple((2 * p + q) / 3 for p, q in zip(a, b)), tuple((p + 2 * q) / 3 for p, q in zip(a, b)))
            bits = 0
            for i, c in enumerate(cols):
                if clear and c[3] < 2:
                    v = 3
                else:
                    v = min(range(len(pal)), key=lambda k: sum((c[j] - pal[k][j]) ** 2 for j in range(3)))
                bits |= v << (2 * i)
            struct.pack_into("<HHI", out, cb, c0, c1, bits)
    return bytes(out), nw, nh


class Texture:
    pass


def read_mod_textures(mt):
    recs = data = None
    for c, s, o in walk_all(mt):
        if c == 0x33310003:
            recs = [struct.unpack_from("<6I", mt, o + i) for i in range(0, s, 24)]
    out = []
    for h, off, cs, us, fl, _ in recs:
        if struct.unpack_from("<I", mt, off)[0] == CARBON_BLOCK:
            d = unpack_carbon(mt[off:off + cs], us)
        else:
            d = unpack(mt[off:off + cs], us)
        at = d.rfind(struct.pack("<I", h))
        if at < 12:
            raise ValueError("a texture has no record")
        # CARBON: the record is 188 bytes at the blob's end, the hash 12 bytes into it: the name 77 bytes past
        # the hash (its length byte just before it), the size 28 past it, the NV2A format 128 past it
        t = Texture()
        t.hash = h
        t.name = d[at + 77:].split(b"\0")[0].decode("latin-1")
        t.w, t.h = struct.unpack_from("<HH", d, at + 28)
        fmt = d[at + 128]
        t.fmt = {0x0C: b"DXT1", 0x0E: b"DXT3", 0x0F: b"DXT5"}.get(fmt)
        if t.fmt is None:
            continue  # a format no Xbox record is copied for: one texture less, not one car less
        base = ((t.w + 3) // 4) * ((t.h + 3) // 4) * dxt_block_size(t.fmt)
        t.px = d[:base]  # the first level alone: the Xbox's car textures have one
        out.append(t)
    return out


def shrink_textures(tex, limit):
    total = lambda: sum(len(t.px) for t in tex)
    before = total()
    while total() > limit:
        big = max(tex, key=lambda t: len(t.px))
        if big.w <= 32 or big.h <= 32:
            break
        big.px, big.w, big.h = shrink_dxt(big.px, big.w, big.h, big.fmt)
    print("textures: %d, %d KB, to %d KB" % (len(tex), before // 1024, total() // 1024))


def ilog2(n):
    r = 0
    while (1 << r) < n:
        r += 1
    return r


def template_records(xt):
    # the Xbox's info record of a DXT1 and of a DXT3 texture, whose fields the mod's are filled over. CARBON's is
    # 188 bytes, the NV2A format at +140 from its start (+128 from its hash, which is at +12)
    recs = None
    for c, s, o in walk_all(xt):
        if c == 0x33310003:
            recs = [struct.unpack_from("<6I", xt, o + i) for i in range(0, s, 24)]
    tpl = {}
    for h, off, cs, us, fl, _ in recs:
        if struct.unpack_from("<I", xt, off)[0] == CARBON_BLOCK:
            try:
                d = unpack_carbon(xt[off:off + cs], us)
            except ValueError:
                continue  # HUFF: no decoder, and the other textures may still give both records
        elif xt[off:off + 4] == b"JDLZ":
            d = jdlz_decode(xt[off:off + cs])
        else:
            continue
        if len(d) < 188:
            continue
        rec = d[-188:]
        if struct.unpack_from("<II", rec, 128) != (0x302, 0x303):
            continue
        for code, name in ((0x0C, b"DXT1"), (0x0E, b"DXT3")):
            if name not in tpl and rec[140] == code:
                tpl[name] = rec
    if b"DXT1" not in tpl or b"DXT3" not in tpl:
        raise ValueError("no Xbox texture record to copy from")
    # DXT5 is the DXT3 record with the NV2A's own format for it
    five = bytearray(tpl[b"DXT3"])
    five[140] = 0x0F
    tpl[b"DXT5"] = bytes(five)
    return tpl


def build_textures(tex, xt, car):
    tpl = template_records(xt)
    t_info = bytearray(next(xt[o:o + s] for c, s, o in walk_all(xt) if c == 0x33310001))
    t_head = bytearray(next(xt[o:o + s] for c, s, o in walk_all(xt) if c == 0x33320001))
    # the pack's name, its hash in both places. CARBON hashes the UPPER-CASE path: bhash of the mixed-case path
    # is 1EA885F6 for the Gallardo, the disc's is EF2B4A16, the upper-case hash
    path = "Global\\Pipeline\\CarTemplateTextures_%s.tpk" % car
    t_info[0x20:0x60] = path.encode("latin-1").ljust(0x40, b"\0")
    t_info[4:4 + 28] = ("CARTEMPLATETEXTURES_" + car).upper().encode("latin-1")[:28].ljust(28, b"\0")
    struct.pack_into("<I", t_info, 0x60, bhash(path.upper()))
    struct.pack_into("<I", t_head, 12, bhash(path.upper()))
    blocks = []
    for t in sorted(tex, key=lambda x: x.hash):
        rec = bytearray(tpl[t.fmt])
        nm = t.name.encode("latin-1")
        # CARBON's record (offsets from its start; from the hash at +12 they are +77, +76, +16, +24, +28, +30,
        # +32, +33): the name at +89, its length byte at +88 (align4(len+2)-1, inferred from 3 samples); the hash
        # at +12, the pixel size at +28 and +36, the size at +40/+42, the log2 sizes at +44/+45. The rest (+16,
        # +20, +46, +53, +56..+58, +124, from the hash +4, +8, +34, +41, +44..+46, +112) is the template's own
        rec[88:124] = b"\0" * 36
        rec[88] = align(len(nm) + 2, 4) - 1
        rec[89:89 + min(len(nm), 34)] = nm[:34]
        struct.pack_into("<I", rec, 12, t.hash)
        n = len(t.px)
        struct.pack_into("<I", rec, 28, n)
        struct.pack_into("<I", rec, 36, n)
        struct.pack_into("<HHBB", rec, 40, t.w, t.h, ilog2(t.w), ilog2(t.h))
        blocks.append((t.hash, t.px + bytes(rec)))  # CARBON: no padding between the pixels and the record
    n = len(blocks)
    hashes = b"".join(struct.pack("<II", h, 0) for h, _ in blocks)
    t_info, t_head = bytes(t_info), bytes(t_head)
    # the table's offsets count from the file's start, so the head is laid out first; its size does not depend on them
    def layout(table):
        head = chunk(0x33310001, t_info) + chunk(0x33310002, hashes) + chunk(0x33310003, table)
        return chunk(0, b"\0" * 48) + chunk(0xB3310000, head) + chunk(0, b"\0" * 124), chunk(0x33320001, t_head) + chunk(0, b"\0" * 80)
    prefix, head2 = layout(b"\0" * (n * 24))
    data_chunk = 8 + len(prefix) + 8 + len(head2) + 8  # b3300000, then b3320000, then 33320002's data
    data_at = align(data_chunk, 0x80)
    data = bytearray(b"\x11" * (data_at - data_chunk))
    rows = []
    for h, block in blocks:
        z = carbon_block(block)
        rows.append(struct.pack("<6I", h, data_chunk + len(data), len(z), len(block), 0x200, 0))
        data += z
        data += b"\0" * (align(data_chunk + len(data), 0x80) - data_chunk - len(data))
    prefix, head2 = layout(b"".join(rows))
    return chunk(0xB3300000, prefix + chunk(0xB3320000, head2 + chunk(0x33320002, bytes(data))))


# the vault (GLOBAL\ATTRIBUTES.BIN)

CLASS_ROW = 0x5E970CBC
COLL_ROW = 0x8E112EB7
TY_STRINGKEY = 0xA502A824


class Vault:
    def __init__(self, b):
        self.name_at = struct.unpack_from("<I", b, 8)[0]
        self.head = b[:0x80]
        binsz, vltsz, binoff, vltoff = struct.unpack_from("<IIII", b, 0x14)
        self.bin = bytearray(b[binoff:binoff + binsz])
        v = b[vltoff:vltoff + vltsz]
        self.dep = self.strn = self.dat = self.exp = self.ptr = None
        off = 0
        self.order = []
        while off + 8 <= len(v):
            cid, s = struct.unpack_from("<4sI", v, off)
            if s < 8:
                break
            self.order.append((cid, v[off:off + s]))
            off += s
        ch = dict(self.order)
        self.vlt = bytearray(v)
        self.dat_at = sum(len(c) for i, c in self.order[:[x[0] for x in self.order].index(b"NtaD")])
        self.dat_size = len(ch[b"NtaD"])
        n = struct.unpack_from("<I", ch[b"NpxE"], 8)[0]
        self.rows = []
        for i in range(n):  # CARBON: the export rows are 16 bytes [key, type, ?, offset], not MW's 20
            r = list(struct.unpack_from("<4I", ch[b"NpxE"], 12 + i * 16))
            r.append(r[3])  # the offset, kept where the rest of the code reads it
            self.rows.append(r)
        pt = ch[b"NrtP"]
        self.ptrs = [struct.unpack_from("<IHHI", pt, 8 + i * 12) for i in range((len(pt) - 8) // 12)]
        self.fix = {0: {}, 1: {}}
        cur = None
        for p in self.ptrs:
            if p[1] == 2:
                cur = p[2]
            elif p[1] == 0:
                break
            else:
                self.fix[cur][p[0]] = p
        self.type_names = {}
        for t in re.findall(rb"[A-Za-z_]\w*::[\w:]+", bytes(self.bin)):
            self.type_names[attrib_key(t.decode())] = t.decode()
        self.data_end = self.dat_at + self.dat_size
        self.bin_fix = dict(self.fix[1])
        self.new_fix = {0: [], 1: []}
        self.new_vlt = bytearray()
        self.new_rows = []
        self.classes = {}
        self.aligns = {}
        self.colls = {}
        self.names = {}
        for r in self.rows:
            if r[1] == CLASS_ROW:
                k, res, nf = struct.unpack_from("<III", self.vlt, r[4])
                layout = struct.unpack_from("<I", self.vlt, r[4] + 24)[0]  # CARBON: the layout size is at +24
                f = self.fix[0][r[4] + 12][3]
                fields = {}
                for i in range(nf):
                    key_, ty, lo, sz, cnt, fl, al = struct.unpack_from("<IIHHHBB", self.bin, f + i * 16)
                    fields[key_] = (ty, lo, sz, cnt, fl)
                    self.aligns[(k, key_)] = 1 << al
                self.classes[k] = (layout, fields)
            elif r[1] == COLL_ROW:
                k, cl = struct.unpack_from("<II", self.vlt, r[4])
                self.colls[(cl, k)] = r[4]

    def clone(self, cls, src, dst, parent=None):
        c, k = attrib_key(cls), attrib_key(src)
        at = self.colls[(c, k)]
        key_, cl, par, res, sh, ne, nt_, lp = struct.unpack("<8I", self._read(at, 32))
        nt = nt_ & 0xFFFF
        layout, fields = self.classes[c]
        size = 32 + nt * 4 + ne * 12
        rec = bytearray(self._read(at, size))
        struct.pack_into("<I", rec, 0, attrib_key(dst))
        if parent is not None:
            struct.pack_into("<I", rec, 8, attrib_key(parent))
        self.new_vlt += b"\0" * (align(len(self.new_vlt), 8) - len(self.new_vlt))
        new_at = self.data_end + len(self.new_vlt)
        # the layout and each value out of the bin, with their null and string pointers
        def copy(off, n):
            base = align(len(self.bin), 16)
            self.bin += b"\0" * (base - len(self.bin))
            self.bin += bytes(self.bin[off:off + n])
            for q in range(off, off + n, 4):
                f_ = self.bin_fix.get(q)
                if f_:
                    moved = (base + q - off, f_[1], f_[2], f_[3])
                    self.new_fix[1].append(moved)
                    self.bin_fix[moved[0]] = moved
            return base
        lf = self._all_fix(at + 0x1C)
        if lf:
            nl = copy(lf[3], layout)
            self.new_fix[0].append((new_at + 0x1C, lf[1], lf[2], nl))
        ents_at = 32 + nt * 4
        for e in range(ne):
            fk, val, ti, fl = struct.unpack_from("<IIHH", rec, ents_at + e * 12)
            pf = self._all_fix(at + ents_at + e * 12 + 4)
            if not pf or fl & 0x20:
                continue
            nv = copy(pf[3], self.value_size(pf[3], fields.get(fk), self.aligns.get((c, fk), 4)))
            self.new_fix[0].append((new_at + ents_at + e * 12 + 4, pf[1], pf[2], nv))
        self.new_vlt += rec
        self.new_rows.append([attrib_key("%08x/%s" % (c, dst)), COLL_ROW, 0, new_at, new_at])
        self.colls[(c, attrib_key(dst))] = new_at
        return new_at

    def value_size(self, off, d, al):
        ty, lo, sz, cnt, fl = d
        if fl & 1:
            n, cap, el = struct.unpack_from("<HHH", self.bin, off)
            return align(8, al) + cap * el
        return sz

    def _rec(self, cls, node):
        at = self.colls[(attrib_key(cls), attrib_key(node))]
        return at

    def _read(self, off, n):
        if off >= self.data_end:
            return bytes(self.new_vlt[off - self.data_end:off - self.data_end + n])
        return bytes(self.vlt[off:off + n])

    def _write(self, off, b):
        if off >= self.data_end:
            self.new_vlt[off - self.data_end:off - self.data_end + len(b)] = b
        else:
            self.vlt[off:off + len(b)] = b

    def _all_fix(self, off):
        for f in self.new_fix[0]:
            if f[0] == off:
                return f
        return self.fix[0].get(off)

    def field_place(self, cls, node, field, index):
        # where a field's value lives in the bin, in this node; None when only its parent has it
        c = attrib_key(cls)
        layout, fields = self.classes[c]
        fk = attrib_key(field)
        d = fields.get(fk)
        if d is None:
            return None, None
        ty, lo, sz, cnt, fl = d
        at = self._rec(cls, node)
        ne, nt = struct.unpack("<II", self._read(at + 20, 8))
        nt &= 0xFFFF
        ents = at + 32 + nt * 4
        if fl & 2:
            f = self._all_fix(at + 0x1C)
            if not f:
                return None, d
            if not fl & 1:
                return f[3] + lo, d
            # an array in the layout has its count, room and element size there too
            base = f[3] + lo
            count, cap, el = struct.unpack_from("<HHH", self.bin, base)
            if index >= cap:
                return None, d
            if index >= count:
                struct.pack_into("<H", self.bin, base, index + 1)
            return base + align(8, self.aligns.get((c, fk), 4)) + index * el, d
        for e in range(ne):
            k_, v, ti, efl = struct.unpack("<IIHH", self._read(ents + e * 12, 12))
            if k_ != fk:
                continue
            if efl & 0x20:
                return ("inline", ents + e * 12 + 4), d
            f = self._all_fix(ents + e * 12 + 4)
            base = f[3]
            if fl & 1:
                count, cap, el = struct.unpack_from("<HHH", self.bin, base)
                if index >= cap:
                    return None, d
                if index >= count:
                    struct.pack_into("<H", self.bin, base, index + 1)
                return base + align(8, self.aligns.get((c, fk), 4)) + index * el, d
            return base, d
        return None, d

    def set_value(self, cls, node, field, index, member, text):
        place, d = self.field_place(cls, node, field, index)
        if d is None:
            return "no such field"
        if place is None:
            return "not in this node"
        ty, lo, sz, cnt, fl = d
        at = MEMBERS.get(member, 0) if member else 0
        if member and member not in MEMBERS:
            return "unknown member " + member
        if member in ("Collection", "Class"):
            raw = struct.pack("<I", attrib_key(text))
        elif member:
            raw = pack_float(text)
        else:
            raw = pack_scalar(text, self.type_names.get(ty, ""), sz)
        if raw is None:
            return "value %s of %s" % (text, self.type_names.get(ty, hex(ty)))
        if isinstance(place, tuple):
            self._write(place[1] + at, raw)
        else:
            self.bin[place + at:place + at + len(raw)] = raw
        return None

    def set_model(self, node, name):
        # CARBON: MODEL is 8 bytes, the name's attribKey alone (the Gallardo's is attribKey("GALLARDO")). MW's
        # 12-byte StringKey would run into the next field, at +36
        place, d = self.field_place("pvehicle", node, "MODEL", 0)
        struct.pack_into("<I", self.bin, place, attrib_key(name))
        struct.pack_into("<I", self.bin, place + 4, 0)

    # CARBON: a TireOffsets element is (x, y, z, radius) in the world's axes (X = length, Y = width, Z = height,
    # proven from the BODY bounds), exactly as in MW: the Gallardo's tire0 is (1.16, 0.93, 0.0, 0.325) - the axle
    # at +0 (+1.16 front, -1.39 rear, a 2.55 wheelbase), the lateral at +4 (+/-0.93, the track half-width), the
    # height 0 at +8, and the tyre radius 0.325 at +12 (the geometric radius). So MW's set_wheels is right as-is:
    # x at +0, y at +4 (the template's sign kept), the radius at +12. The skid width is the tyre's, as in MW.
    def set_wheels(self, node, w):
        for i in range(4):
            at, _ = self.field_place("ecar", node, "TireOffsets", i)
            if not isinstance(at, int):
                continue
            x_, y_ = (w["front_x"], w["front_y"]) if i < 2 else (w["rear_x"], w["rear_y"])
            y = struct.unpack_from("<f", self.bin, at + 4)[0]
            struct.pack_into("<ff", self.bin, at, x_, -y_ if y < 0 else y_)
            struct.pack_into("<f", self.bin, at + 12, w["radius"])
            skid, _ = self.field_place("ecar", node, "TireSkidWidth", i)
            if isinstance(skid, int):
                struct.pack_into("<f", self.bin, skid, w["width"])

    def set_layout_text(self, cls, node, offset, text):
        self._set_text(self._all_fix(self._rec(cls, node) + 0x1C)[3] + offset, text)

    # the frontend's make, a byte (0x134250's table, 0 none): it, not the car type's, names the car's CARNAME_%s_%s
    # (0x140C60) and its logo. A make the game has not keeps the node's, whose name is the one returned
    def set_make(self, node, manufacturer):
        at = self._all_fix(self._rec("frontend", node) + 0x1C)[3] + MAKE_OFFSET
        make = make_index(manufacturer)
        if make <= 0:
            make = self.bin[at]
        if make <= 0 or make >= len(MAKES):
            make = make_index("LAMBORGHINI")
        self.bin[at] = make
        return MAKES[make]

    def _set_text(self, ptr, text):
        at = len(self.bin)
        self.bin += text.encode("latin-1") + b"\0"
        self.new_fix[1] = [f for f in self.new_fix[1] if f[0] != ptr] + [(ptr, 3, 1, at)]
        self.bin_fix[ptr] = (ptr, 3, 1, at)

    def write(self):
        # the bin's end moves, the fixups' groups are 1 for the bin, 0 for the vlt, in that order
        v = bytearray()
        for cid, _c in self.order:
            if cid == b"NtaD":
                body = bytes(self.vlt[self.dat_at + 8:self.dat_at + self.dat_size]) + bytes(self.new_vlt)
                n = align(8 + len(body), 16)
                v += struct.pack("<4sI", cid, n) + body + b"\0" * (n - 8 - len(body))
            elif cid == b"NpxE":
                rows = self.rows + self.new_rows
                body = struct.pack("<I", len(rows)) + b"".join(struct.pack("<4I", *r[:4]) for r in rows)  # CARBON: 16-byte rows
                pad = bytes(_c[12 + len(self.rows) * 16:])
                n = align(8 + len(body) + len(pad), 16)
                v += struct.pack("<4sI", cid, n) + body + pad + b"\0" * (n - 8 - len(body) - len(pad))
            elif cid == b"NrtP":
                out = bytearray()
                cur = None
                for p in self.ptrs:
                    if p[1] == 2:
                        if cur is not None:
                            for f in self.new_fix[cur]:
                                out += struct.pack("<IHHI", *f)
                        cur = p[2]
                    elif p[1] == 0:
                        if cur is not None:  # CARBON: the stream ends with two (0, 0, 0, 0) records in a row
                            for f in self.new_fix[cur]:
                                out += struct.pack("<IHHI", *f)
                        cur = None
                    out += struct.pack("<IHHI", *p)
                n = align(8 + len(out), 16)
                v += struct.pack("<4sI", cid, n) + out + b"\0" * (n - 8 - len(out))
            else:
                v += self.vlt[sum(len(c) for i, c in self.order[:[x[0] for x in self.order].index(cid)]):][:len(_c)]
        b = bytes(self.bin)
        vlt_off = align(0x80 + len(b), 0x80)
        head = bytearray(self.head)
        struct.pack_into("<IIII", head, 0x14, len(b), len(v), 0x80, vlt_off)
        out = bytes(head) + b + b"\0" * (vlt_off - 0x80 - len(b)) + bytes(v)
        return out + b"\0" * (align(len(out), 0x80) - len(out))


MEMBERS = {
    "Front": 0, "Rear": 4,
    "X": 0, "Y": 4, "Z": 8, "W": 12,
    "DegPerG": 0, "DegPerSec": 4, "MaxG": 8,
    "Class": 0, "Collection": 4,
}


def pack_float(text):
    try:
        return struct.pack("<f", float(text))
    except ValueError:
        return None


def pack_scalar(text, type_name, size):
    try:
        if "Float" in type_name:
            return struct.pack("<f", float(text))
        if "Bool" in type_name:
            return bytes([text.lower() in ("true", "1")])
        if "Int" in type_name or "Enum" in type_name or text.lower().startswith("0x"):
            return (int(text, 0) & ((1 << size * 8) - 1)).to_bytes(size, "little")
    except ValueError:
        pass
    return None


def string_key64(name):  # Jenkins' lookup8 (0x047740)
    k = name.encode("latin-1")
    M = 0xFFFFFFFFFFFFFFFF
    a = b = 0xABCDEF0011223344
    c = 0x9E3779B97F4A7C13
    def mix(a, b, c):
        a = (a - b - c) & M; a ^= c >> 43
        b = (b - c - a) & M; b ^= (a << 9) & M
        c = (c - a - b) & M; c ^= b >> 8
        a = (a - b - c) & M; a ^= c >> 38
        b = (b - c - a) & M; b ^= (a << 23) & M
        c = (c - a - b) & M; c ^= b >> 5
        a = (a - b - c) & M; a ^= c >> 35
        b = (b - c - a) & M; b ^= (a << 49) & M
        c = (c - a - b) & M; c ^= b >> 11
        a = (a - b - c) & M; a ^= c >> 12
        b = (b - c - a) & M; b ^= (a << 18) & M
        c = (c - a - b) & M; c ^= b >> 22
        return a, b, c
    i = 0
    while len(k) - i >= 24:
        a = (a + int.from_bytes(k[i:i + 8], "little")) & M
        b = (b + int.from_bytes(k[i + 8:i + 16], "little")) & M
        c = (c + int.from_bytes(k[i + 16:i + 24], "little")) & M
        a, b, c = mix(a, b, c)
        i += 24
    c = (c + len(k)) & M
    r = k[i:]
    for j in range(len(r) - 1, -1, -1):
        if j >= 16: c = (c + (r[j] << (8 * (j - 15)))) & M
        elif j >= 8: b = (b + (r[j] << (8 * (j - 8)))) & M
        else: a = (a + (r[j] << (8 * j))) & M
    return mix(a, b, c)[2]


def apply_script(main, fe, path):
    # VltEd's script, the parts of it the Xbox's database holds: nodes and their values. FE_ATTRIB holds the frontend
    # class's nodes, whose definition is in ATTRIBUTES
    skipped = collections.Counter()
    done = 0
    for line in open(path, encoding="latin-1"):
        w = line.split()
        if not w:
            continue
        op, cls = w[0], w[1] if len(w) > 1 else ""
        if cls in VLT_CLASSES_SKIPPED:
            continue
        v = fe if cls == "frontend" else main
        if op == "copy_node":
            src, parent, dst = w[2:5]
            if (attrib_key(cls), attrib_key(src)) not in v.colls:
                skipped["copy %s %s: source missing" % (cls, src)] += 1
                continue
            v.clone(cls, src, dst, parent)
            done += 1
        elif op == "update_field":
            node, field = w[2], w[3]
            m = re.fullmatch(r"(\w+)(?:\[(\d+)\])?", field)
            name, index = m.group(1), int(m.group(2) or 0)
            member = w[4] if len(w) > 5 else None
            text = w[-1]
            if (attrib_key(cls), attrib_key(node)) not in v.colls:
                skipped["update of %s %s" % (cls, node)] += 1
                continue
            if name == "MODEL":
                v.set_model(node, text)
                continue
            why = v.set_value(cls, node, name, index, member, text)
            if why:
                skipped["%s.%s: %s" % (cls, name, why)] += 1
            else:
                done += 1
        elif op in ("add_field", "resize_field"):
            skipped[op + " " + cls + "." + w[3]] += 1
    print("vault: %d script lines applied" % done)
    for k, n in sorted(skipped.items()):
        print("  skipped %s (%d)" % (k, n))


# GLOBAL\GLOBALB: the car's record, its parts' table entry and collision, all in place: the size stays and with it the
# alignment of the textures after them

def car_names(g):
    for c, s, o in chunks(g):
        if c == 0x34600:
            return [bytes(g[o + 8 + i * 0xD0:o + 8 + i * 0xD0 + 16]).split(b"\0")[0].decode() for i in range(s // 0xD0)]  # CARBON: 95 rows
    sys.exit("no car table in the game's files")


# CARBON: the frontend layout's make byte is at +0x6C, not MW's +0x40. The names are by the game's make table:
# 11 is MCLAREN (the SLR's CARNAME is MCLAREN_SLR) and 12 is MERCEDES, as in MW's list. The Carbon-only makes
# follow: 27 PLYMOUTH, 28 PAGANI, 29 CHRYSLER, 30 ALFA_ROMEO, 31 KOENIGSEGG, 32 INFINITI. 26 is unknown (no Carbon
# car with a frontend node uses it; see CarConverter.java). The spellings are the CARNAME strings' (CHEVROLET, not
# the car table's CHEVEROLET typo; ALFA_ROMEO; ASTON_MARTIN).
MAKE_OFFSET = 0x6C
MAKES = ("", "BMW", "FORD", "SUBARU", "PORSCHE", "AUDI", "MAZDA", "GENERAL_MOTORS", "DODGE", "TOYOTA",
         "MITSUBISHI", "MCLAREN", "MERCEDES", "NISSAN", "LOTUS", "LAMBORGHINI", "RENAULT", "LEXUS", "PONTIAC", "CHEVROLET", "VAUXHALL",
         "ASTON_MARTIN", "VOLKSWAGEN", "FIAT", "CADILLAC", "CORVETTE", "", "PLYMOUTH", "PAGANI", "CHRYSLER", "ALFA_ROMEO", "KOENIGSEGG", "INFINITI")


# a mod's make may be spaced (ASTON MARTIN, GENERAL MOTORS), joined (ASTONMARTIN, ALFAROMEO), or spelled as the
# car table spells it (CHEVEROLET, HOLDEN): the CARNAME make is CHEVROLET and VAUXHALL for those two.
def make_index(manufacturer):
    m = manufacturer.upper()
    if m == "CHEVEROLET":
        m = "CHEVROLET"
    if m == "HOLDEN":
        m = "VAUXHALL"
    norm = m.replace(" ", "").replace("_", "")
    for i, name in enumerate(MAKES):
        if name and name.replace("_", "") == norm:
            return i
    return -1


def patch_global(g, car, manufacturer):
    # CARBON: appends a "no car" row (type 95) and the add-on's row (type 96) to the 95-row car table, appends the
    # add-on's hash to the parts' car table (0x3460B), and appends a collision of the Gallardo's. No parts rows are
    # copied: Carbon splits MW's 14-byte parts rows into four tables (0x34604 is 20734 x 4 bytes, a car index and a
    # part reference; 0x34605, 0x3460C and 0x3460A are unmapped), and no template car's parts can be copied until
    # they are mapped. The add-on therefore has no parts rows: it drives, and has no body kits or wheels to fit.
    # Inserts go from the file's end to its start, so the offsets stay good.
    g = bytearray(g)
    sub = {}
    for c, s, o in chunks(g):
        if c == 0x34600:
            types = (s, o)
        elif c == 0x80034602:
            sub = {c2: (s2, o2) for c2, s2, o2 in chunks(g, o, o + s)}
            parts_chunk = (s, o)
        elif c == 0x8003B900:
            collisions = (s, o)
    names = car_names(g)
    gallardo = names.index("GALLARDO")
    if 0x3460B not in sub or 0x34603 not in sub:
        sys.exit("not the game's car tables")
    # a collision of its own, the Gallardo's (the 0x3B901 item's payload starts with the car's attribKey)
    s, o = collisions
    item = None
    for c2, s2, o2 in chunks(g, o, o + s):
        p = payload(g, o2, s2)
        if struct.unpack_from("<I", g, p)[0] == attrib_key("GALLARDO"):
            item = bytes(g[p:o2 + s2])
            cid = c2
    if item is None:
        sys.exit("no Gallardo collision to copy")
    end = o + s
    fill = (16 - (end + 8) % 16) % 16
    body = bytearray(b"\x11" * fill + item)
    struct.pack_into("<I", body, fill, attrib_key(car))
    grow = align(8 + len(body), 0x80)
    body += b"\0" * (grow - 8 - len(body))
    g[end:end] = struct.pack("<II", cid, grow - 8) + body
    struct.pack_into("<I", g, o - 4, s + grow)
    # the parts' car table (0x3460B, 122 hashes, sorted): the add-on's hash after the game's, so a lookup finds it
    (sb, ob), (sh, oh) = sub[0x3460B], sub[0x34603]
    car_count = sb // 4
    g[ob + sb:ob + sb] = struct.pack("<I", bhash(car))
    struct.pack_into("<I", g, ob - 4, sb + 4)
    struct.pack_into("<I", g, parts_chunk[1] - 4, parts_chunk[0] + 4)
    struct.pack_into("<I", g, oh + 0x28, car_count + 1)
    print("parts: 1 add-on with no parts rows (Carbon's parts tables are not yet mapped)")
    # the car types: an empty one for 95, the "no car" answer, then the add-on
    ts, to = types
    cars = to + 8
    rec = bytearray(g[cars + gallardo * 0xD0:cars + (gallardo + 1) * 0xD0])
    for at, n, txt in ((0, 16, car), (0x10, 16, car), (0x20, 32, "CARS\\%s\\GEOMETRY.BIN" % car), (0x40, 16, manufacturer)):
        rec[at:at + n] = txt.encode("latin-1").ljust(n, b"\0")
    struct.pack_into("<I", rec, 0x50, bhash(car))
    struct.pack_into("<I", rec, 0x90, FIRST_ADDON_TYPE)
    none = bytearray(0xD0)
    struct.pack_into("<I", none, 0x90, STOCK_TYPES)
    new = bytes(none) + bytes(rec)
    new += b"\0" * (align(len(new), 0x80) - len(new))
    g[to + ts:to + ts] = new
    struct.pack_into("<I", g, to - 4, ts + len(new))
    return bytes(g)


# the disc's file list

def build_zdir(disc, files, alias):
    arcs = disc.nfs
    last = max(r[1] for r in disc.recs.values())
    if arcs["ZZDATA%d.BIN" % last][1] != 0:
        last += 1
    archive = "ZZDATA%d.BIN" % last
    # sectors count across the archives in their numbering
    sizes = [(arcs["ZZDATA%d.BIN" % i][1] + 2047) // 2048 for i in range(last)]
    abs_base = max((r[3] + (r[4] + 2047) // 2048 for r in disc.recs.values() if r[4]), default=0)
    recs = dict(disc.recs)
    data = bytearray()
    for path, b in files.items():
        h = bhash(path)
        sector = len(data) // 2048
        recs[h] = (h, last, sector, abs_base + sector, len(b), 0xFFFFFFFF)
        data += b + b"\0" * (align(len(b), 2048) - len(b))
    for path, of in alias.items():
        r = disc.recs[bhash(of)]
        recs[bhash(path)] = (bhash(path),) + r[1:]
    z = b"".join(struct.pack("<6I", *recs[k]) for k in sorted(recs))
    return archive, z, bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("mod")
    ap.add_argument("out")
    ap.add_argument("--no-vault-script", action="store_true", help="the car's own handling and wheel places left out")
    ap.add_argument("--max-textures", type=int, default=1536, help="KB of all the car's textures; the disc's have about 100")
    args = ap.parse_args()

    disc = Disc(args.iso)
    root = args.mod
    geo = glob.glob(os.path.join(glob.escape(root), "**", "CARS", "*", "GEOMETRY.BIN"), recursive=True)
    geo += glob.glob(os.path.join(glob.escape(root), "**", "Car", "*", "GEOMETRY.BIN"), recursive=True)  # a pack's installers
    geo += glob.glob(os.path.join(glob.escape(root), "**", "CARS_REPLACE", "*", "GEOMETRY.BIN"), recursive=True)
    if not geo:
        # some repacks have the car's folder loose beside their .end scripts, no CARS above it (EVO8 U2\LANCEREVO8)
        geo = sorted(p for p in glob.glob(os.path.join(glob.escape(root), "**", "GEOMETRY.BIN"), recursive=True)
                     if os.path.exists(os.path.join(os.path.dirname(p), "TEXTURES.BIN")))
    if not geo:
        sys.exit("no <name>\\GEOMETRY.BIN with a TEXTURES.BIN beside it under " + root)
    car = os.path.basename(os.path.dirname(geo[0])).upper()
    if len(car) > 15 or len("CARS\\%s\\GEOMETRY.BIN" % car) > 31:
        sys.exit("%s: the name is too long for the car's record" % car)
    tex_path = os.path.join(os.path.dirname(geo[0]), "TEXTURES.BIN")
    mg, mt = open(geo[0], "rb").read(), open(tex_path, "rb").read()

    xg = disc.load("CARS\\GALLARDO\\GEOMETRY.BIN")
    solids = []
    for d in read_mod_solids(mg):
        s = convert_solid(d, car)
        if s:
            solids.append(s)
    print("geometry: %d solids" % len(solids))
    # thinned first, each attachment alone, so the body they go into stays under a solid's vertex count
    reduce_lods(solids)
    merge_attachments(solids)
    geometry = build_geometry(solids, xg)

    tex = read_mod_textures(mt)
    shrink_textures(tex, args.max_textures * 1024)
    textures = build_textures(tex, disc.load("CARS\\GALLARDO\\TEXTURES.BIN"), car)

    g = jdlz_decode(disc.load("GLOBAL\\GLOBALB.LZC"))
    if car in car_names(g):
        # named as one of the game's: its model and paint in that car's place, the rest stays the game's
        files = {"CARS\\%s\\GEOMETRY.BIN" % car: geometry, "CARS\\%s\\TEXTURES.BIN" % car: textures}
        archive, zdir, data = build_zdir(disc, files, {})
        nfs = os.path.join(args.out, "NFS")
        os.makedirs(nfs, exist_ok=True)
        open(os.path.join(nfs, "ZDIR.BIN"), "wb").write(zdir)
        open(os.path.join(nfs, archive), "wb").write(data)
        print("%s: replaced, %s %d KB" % (car, archive, len(data) // 1024))
        return

    manufacturer = "LAMBORGHINI"
    ci = glob.glob(os.path.join(glob.escape(root), "**", "CarTypeInfo*.bin"), recursive=True)
    if ci:
        raw = open(ci[0], "rb").read()
        m = re.search(rb"%s\0([A-Z0-9 ]+)\0" % car.encode(), raw)
        if m:
            manufacturer = m.group(1).decode()
    # the name from the strings an installer adds: English.end's first, then any other .end (a repack's own script).
    # With no CarTypeInfo, the string's make is the car's too
    title, found, other = car.title(), None, None
    ends = sorted(glob.glob(os.path.join(glob.escape(root), "**", "*.end"), recursive=True),
                  key=lambda p: (os.path.basename(p).lower() != "english.end", p))
    for p in ends:
        for m in re.finditer(r'CARNAME_([A-Z0-9_]+)_%s\s+"([^"]+)"' % re.escape(car), open(p, encoding="latin-1").read()):  # CARBON: makes like ALFA_ROMEO, ASTON_MARTIN
            if m.group(1) == manufacturer:
                found = m.groups()
                break
            other = other or m.groups()
        if found:
            break
    if not found and not ci:
        found = other
    if found:
        manufacturer, title = found

    g = patch_global(g, car, manufacturer)
    lzc = jdlz_encode(g)
    if jdlz_decode(lzc) != g:
        sys.exit("JDLZ encoder failed its round trip")

    v = Vault(disc.load("GLOBAL\\ATTRIBUTES.BIN"))
    fe = Vault(disc.load("GLOBAL\\FE_ATTRIB.BIN"))
    fe.classes, fe.type_names = v.classes, v.type_names
    node = car.lower()
    # the VltEd script for the car: named for it, else the install script, else the only one there is
    scripts = sorted(p for p in glob.glob(os.path.join(glob.escape(root), "**", "*.nfsms"), recursive=True)
                     if "uninstall" not in os.path.basename(p).lower())
    script = ([p for p in scripts if node in os.path.basename(p).lower()]
              or [p for p in scripts if "install" in os.path.basename(p).lower()]
              or (scripts if len(scripts) == 1 else []))[:1]
    if script and not args.no_vault_script:
        apply_script(v, fe, script[0])
    # no script, or one of another name for the car: the Gallardo's handling, the wheels where the model has them
    if (attrib_key("ecar"), attrib_key(node)) not in v.colls:
        v.clone("ecar", "gallardo", node)
        wheels = measure_wheels(solids)
        if wheels:
            v.set_wheels(node, wheels)
            print("axles at %.3f front, %.3f rear" % (wheels["front_x"], wheels["rear_x"]))
    if (attrib_key("pvehicle"), attrib_key(node)) not in v.colls:
        v.clone("pvehicle", "gallardo", node)
    if (attrib_key("frontend"), attrib_key(node)) not in fe.colls:
        fe.clone("frontend", "gallardo", node)
    # a copied pvehicle still names the Gallardo's: the car select then took this car for it
    v.set_value("pvehicle", node, "frontend", 0, "Collection", node)
    v.set_model(node, car)
    fe.set_layout_text("frontend", node, 0x18, node)  # CARBON: the disc holds the lower-case node name here
    make = fe.set_make(node, manufacturer)
    attributes = v.write()
    memory = rebuild_memory_file(disc.load("GLOBAL\\GLOBALMEMORYFILE.BIN"),
                                 {bhash("GLOBAL\\ATTRIBUTES.BIN"): attributes, bhash("GLOBAL\\GLOBALB.LZC"): lzc})

    files = {
        "GLOBAL\\GLOBALB.LZC": lzc,
        "GLOBAL\\GLOBALB.BUN": g,
        "GLOBAL\\ATTRIBUTES.BIN": attributes,
        "GLOBAL\\GLOBALMEMORYFILE.BIN": memory,
        "GLOBAL\\FE_ATTRIB.BIN": fe.write(),
        "CARS\\%s\\GEOMETRY.BIN" % car: geometry,
        "CARS\\%s\\TEXTURES.BIN" % car: textures,
    }
    alias = {"CARS\\%s\\VINYLS.BIN" % car: "CARS\\GALLARDO\\VINYLS.BIN"}  # CARBON has no PREVINYL.BIN
    archive, zdir, data = build_zdir(disc, files, alias)
    nfs = os.path.join(args.out, "NFS")
    os.makedirs(nfs, exist_ok=True)
    open(os.path.join(nfs, "ZDIR.BIN"), "wb").write(zdir)
    open(os.path.join(nfs, archive), "wb").write(data)
    # for src/game/car_mods.cpp: the profile's list and the car's name (title, above)
    # the car select's name is a texture in the front end's pack, SECONDARY_LOGO_%s; an add-on brings its own
    logo = glob.glob(os.path.join(glob.escape(root), "**", "SECONDARY_LOGO_%s.dds" % car), recursive=True)
    logo_info = "0 0 0 0"
    if logo:
        dds = open(logo[0], "rb").read()
        lh, lw = struct.unpack_from("<II", dds, 12)
        fourcc = dds[84:88]
        if fourcc in XBOX_FORMAT:
            # the name texture, by its hash (what src/game/car_mods.cpp reads first) and as car_name.dxt (its fallback)
            px = dds[128:128 + ((lw + 3) // 4) * ((lh + 3) // 4) * dxt_block_size(fourcc)]
            with open(os.path.join(args.out, "name_%08x.dxt" % bhash("SECONDARY_LOGO_" + car)), "wb") as f:
                f.write(px)
            with open(os.path.join(args.out, "car_name.dxt"), "wb") as f:
                f.write(px)
            logo_info = "%08x %d %d %d" % (bhash("SECONDARY_LOGO_" + car), lw, lh, XBOX_FORMAT[fourcc])
    with open(os.path.join(args.out, "cars.txt"), "w", encoding="latin-1", newline="\n") as f:
        f.write("%08x %08x %08x %s %s\n" % (attrib_key(node), attrib_key(node), bhash("CARNAME_%s_%s" % (make, car)), logo_info, title))
    with open(os.path.join(args.out, "car_types.txt"), "w", encoding="latin-1", newline="\n") as f:
        f.write("%d\n" % (FIRST_ADDON_TYPE + 1))
    print("%s: geometry %d KB, textures %d KB, %s %d KB" % (car, len(geometry) // 1024, len(textures) // 1024, archive, len(data) // 1024))


if __name__ == "__main__":
    main()
