# -*- coding: utf-8 -*-
"""Parse an ExMachina minidump and print the RAW exception record + registers.

Why not trust hta.exe's own crash log:
  it resolves EIP against a bogus symbol table (it reported 0x686A5E as
  "n_SetWeather +2607758"), so its idea of where the fault happened is
  unreliable. The MINIDUMP_EXCEPTION_STREAM is the only trustworthy source.

Layout notes (these bit me once):
  MINIDUMP_EXCEPTION uses ULONG64 for ExceptionRecord/ExceptionAddress and
  ULONG64 ExceptionInformation[15], so the struct is 152 bytes on BOTH
  32- and 64-bit.  MINIDUMP_MODULE is 108 bytes with an 8-byte BaseOfImage.
"""
import struct
import sys

STREAM_NAMES = {
    3: "ThreadList", 4: "ModuleList", 5: "MemoryList", 6: "Exception",
    7: "SystemInfo", 8: "ThreadExList", 9: "Memory64List",
    14: "UnloadedModuleList", 15: "MiscInfo", 16: "MemoryInfoList",
}

# x86 CONTEXT offsets (after ContextFlags/Dr/FloatSave)
CTX_X86 = [
    (0x8C, "SegGs"), (0x90, "SegFs"), (0x94, "SegEs"), (0x98, "SegDs"),
    (0x9C, "Edi"), (0xA0, "Esi"), (0xA4, "Ebx"), (0xA8, "Edx"),
    (0xAC, "Ecx"), (0xB0, "Eax"), (0xB4, "Ebp"), (0xB8, "Eip"),
    (0xBC, "SegCs"), (0xC0, "EFlags"), (0xC4, "Esp"), (0xC8, "SegSs"),
]

# x64 CONTEXT offsets
CTX_X64 = [
    (0x78, "Rax"), (0x80, "Rcx"), (0x88, "Rdx"), (0x90, "Rbx"),
    (0x98, "Rsp"), (0xA0, "Rbp"), (0xA8, "Rsi"), (0xB0, "Rdi"),
    (0xB8, "R8"), (0xC0, "R9"), (0xC8, "R10"), (0xD0, "R11"),
    (0xD8, "R12"), (0xE0, "R13"), (0xE8, "R14"), (0xF0, "R15"),
    (0xF8, "Rip"),
]

ACCESS = {0: "READ", 1: "WRITE", 8: "EXECUTE/DEP"}


def parse(data):
    magic, version, nstreams, dirrva = struct.unpack_from("<IIII", data, 0)
    assert magic == 0x504D444D, "not a minidump"
    streams = {}
    for i in range(nstreams):
        stype, dsize, rva = struct.unpack_from("<III", data, dirrva + i * 12)
        if stype:
            streams.setdefault(stype, []).append((dsize, rva))
    return streams


def read_mdstring(data, rva):
    (length,) = struct.unpack_from("<I", data, rva)
    return data[rva + 4:rva + 4 + length].decode("utf-16-le", "replace")


