# -*- coding: utf-8 -*-
"""
tr.py — Hard Truck Apocalypse 汉化翻译写入工具
用法: python -X utf8 tr.py <content_file> <target_xml>
content_file: UTF-8 文本，包含翻译后的完整 XML 内容
写入时转成 GBK 编码（DLL 转码层按 GBK 识别双字节）
"""
import sys

def main():
    if len(sys.argv) != 3:
        print('usage: tr.py <content_file> <target_xml>')
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        raw = f.read()
    # 去除 UTF-8 BOM
    if raw.startswith(b'\xef\xbb\xbf'):
        raw = raw[3:]
    text = raw.decode('utf-8')
    # 检查 GBK 可编码性
    bad = []
    for i, ch in enumerate(text):
        try:
            ch.encode('gbk')
        except UnicodeEncodeError:
            bad.append((i, hex(ord(ch))))
    if bad:
        print('GBK encode check FAILED, first 20 bad chars:')
        for i, c in bad[:20]:
            # 上下文
            s = max(0, i-15); e = min(len(text), i+15)
            print('  at %d char %s context: %r' % (i, c, text[s:e]))
        return 2
    data = text.encode('gbk')
    with open(dst, 'wb') as f:
        f.write(data)
    print('OK: %d bytes GBK -> %s' % (len(data), dst))
    return 0

if __name__ == '__main__':
    sys.exit(main())
