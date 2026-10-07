# -*- coding: utf-8 -*-
"""批量对比多个 minidump：异常、寄存器、模块归属、栈上返回地址。"""
import struct
import sys
import glob
import os

ST_THREADLIST, ST_MODULELIST, ST_MEM64, ST_MEMLIST, ST_EXCEPTION = 3, 4, 9, 5, 6


def load(path):
    d = open(path, 'rb').read()
    sig, ver, nst, dirava = struct.unpack_from('<IIII', d, 0)
    S = {}
    for i in range(nst):
        t, size, rva = struct.unpack_from('<III', d, dirava + i * 12)
        S.setdefault(t, []).append((size, rva))
    return d, S


def parse(d, S):
    _, rva = S[ST_EXCEPTION][0]
    tid, _al = struct.unpack_from('<II', d, rva)
    code, _f, _r, addr, npar, _u = struct.unpack_from('<IIQQII', d, rva + 8)
    csize, crva = struct.unpack_from('<II', d, rva + 160)
    off = dict(Edi=0x9C, Esi=0xA0, Ebx=0xA4, Edx=0xA8, Ecx=0xAC,
               Eax=0xB0, Ebp=0xB4, Eip=0xB8, Esp=0xC4)
    ctx = {k: struct.unpack_from('<I', d, crva + o)[0] for k, o in off.items()}
    ctx['_tid'] = tid
    ctx['_code'] = code
    ctx['_addr'] = addr
    ctx['_info0'] = struct.unpack_from('<Q', d, rva + 40)[0]
    ctx['_info1'] = struct.unpack_from('<Q', d, rva + 48)[0]

    mods = []
    _, rva = S[ST_MODULELIST][0]
    n = struct.unpack_from('<I', d, rva)[0]
    for i in range(n):
        base, size = struct.unpack_from('<QI', d, rva + 4 + i * 108)
        if base:
            mods.append((base, size))

    regs = []
    if ST_MEM64 in S:
        _, rva = S[ST_MEM64][0]
        n, baserva = struct.unpack_from('<QQ', d, rva)
        o = baserva
        for i in range(n):
            s, sz = struct.unpack_from('<QQ', d, rva + 16 + i * 16)
            regs.append((s, sz, o))
            o += sz
    if ST_MEMLIST in S:
        _, rva = S[ST_MEMLIST][0]
        n = struct.unpack_from('<I', d, rva)[0]
        for i in range(n):
            s, sz, drva = struct.unpack_from('<QII', d, rva + 4 + i * 16)
            regs.append((s, sz, drva))
    regs.sort()
    return ctx, mods, regs


def rd(regs, addr, cnt):
    for s, sz, o in regs:
        if s <= addr < s + sz:
            k = min(cnt, sz - (addr - s))
            return o + (addr - s)
    return None


def modof(mods, a):
    for base, size in mods:
        if base <= a < base + size:
            return base, a - base
    return None, None


def main():
    out = []
    for path in sorted(glob.glob(sys.argv[1])):
        d, S = load(path)
        ctx, mods, regs = parse(d, S)
        name = os.path.basename(path)
        out.append('=' * 78)
        out.append('DUMP %s' % name)
        out.append('  thread=%X  code=%08X  faultAddr=%08X  read=%08X'
                   % (ctx['_tid'], ctx['_code'], ctx['_addr'], ctx['_info1']))
        base, off = modof(mods, ctx['Eip'])
        if base:
            out.append('  EIP  = %08X   MOD +0x%X' % (ctx['Eip'], off))
        for r in ('Eax', 'Ebx', 'Ecx', 'Edx', 'Esi', 'Edi', 'Ebp', 'Esp'):
            v = ctx[r]
            b, o = modof(mods, v)
            tag = ''
            if b:
                tag = '  <- MOD +0x%X' % o
            elif regs and any(s <= v < s + sz for s, sz, _ in regs):
                tag = '  <- heap/stack'
            else:
                tag = '  <- UNMAPPED'
            out.append('  %-4s = %08X%s' % (r, v, tag))
        out.append('  NOT(Eax) = %08X      NOT(Esi) = %08X' % (~ctx['Eax'] & 0xFFFFFFFF,
                                                                 ~ctx['Esi'] & 0xFFFFFFFF))
        out.append('  aligned? Eax&3=%d Esi&3=%d Edi&3=%d' % (ctx['Eax'] & 3,
                                                              ctx['Esi'] & 3, ctx['Edi'] & 3))
        out.append('  --- stack return addrs ---')
        o = rd(regs, ctx['Esp'], 0x600)
        cnt = 0
        if o:
            for i in range(0, 0x600 - 3, 4):
                v = struct.unpack_from('<I', d, o + i)[0]
                b, off = modof(mods, v)
                if b:
                    out.append('    [%08X] %08X  MOD+0x%X' % (ctx['Esp'] + i, v, off))
                    cnt += 1
                    if cnt > 14:
                        break
        out.append('')
    txt = '\n'.join(out)
    open(sys.argv[2], 'w', encoding='utf-8').write(txt)
    print(txt)


main()