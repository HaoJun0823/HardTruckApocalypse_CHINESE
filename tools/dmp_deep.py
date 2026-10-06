# -*- coding: utf-8 -*-
"""minidump 取证：模块表 / 栈顶 / 全内存模式搜索。
用法: python dmp_deep.py <dump> [搜索的字节模式(空格分隔, 可多个用 ; 分隔)]
"""
import struct
import sys

MDMP = 0x504D444D
ST_THREADLIST = 3
ST_MODULELIST = 4
ST_MEMLIST = 5
ST_EXCEPTION = 6
ST_MEM64 = 9


def read_mem(data, regions, addr, count):
    for start, size, off in regions:
        if start <= addr < start + size:
            n = min(count, size - (addr - start))
            return data[off + (addr - start): off + (addr - start) + n]
    return None


def main():
    path = sys.argv[1]
    pats = []
    if len(sys.argv) > 2:
        for grp in sys.argv[2].split(';'):
            b = bytes(int(x, 16) for x in grp.split())
            pats.append((grp, b))
    data = open(path, 'rb').read()
    sig, ver, nstreams, dirava = struct.unpack_from('<IIII', data, 0)
    print('sig=%08X ver=%08X streams=%d' % (sig, ver, nstreams))
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from('<III', data, dirava + i * 12)
        streams.setdefault(t, []).append((size, rva))

    ctx = {}
    if ST_EXCEPTION in streams:
        _, rva = streams[ST_EXCEPTION][0]
        tid, _al = struct.unpack_from('<II', data, rva)
        code, flags, _rec, addr, nparam, _un = struct.unpack_from('<IIQQII', data, rva + 8)
        print('EXCEPTION tid=%X code=%08X addr=%08X' % (tid, code, addr))
        for i in range(min(nparam, 4)):
            v = struct.unpack_from('<Q', data, rva + 40 + i * 8)[0]
            print('   info[%d] = %016X' % (i, v))
        csize, crva = struct.unpack_from('<II', data, rva + 160)
        names = [('Edi', 0x9C), ('Esi', 0xA0), ('Ebx', 0xA4), ('Edx', 0xA8),
                 ('Ecx', 0xAC), ('Eax', 0xB0), ('Ebp', 0xB4), ('Eip', 0xB8),
                 ('SegCs', 0xBC), ('EFlags', 0xC0), ('Esp', 0xC4), ('SegSs', 0xC8)]
        ctx = {n: struct.unpack_from('<I', data, crva + o)[0] for n, o in names}

    # ---- 模块表 ----
    if ST_MODULELIST in streams:
        _, rva = streams[ST_MODULELIST][0]
        n = struct.unpack_from('<I', data, rva)[0]
        print('MODULES=%d' % n)
        for i in range(n):
            base, size = struct.unpack_from('<QI', data, rva + 4 + i * 108)
            nrva = struct.unpack_from('<I', data, rva + 4 + i * 108 + 32)[0]
            name = ''
            if nrva:
                ln = struct.unpack_from('<I', data, nrva)[0]
                name = data[nrva + 4:nrva + 4 + ln].decode('utf-16-le', 'replace')
            if base:
                print('   %08X-%08X  %s' % (base, base + size, name))

    # ---- 内存块 ----
    regions = []
    if ST_MEM64 in streams:
        _, rva = streams[ST_MEM64][0]
        n, baserva = struct.unpack_from('<QQ', data, rva)
        off = baserva
        for i in range(n):
            start, size = struct.unpack_from('<QQ', data, rva + 16 + i * 16)
            regions.append((start, size, off))
            off += size
    if ST_MEMLIST in streams:
        _, rva = streams[ST_MEMLIST][0]
        n = struct.unpack_from('<I', data, rva)[0]
        for i in range(n):
            start, size, drva = struct.unpack_from('<QII', data, rva + 4 + i * 16)
            regions.append((start, size, drva))
    regions.sort()
    print('REGIONS=%d' % len(regions))
    for s, sz, _o in regions:
        print('   %08X + %08X' % (s, sz))

    # ---- 栈顶 ----
    esp = ctx.get('Esp')
    if esp:
        raw = read_mem(data, regions, esp, 256)
        print('STACK @%08X:' % esp)
        if raw:
            for i in range(0, len(raw), 4):
                v = struct.unpack_from('<I', raw, i)[0]
                tag = ''
                for s, sz, _o in regions:
                    if s <= v < s + sz:
                        tag = '  -> region %08X' % s
                        break
                print('   [%08X] %08X%s' % (esp + i, v, tag))
        else:
            print('   (栈不可读)')

    # ---- EIP 处模块定位 ----
    eip = ctx.get('Eip')
    if eip:
        print('EIP=%08X 字节: %s' % (eip, read_mem(data, regions, eip, 16)))

    # ---- 模式搜索 ----
    for txt, pat in pats:
        print('SEARCH %s' % txt)
        hits = 0
        for s, sz, off in regions:
            blob = data[off:off + sz]
            p = blob.find(pat)
            while p >= 0:
                print('   @%08X' % (s + p))
                hits += 1
                p = blob.find(pat, p + 1)
                if hits > 40:
                    break
            if hits > 40:
                break
        if not hits:
            print('   (无命中)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
