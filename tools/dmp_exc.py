# -*- coding: utf-8 -*-
"""只打印 minidump 的异常记录与 CONTEXT，不做模块解析。"""
import struct
import sys

ST_EXCEPTION = 6

REGS = [('Edi', 0x9C), ('Esi', 0xA0), ('Ebx', 0xA4), ('Edx', 0xA8),
        ('Ecx', 0xAC), ('Eax', 0xB0), ('Ebp', 0xB4), ('Eip', 0xB8),
        ('SegCs', 0xBC), ('EFlags', 0xC0), ('Esp', 0xC4), ('SegSs', 0xC8)]


def main():
    data = open(sys.argv[1], 'rb').read()
    sig, ver, nstreams, dirava = struct.unpack_from('<IIII', data, 0)
    rva = None
    for i in range(nstreams):
        t, size, r = struct.unpack_from('<III', data, dirava + i * 12)
        if t == ST_EXCEPTION:
            rva = r
            break
    if rva is None:
        print('无异常流')
        return 1

    tid, al = struct.unpack_from('<II', data, rva)
    code, flags, rec, addr, nparam, _un = struct.unpack_from('<IIQQII', data, rva + 8)
    print('线程ID      = %X' % tid)
    print('异常代码    = %08X' % code)
    print('异常标志    = %08X' % flags)
    print('异常记录    = %016X' % rec)
    print('异常地址    = %08X   <-- CPU 报的出错指令地址' % addr)
    print('参数个数    = %d' % nparam)
    for i in range(nparam):
        v = struct.unpack_from('<Q', data, rva + 40 + i * 8)[0]
        print('  ExceptionInformation[%d] = %016X' % (i, v))
    csize, crva = struct.unpack_from('<II', data, rva + 160)
    print('CONTEXT 大小=%d @%08X' % (csize, crva))
    for n, o in REGS:
        print('  %-7s = %08X' % (n, struct.unpack_from('<I', data, crva + o)[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