def main(path):
    data = open(path, "rb").read()
    streams = parse(data)

    print("=== streams ===")
    for st in sorted(streams):
        for dsize, rva in streams[st]:
            print("  %-20s type=%-3d size=%-7d rva=0x%X"
                  % (STREAM_NAMES.get(st, "?"), st, dsize, rva))

    if 6 not in streams:
        print("no Exception stream")
        return 1

    er = streams[6][0][1]
    tid, _align = struct.unpack_from("<II", data, er)
    code, flags = struct.unpack_from("<II", data, er + 8)
    nested, = struct.unpack_from("<Q", data, er + 16)
    addr, = struct.unpack_from("<Q", data, er + 24)
    nparam, _unused = struct.unpack_from("<II", data, er + 32)

    print("\n=== RAW EXCEPTION_RECORD (offset 0x%X) ===" % er)
    print("  ThreadId         = 0x%X" % tid)
    print("  ExceptionCode    = 0x%08X" % code)
    print("  ExceptionFlags   = 0x%08X" % flags)
    print("  NestedRecord     = 0x%X" % nested)
    print("  ExceptionAddress = 0x%08X   <-- where it really faulted" % (addr & 0xFFFFFFFF))
    print("  NumberParameters = %d" % nparam)
    if nparam:
        n = min(nparam, 15)
        info = struct.unpack_from("<%dQ" % n, data, er + 40)
        for i, v in enumerate(info):
            print("    [%d] = 0x%X" % (i, v))
        if code == 0xC0000005 and n >= 2:
            print("  => %s at 0x%08X" % (ACCESS.get(info[0], "?"), info[1] & 0xFFFFFFFF))

    (ctx_rva,) = struct.unpack_from("<I", data, er + 160)
    print("\n=== thread context (rva 0x%X) ===" % ctx_rva)
    ctxflags, = struct.unpack_from("<I", data, ctx_rva)
    is64 = 0x00100000 in (ctxflags & 0x00100000,) and (ctxflags & 0x100000) != 0
    # AMD64 CONTEXT has ContextFlags bit 0x100000
    table = CTX_X64 if (ctxflags & 0x100000) else CTX_X86
    regs = {}
    for off, name in table:
        (val,) = struct.unpack_from("<Q" if table is CTX_X64 else "<I", data, ctx_rva + off)
        regs[name] = val
        print("  %-8s = 0x%016X" % (name, val))

    eip = regs.get("Eip") or regs.get("Rip") or (addr & 0xFFFFFFFF)

    # Dump memory around the fault address so we can tell whether the
    # reported address is even an instruction boundary.
    regions = []
    if 9 in streams:
        _, rva = streams[9][0]
        nreg, = struct.unpack_from("<Q", data, rva)
        cur = rva + 8 + nreg * 16
        for i in range(nreg):
            sa, ss = struct.unpack_from("<QQ", data, rva + 8 + i * 16)
            regions.append((sa, ss, cur))
            cur += ss
    if 5 in streams:
        _, rva = streams[5][0]
        nreg, = struct.unpack_from("<I", data, rva)
        for i in range(nreg):
            sa, dsize, drva = struct.unpack_from("<QII", data, rva + 4 + i * 16)
            regions.append((sa, dsize, drva))

    print("\n=== bytes around fault address ===")
    for sa, ss, frva in regions:
        if sa <= eip < sa + ss:
            off = eip - sa
            start = max(0, off - 48)
            end = min(ss, off + 48)
            raw = data[frva + start: frva + end]
            base = sa + start
            for r in range(0, len(raw), 16):
                chunk = raw[r:r + 16]
                pos = base + r
                mark = "  <-- EIP" if pos <= eip < pos + 16 else ""
                print("  0x%08X  %-47s %s" % (pos, " ".join("%02X" % b for b in chunk), mark))
            break
    else:
        print("  fault address not inside any dumped region")

    # --- stack / memory scan ---------------------------------------------
    # If nobody jumps to the fault address (xrefs_to == 0), the only way RIP
    # got there is a `ret`/indirect jump. So look for the value anywhere in
    # the dumped memory: if it sits where a return address belongs, we know
    # which call returned to the wrong place.
    print("\n=== search all dumped memory for 0x%08X ===" % (eip & 0xFFFFFFFF))
    needle = struct.pack("<I", eip & 0xFFFFFFFF)
    holes = []
    for sa, ss, frva in regions:
        blob = data[frva: frva + ss]
        start = 0
        while True:
            k = blob.find(needle, start)
            if k < 0:
                break
            holes.append((sa + k, sa, ss, frva + k))
            start = k + 1
    if not holes:
        print("  not found in any dumped region")
    for a, sa, ss, frva in holes:
        print("  0x%08X  (region base 0x%08X size 0x%X)" % (a, sa, ss))
        lo = max(0, a - sa - 32)
        hi = min(ss, a - sa + 32)
        for r in range(lo, hi, 4):
            if r + 4 > hi:
                break
            val, = struct.unpack_from("<I", data, frva - (a - sa) + r)
            mark = "  <== 0x%08X" % (eip & 0xFFFFFFFF) if val == (eip & 0xFFFFFFFF) else ""
            print("      0x%08X = 0x%08X%s" % (sa + r, val, mark))

    if 4 in streams:
        _, rva = streams[4][0]
        nmod, = struct.unpack_from("<I", data, rva)
        print("\n=== modules (%d) ===" % nmod)
        hits = []
        for i in range(nmod):
            b = rva + 4 + i * 108
            baddr, = struct.unpack_from("<Q", data, b)
            size, _cs, _tds = struct.unpack_from("<III", data, b + 8)
            namerva, = struct.unpack_from("<I", data, b + 20)
            name = read_mdstring(data, namerva)
            if baddr <= eip < baddr + size:
                hits.append((baddr, size, name))
            print("  0x%08X size=0x%-8X %s" % (baddr, size, name))
        print("\n=== which module owns the fault address ===")
        for baddr, size, name in hits:
            print("  %s  base=0x%08X  +0x%X" % (name, baddr, eip - baddr))
        if not hits:
            print("  (none - address is not inside any module)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
