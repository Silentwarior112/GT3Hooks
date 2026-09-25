#!/usr/bin/env python3
"""
gt3pack.py - unpack and repack Gran Turismo 3 / GT Concept `core.gt3` images,
and build a correct ELF from one.

The container (confirmed against retail CORE_SCUS-97102 and CORE_SCES-50294, and
against PDTools.GT4ElfBuilderTool's GTImageLoader.cs):

    u8    loadSourceFlags_lo        priority/index of the load source
    u8    loadSourceFlags_hi          in order: disk, mem card 1, mem card 2, host
    u32   rawSize                   inflated size
    ...   raw DEFLATE stream        no zlib/gzip wrapper (wbits = -15)

  inflated body:
    i16   rsaValue1Length           0x80 in retail GT3
    u8[]  rsaValue1                 the RSA modulus N, big-endian
    i16   rsaValue2Length           0x80 in retail GT3
    u8[]  rsaValue2                 the signature, big-endian
    --- hashStart: SHA-512 covers from here to the end of the body ---
    i32   nSection
    i32   entryPoint
      repeated nSection times:
        i32   targetOffset
        i32   size
        u8[]  data

The signature is textbook RSA with a per-build PUBLIC exponent:

    pow(SIG, e, N)  ->  low 512 bits, big-endian  ==  SHA-512(body[hashStart:])

Verified: CORE_SCUS-97102 with e=66001, CORE_SCES-50294 with e=67001.

Because the modulus travels in the file, a rebuilt image can be re-signed with a
freshly generated key of the same public exponent -- PROVIDED the game reads the
modulus from the file rather than from a baked-in constant. That has NOT been
established here; see FINDINGS.md. `repack --resign` does it anyway, so the
question can be settled by trying it.

GT4 Online layers Salsa20 encryption and a CRC32C tail on top of this. GT3 does
not (its first two bytes are 01 01), and this tool does not implement it.
"""

import argparse
import hashlib
import struct
import sys
import zlib

# Public exponents by build, from PDTools.GT4ElfBuilderTool/GTImageLoader.cs.
KNOWN_EXPONENTS = {
    65537: "GT3 (JP)",
    66001: "GT3 (US)",
    67001: "GT3 (EU)",
    69001: "GT Concept 2001 (JP)",
    71003: "GT Concept 2002 Tokyo-Geneva (EU)",
    72001: "GT Concept 2002 Tokyo-Geneva (EU)",
    77001: "GT4P (JP)",
    78001: "GT4P (AS)",
    79001: "GT4P (KR)",
    80001: "GT4P (EU)",
    81001: "GT4O (US)",
    82001: "GT4 (JP)",
    82101: "GT4 (US)",
    82201: "GT4 (EU)",
    82501: "GT4 Press Copy (CN)",
    83201: "GT4O (JP)",
    90001: "Tourist Trophy (JP)",
    90301: "Tourist Trophy (US)",
    90401: "Tourist Trophy (EU)",
}


