"""Find the game editor's slider structs (Granularity, Height, Spacing, Radius, Focus, Strength, Zoom) in an mx.exe by
their labels: each is 64 bytes {char *label; double value, step, min, max; int rect[4]; int bar[2];} in .data, laid
out next to each other ending with Zoom. Give it the build's rva_zoom (from src/builds.c, or port_build.py) and it
prints each slider's label and its value's RVA. Read-only.

    python find_sliders.py "<mx.exe>" <rva_zoom, hex>
"""
import struct, sys
def load(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]; optsz = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        name = d[o:o+8].rstrip(b"\0").decode(); vs, va, rs, raw = struct.unpack_from("<IIII", d, o + 8)
        secs.append((name, va, max(vs, rs), raw, rs))
    return d, base, secs
def off(secs, rva):
    for n, va, sz, raw, rs in secs:
        if va <= rva < va + sz:
            return raw + rva - va if rva - va < rs else None
    return None
def cstr(d, secs, base, va):
    o = off(secs, va - base) if va else None
    if o is None: return "?"
    return d[o:d.index(b"\0", o)][:40].decode("latin1")
path, zoom_rva = sys.argv[1], int(sys.argv[2], 16)
d, base, secs = load(path)
struct0 = zoom_rva - 8
for k in range(-10, 2):
    rva = struct0 + 64 * k
    o = off(secs, rva)
    if o is None: print(hex(rva), "bss (zero on disk)"); continue
    lab, val, step, mn, mx = struct.unpack_from("<Qdddd", d, o)
    print(f"slider at rva {rva:#x} value at {rva+8:#x}: {cstr(d, secs, base, lab)!r:24} value={val:g} step={step:g} min={mn:g} max={mx:g}")
