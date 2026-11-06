"""Find the editor mod's mx.exe addresses in another game build.

    python tools/port_build.py "<known mx.exe>" "<other mx.exe>"

Code addresses: the instructions at the known address, with branch targets and RIP-relative displacements masked
(those move between builds; everything else, stack sizes and struct offsets included, must match), searched for in
the other build; a match must be unique. Data addresses: the instructions that use the global in the known build are
matched the same way, and where the other build's instruction points is the answer; the sites vote.
Needs capstone and pefile (pip install capstone pefile). Read-only: neither exe is changed.
"""
import collections
import re
import sys

import capstone
import pefile

CODE = {  # name: rva in the known build (src/ingame.c, game.c)
    "RVA_EDITOR_DRAW": 0x613B0, "RVA_KEY_HANDLER": 0x645D0,
    "RVA_MATRIX_MODE": 0x44E30, "RVA_IDENTITY": 0x44910, "RVA_TRANSLATE": 0x44B00, "RVA_SCALE": 0x44C20,
    "RVA_PUSH": 0x44E80, "RVA_POP": 0x44EC0, "RVA_ENABLE": 0x44710, "RVA_DISABLE": 0x446C0,
    "RVA_BIND_TEXTURE": 0x44540, "RVA_BLEND_FUNC": 0x445B0, "RVA_BEGIN": 0xC9B0, "RVA_VERTEX": 0x5BB60,
    "RVA_END": 0xCD00, "RVA_NEXT_ROW": 0x1E190, "RVA_BUTTON": 0x1EA30, "RVA_CHOICE": 0x1ED10,
    "RVA_SLIDER": 0x1F130, "RVA_READOUT": 0x1E830, "RVA_BRUSH_DISTANCE": 0x60035, "RVA_BRUSH_RETURN": 0x6005A,
    "RVA_FN_MARK_DIRTY": 0x17830, "RVA_FN_UNDO_SNAPSHOT": 0x5B060, "RVA_GAME_REALLOC": 0xBBC0,
    "RVA_GAME_MALLOC": 0xB9C0, "RVA_GAME_FREE": 0xBCE0,"RVA_RADIAL_HEIGHT": 0x5FA10, "RVA_PROFILE_HEIGHT": 0x5F820,
}
DATA = {
    "RVA_PTR_WINDOW": 0x23D7E0, "RVA_PTR_KEYS": 0x23D670, "RVA_PTR_COLOUR": 0x23D9A0,
    "RVA_PTR_BATCH_FLAG": 0x23D9E0, "RVA_PTR_WHITE_TEX": 0x23D890, "RVA_PTR_READOUT_A": 0x23D560,
    "RVA_PTR_READOUT_B": 0x23D550, "RVA_EDITOR_TERRAIN": 0x17E25C8, "RVA_VIEW_X": 0x17E25DC,
    "RVA_VIEW_Z": 0x17E25E0, "RVA_ZOOM": 0x1778E8, "RVA_HOVERED_WIDGET": 0x17E25A4, "RVA_EDITOR_MODE": 0x17E2600,
    "RVA_MOUSE_DOWN": 0x17E25E4, "RVA_GRADIENTS": 0x17E2618, "RVA_GRADIENT_CAP": 0x17E2620,
    "RVA_GRADIENT_COUNT": 0x17E2624, "RVA_GRADIENT_STRIDE": 0x17E2628, "RVA_SELECTED_POINT": 0x17E2630,
    "RVA_SELECTED_GRADIENT": 0x17E2634, "RVA_UNDO_PENDING": 0x17E25D8, "RVA_STATUS_TEXT": 0x17E26A8,
    "RVA_DRAG_STATE": 0x17E2608,
}


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        text = next(s for s in self.pe.sections if s.Name.rstrip(b"\0") == b".text")
        self.text_rva = text.VirtualAddress
        self.text = text.get_data()[: text.Misc_VirtualSize]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True

    def insns(self, rva, size):
        off = rva - self.text_rva
        return list(self.md.disasm(self.text[off: off + size], rva))


def masked(insns):
    """A regex for these instructions: branch targets and RIP-relative displacements are wildcards."""
    out = b""
    for i in insns:
        raw = bytearray(i.bytes)
        wild = set()
        if i.disp_size and any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                               for op in i.operands):
            wild.update(range(i.disp_offset, i.disp_offset + i.disp_size))
        if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL):
            if i.imm_size and i.operands and i.operands[0].type == capstone.x86.X86_OP_IMM:
                wild.update(range(i.imm_offset, i.imm_offset + i.imm_size))
        out += b"".join(b"." if k in wild else re.escape(bytes([raw[k]])) for k in range(len(raw)))
    return re.compile(out, re.S)


def find(old, new, rva, min_len=24, max_len=160):
    """The other build's rva for the known one, growing the window until the match is unique."""
    size = min_len
    while size <= max_len:
        insns = old.insns(rva, size + 16)
        took, n = [], 0
        for i in insns:
            took.append(i)
            n += i.size
            if n >= size:
                break
        hits = [m.start() for m in masked(took).finditer(new.text)]
        if len(hits) == 1:
            return new.text_rva + hits[0], n
        if not hits:
            return None, n
        size += 16
    return "ambiguous", size


def rip_target(i):
    """Where a RIP-relative operand points, or a call's target (so functions get call sites too)."""
    for op in i.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    if i.group(capstone.CS_GRP_CALL) and i.operands and i.operands[0].type == capstone.x86.X86_OP_IMM:
        return i.operands[0].imm
    return None


def vote(old, new, sites, limit=12):
    votes = collections.Counter()
    for site in sites[:limit]:
        got, _ = find(old, new, site, min_len=16, max_len=96)
        if isinstance(got, int):
            t = rip_target(new.insns(got, 16)[0])
            if t is not None:
                votes[t] += 1
    return votes


def sites_using(old, wanted):
    """Every instruction in the known build's .text that reads or writes one of `wanted`, by linear sweep."""
    sites = collections.defaultdict(list)
    off = 0
    md = old.md
    while off < len(old.text):
        got = False
        for i in md.disasm(old.text[off:], old.text_rva + off):
            got = True
            t = rip_target(i)
            if t in wanted:
                sites[t].append(i.address)
            off = i.address + i.size - old.text_rva
        if not got:
            off += 1
    return sites


def main(old_path, new_path):
    old, new = Image(old_path), Image(new_path)
    sites = sites_using(old, set(DATA.values()) | set(CODE.values()))
    print("code (a unique match; else votes from call sites)")
    for name, rva in CODE.items():
        got, n = find(old, new, rva)
        shown = f"0x{got:x}" if isinstance(got, int) else str(got)
        if not isinstance(got, int):
            votes = vote(old, new, sites.get(rva, []))
            shown += "; calls: " + (", ".join(f"0x{t:x} x{c}" for t, c in votes.most_common(3)) or "no votes")
        print(f"  {name:24} 0x{rva:x} -> {shown}  ({n} bytes, {len(sites.get(rva, []))} sites)")
    print("data (votes from matched instructions)")
    for name, rva in DATA.items():
        votes = vote(old, new, sites.get(rva, []))
        top = ", ".join(f"0x{t:x} x{c}" for t, c in votes.most_common(3)) or "no votes"
        print(f"  {name:24} 0x{rva:x} -> {top}  ({len(sites.get(rva, []))} sites)")


if __name__ == "__main__":
    main(*sys.argv[1:3])
