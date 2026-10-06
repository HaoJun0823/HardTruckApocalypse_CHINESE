# -*- coding: utf-8 -*-
"""在 minidump 的全部内存块里搜索字节模式，并列出所有覆盖某地址的块。
用法: python dmp_find.py <dump> <addr1,addr2> [pattern;pattern]
"""
import struct
import sys

MDMP = 0x504D444D
ST_MEMLIST = 5
ST_MEM64 = 9


def main():
    path = sys.argv[1]
    addrs = [int(a, 16) for a in sys.argv[2].split(',')] if len(sys.argv) > 2 and sys.argv[2] else []
    pats = []
    if len(sys.argv) > 3 and sys.argv[3]:
        for grp in sys.argv[3].split(';'):
            pats.append((grp, bytes(int(x, 16) for x in grp.split())))
    data = open(path, 'rb').read()
    sig, ver, nstreams, dirava = struct.unpack_from('<IIII', data, 0)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from('<III', data, dirava + i * 12)
        streams.setdefault(t, []).append((size, rva))

    regions = []
    if ST_MEM64 in streams:
        _, rva = streams[ST_MEM64][0]
        n, baserva = struct.unpack_from('<QQ', data, rva)
        off = baserva
        for i in range(n):
            start, size = struct.unpack_from('<QQ', data, rva + 16 + i * 16)
            regions.append([start, size, off, 'MEM64'])
            off += size
    if ST_MEMLIST in streams:
        _, rva = streams[ST_MEMLIST][0]
        n = struct.unpack_from('<I', data, rva)[0]
        for i in range(n):
            start, size, drva = struct.unpack_from('<QII', data, rva + 4 + i * 16)
            regions.append([start, size, drva, 'MEMLIST'])
    print('地区数=%d  文件大小=%d' % (len(regions), len(data)))

    for a in addrs:
        print('--- 覆盖 %08X 的块 ---' % a)
        hit = 0
        for start, size, off, kind in regions:
            if start <= a < start + size:
                rel = a - start
                raw = data[off + rel: off + rel + 24]
                print('   [%s] %08X..%08X  +%X  %s' % (kind, start, start + size, rel,
                                                       ' '.join('%02X' % b for b in raw)))
                hit += 1
        if not hit:
            print('   (无)')

    for txt, pat in pats:
        print('--- 搜索 %s ---' % txt)
        total = 0
        for start, size, off, kind in regions:
            blob = data[off:off + size]
            p = blob.find(pat)
            while p >= 0:
                print('   [%s] @%08X  (块基址 %08X)' % (kind, start + p, start))
                total += 1
                p = blob.find(pat, p + 1)
        print('   合计 %d 处' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
