# -*- coding: utf-8 -*-
"""极简 minidump 探针：只回答「崩溃瞬间，某个地址上的字节是什么」。
用法: python dmp_probe.py <dump> <hexaddr> [count]
"""
import struct
import sys

MDMP = 0x504D444D
ST_THREADLIST = 3
ST_MODULELIST = 4
ST_MEMLIST = 5
ST_EXCEPTION = 6
ST_MEM64 = 9


def main():
    path = sys.argv[1]
    targets = [int(a, 16) for a in sys.argv[2].split(',')]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    data = open(path, 'rb').read()
    sig, ver, nstreams, dirava = struct.unpack_from('<IIII', data, 0)
    if sig != MDMP:
        print('不是 minidump: %08X' % sig)
        return 2
    print('minidump ver=%08X streams=%d' % (ver, nstreams))
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from('<III', data, dirava + i * 12)
        streams.setdefault(t, []).append((size, rva))
    for k in sorted(streams):
        print('  stream %2d size=%-10d rva=%08X' % (k, streams[k][0][0], streams[k][0][1]))

    # ---- 异常流 ----
    if ST_EXCEPTION in streams:
        _, rva = streams[ST_EXCEPTION][0]
        tid, _al = struct.unpack_from('<II', data, rva)
        code, flags, _rec, addr, nparam, _un = struct.unpack_from('<IIQQII', data, rva + 8)
        print('EXCEPTION tid=%X code=%08X flags=%X addr=%08X nparam=%d'
              % (tid, code, flags, addr, nparam))
        for i in range(min(nparam, 15)):
            v = struct.unpack_from('<Q', data, rva + 40 + i * 8)[0]
            print('   info[%d] = %016X' % (i, v))
        csize, crva = struct.unpack_from('<II', data, rva + 160)
        print('CONTEXT size=%d rva=%08X' % (csize, crva))
        if crva and csize >= 0xCC:
            names = [('Edi', 0x9C), ('Esi', 0xA0), ('Ebx', 0xA4), ('Edx', 0xA8),
                     ('Ecx', 0xAC), ('Eax', 0xB0), ('Ebp', 0xB4), ('Eip', 0xB8),
                     ('SegCs', 0xBC), ('EFlags', 0xC0), ('Esp', 0xC4), ('SegSs', 0xC8)]
            ctx = {n: struct.unpack_from('<I', data, crva + o)[0] for n, o in names}
            print('  ' + '  '.join('%s=%08X' % (n, ctx[n]) for n, _ in names[:8]))
            print('  ' + '  '.join('%s=%08X' % (n, ctx[n]) for n, _ in names[8:]))

    # ---- 内存块表 ----
    regions = []
    if ST_MEM64 in streams:
        _, rva = streams[ST_MEM64][0]
        n, baserva = struct.unpack_from('<QQ', data, rva)
        print('MEM64 ranges=%d baserva=%X' % (n, baserva))
        off = baserva
        for i in range(n):
            start, size = struct.unpack_from('<QQ', data, rva + 16 + i * 16)
            regions.append((start, size, off))
            off += size
    if ST_MEMLIST in streams:
        _, rva = streams[ST_MEMLIST][0]
        n = struct.unpack_from('<I', data, rva)[0]
        print('MEMLIST ranges=%d' % n)
        for i in range(n):
            start, size, drva = struct.unpack_from('<QII', data, rva + 4 + i * 16)
            regions.append((start, size, drva))
    print('合计可读内存块 %d' % len(regions))

    def find(addr):
        for start, size, off in regions:
            if start <= addr < start + size:
                return start, size, off + (addr - start)
        return None

    for t in targets:
        r = find(t)
        if not r:
            print('@%08X 不在任何内存块里' % t)
            continue
        start, size, off = r
        print('@%08X 属于块 [%08X..%08X) 文件偏移 %08X' % (t, start, start + size, off))
        raw = data[off:off + count]
        print('   ' + ' '.join('%02X' % b for b in raw))
    return 0


if __name__ == '__main__':
    sys.exit(main())