class Core:
    def __init__(self):
        self.load_source_flags = 0x0101
        self.rsa_modulus = b""      # rsaValue1, big-endian
        self.rsa_signature = b""    # rsaValue2, big-endian
        self.entry_point = 0
        self.segments = []          # list of (target_offset, data)

    # ---------------------------------------------------------------- reading
    @classmethod
    def unpack(cls, raw):
        self = cls()
        if raw[0] != 1 and raw[1] != 1:
            raise NotImplementedError(
                "this image looks Salsa20-encrypted (GT4 Online); not supported")
        self.load_source_flags = raw[0] | (raw[1] << 8)
        raw_size, = struct.unpack_from("<I", raw, 2)

        body = zlib.decompress(raw[6:], -15)
        if len(body) != raw_size:
            raise ValueError(
                "inflated size 0x%X does not match header 0x%X" % (len(body), raw_size))

        p = 0
        n1, = struct.unpack_from("<h", body, p); p += 2
        self.rsa_modulus = body[p:p + n1]; p += n1
        n2, = struct.unpack_from("<h", body, p); p += 2
        self.rsa_signature = body[p:p + n2]; p += n2
        self.hash_start = p

        n_section, self.entry_point = struct.unpack_from("<ii", body, p); p += 8
        for _ in range(n_section):
            target, size = struct.unpack_from("<ii", body, p); p += 8
            self.segments.append((target, body[p:p + size])); p += size
        if p != len(body):
            raise ValueError("body not fully consumed: 0x%X of 0x%X" % (p, len(body)))
        self.body = body
        return self

    # ---------------------------------------------------------------- writing
    def signed_region(self):
        """Everything the SHA-512 covers: nSection, entryPoint and the segments."""
        out = bytearray()
        out += struct.pack("<ii", len(self.segments), self.entry_point)
        for target, data in self.segments:
            out += struct.pack("<ii", target, len(data))
            out += data
        return bytes(out)

    def build_body(self):
        out = bytearray()
        out += struct.pack("<h", len(self.rsa_modulus)) + self.rsa_modulus
        out += struct.pack("<h", len(self.rsa_signature)) + self.rsa_signature
        out += self.signed_region()
        return bytes(out)

    def pack(self, level=9):
        body = self.build_body()
        comp = zlib.compressobj(level, zlib.DEFLATED, -15)
        deflated = comp.compress(body) + comp.flush()
        return struct.pack("<BBI",
                           self.load_source_flags & 0xFF,
                           (self.load_source_flags >> 8) & 0xFF,
                           len(body)) + deflated

    # ------------------------------------------------------------- signatures
    def digest(self):
        return hashlib.sha512(self.signed_region()).digest()

    def verify(self):
        """Return the matching exponent, or None."""
        if not self.rsa_modulus or not self.rsa_signature:
            return None
        n = int.from_bytes(self.rsa_modulus, "big")
        sig = int.from_bytes(self.rsa_signature, "big")
        want = self.digest()
        for e in KNOWN_EXPONENTS:
            if (pow(sig, e, n) % (1 << 512)).to_bytes(64, "big") == want:
                return e
        return None

    def resign(self, exponent, bits=1024):
        """Generate a fresh keypair with this public exponent and sign the body.

        Only useful if the game validates with the modulus carried in the file.
        """
        n, d = _make_key(exponent, bits)
        m = int.from_bytes(self.digest(), "big")
        sig = pow(m, d, n)
        self.rsa_modulus = n.to_bytes(bits // 8, "big")
        self.rsa_signature = sig.to_bytes(bits // 8, "big")


def _make_key(e, bits):
    """A throwaway RSA key whose public exponent is `e`. Needs `cryptography`
    or falls back to a slow pure-Python prime search."""
    half = bits // 2
    while True:
        p = _prime(half, e)
        q = _prime(half, e)
        if p == q:
            continue
        n = p * q
        if n.bit_length() != bits:
            continue
        phi = (p - 1) * (q - 1)
        try:
            d = pow(e, -1, phi)
        except ValueError:
            continue
        return n, d


def _prime(bits, e):
    import random
    while True:
        c = random.getrandbits(bits) | (1 << (bits - 1)) | 1
        if c % e == 0:
            continue
        if (c - 1) % e == 0:          # need gcd(e, p-1) == 1
            continue
        if _is_probable_prime(c):
            return c


def _is_probable_prime(n, rounds=24):
    import random
    small = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37]
    for s in small:
        if n % s == 0:
            return n == s
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2; r += 1
    for _ in range(rounds):
        a = random.randrange(2, n - 1)
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


# ----------------------------------------------------------------- ELF output
ELF_FLAGS = 546455553      # what GT4ElfBuilderTool writes; matches retail bases


def build_elf(core):
    """A CORRECT ELF for a core image.

    GT4ElfBuilderTool's single-segment path has two bugs, both fixed here:
      1. it writes e_shnum=6 / e_shstrndx=5 but emits only 2 section headers,
         so the table runs past EOF and nothing can parse it;
      2. it writes p_filesz and p_memsz as (size - 0x18) while writing the full
         data, so the last 24 bytes are present but not loadable -- and in GT3
         those bytes hold the engine heap's base and limit constants.
    """
    names = ["", ".text", ".shstrtab"]
    shstrtab = b"\0".join(n.encode() for n in names) + b"\0"
    name_off = {}
    off = 0
    for n in names:
        name_off[n] = off
        off += len(n) + 1

    n_ph = len(core.segments)
    ph_off = 52
    data_off = 0x1000

    out = bytearray(data_off)
    out[0:4] = b"\x7fELF"
    out[4] = 1          # ELFCLASS32
    out[5] = 1          # ELFDATA2LSB
    out[6] = 1          # EV_CURRENT
    struct.pack_into("<HHI", out, 16, 2, 8, 1)          # ET_EXEC, EM_MIPS, version
    struct.pack_into("<I", out, 24, core.entry_point)
    struct.pack_into("<I", out, 28, ph_off)
    struct.pack_into("<I", out, 36, ELF_FLAGS)
    struct.pack_into("<HHHHHH", out, 40, 52, 32, n_ph, 40, len(names), len(names) - 1)

    seg_file_offsets = []
    for i, (target, data) in enumerate(core.segments):
        seg_file_offsets.append(len(out))
        out += data
        out += b"\0" * (-len(out) % 0x1000)
        struct.pack_into("<IIIIIIII", out, ph_off + i * 32,
                         1,                      # PT_LOAD
                         seg_file_offsets[i],
                         target, target,
                         len(data), len(data),   # <-- the full size, not size-0x18
                         7,                      # RWX
                         0x1000)

    shstr_off = len(out)
    out += shstrtab
    out += b"\0" * (-len(out) % 4)

    sh_off = len(out)
    # [0] NULL
    out += b"\0" * 40
    # [1] .text -- covers the first segment
    target, data = core.segments[0]
    out += struct.pack("<IIIIIIIIII", name_off[".text"], 1, 6, target,
                       seg_file_offsets[0], len(data), 0, 0, 0x40, 0)
    # [2] .shstrtab
    out += struct.pack("<IIIIIIIIII", name_off[".shstrtab"], 3, 0, 0,
                       shstr_off, len(shstrtab), 0, 0, 1, 0)

    struct.pack_into("<I", out, 32, sh_off)
    return bytes(out)


# ------------------------------------------------------------------- commands
def cmd_info(a):
    core = Core.unpack(open(a.core, "rb").read())
    print("loadSourceFlags : 0x%04X" % core.load_source_flags)
    print("entryPoint      : 0x%08X" % core.entry_point)
    print("hashStart       : 0x%X" % core.hash_start)
    print("RSA modulus     : %d bytes (%d bits)"
          % (len(core.rsa_modulus), int.from_bytes(core.rsa_modulus, "big").bit_length()))
    print("RSA signature   : %d bytes (%d bits)"
          % (len(core.rsa_signature), int.from_bytes(core.rsa_signature, "big").bit_length()))
    print("SHA-512         : %s" % core.digest().hex())
    e = core.verify()
    print("signature       : %s" % (
        "VERIFIES with exponent %d (%s)" % (e, KNOWN_EXPONENTS[e]) if e
        else "does NOT verify with any known exponent"))
    for i, (target, data) in enumerate(core.segments):
        print("segment %d       : target 0x%08X  size 0x%X" % (i, target, len(data)))


def cmd_unpack(a):
    core = Core.unpack(open(a.core, "rb").read())
    for i, (target, data) in enumerate(core.segments):
        name = a.out if len(core.segments) == 1 else "%s.%d" % (a.out, i)
        open(name, "wb").write(data)
        print("segment %d -> %s (0x%X bytes @ 0x%08X)" % (i, name, len(data), target))


def cmd_elf(a):
    core = Core.unpack(open(a.core, "rb").read())
    open(a.out, "wb").write(build_elf(core))
    print("wrote %s" % a.out)


def cmd_repack(a):
    core = Core.unpack(open(a.core, "rb").read())
    if a.segment:
        data = open(a.segment, "rb").read()
        target = core.segments[a.index][0]
        if len(data) != len(core.segments[a.index][1]):
            print("note: segment %d size 0x%X -> 0x%X"
                  % (a.index, len(core.segments[a.index][1]), len(data)))
        core.segments[a.index] = (target, data)
    if a.flags is not None:
        core.load_source_flags = a.flags
    if a.resign:
        e = a.resign
        print("re-signing with a fresh %d-bit key, public exponent %d..." % (a.bits, e))
        core.resign(e, a.bits)
        print("  new signature verifies locally: %s" % (core.verify() == e))
    out = core.pack(level=a.level)
    open(a.out, "wb").write(out)
    print("wrote %s (0x%X bytes)" % (a.out, len(out)))


def cmd_roundtrip(a):
    raw = open(a.core, "rb").read()
    core = Core.unpack(raw)
    rebuilt = core.pack()
    again = Core.unpack(rebuilt)
    ok = True
    def check(label, x, y):
        nonlocal ok
        good = x == y
        ok &= good
        print("  %-22s %s" % (label, "OK" if good else "MISMATCH"))
    print("round-trip %s:" % a.core)
    check("inflated body", core.build_body(), again.build_body())
    check("segment data", [d for _, d in core.segments], [d for _, d in again.segments])
    check("entry point", core.entry_point, again.entry_point)
    check("load source flags", core.load_source_flags, again.load_source_flags)
    check("signature verifies", core.verify(), again.verify())
    print("  original 0x%X bytes, rebuilt 0x%X bytes (deflate differs by encoder)"
          % (len(raw), len(rebuilt)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="describe a core.gt3 and check its signature")
    p.add_argument("core"); p.set_defaults(func=cmd_info)

    p = sub.add_parser("unpack", help="write the raw segment image out")
    p.add_argument("core"); p.add_argument("out"); p.set_defaults(func=cmd_unpack)

    p = sub.add_parser("elf", help="build a CORRECT ELF from a core.gt3")
    p.add_argument("core"); p.add_argument("out"); p.set_defaults(func=cmd_elf)

    p = sub.add_parser("repack", help="rebuild a core.gt3, optionally with a new segment")
    p.add_argument("core"); p.add_argument("out")
    p.add_argument("--segment", help="replacement segment image")
    p.add_argument("--index", type=int, default=0)
    p.add_argument("--flags", type=lambda s: int(s, 0), help="override loadSourceFlags")
    p.add_argument("--resign", type=int, metavar="EXPONENT",
                   help="sign with a fresh key using this public exponent")
    p.add_argument("--bits", type=int, default=1024)
    p.add_argument("--level", type=int, default=9)
    p.set_defaults(func=cmd_repack)

    p = sub.add_parser("roundtrip", help="unpack, repack and compare")
    p.add_argument("core"); p.set_defaults(func=cmd_roundtrip)

    a = ap.parse_args()
    sys.exit(a.func(a) or 0)


if __name__ == "__main__":
    main()
