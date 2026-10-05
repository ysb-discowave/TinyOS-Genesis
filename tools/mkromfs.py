#!/usr/bin/env python3
"""根据清单生成 kernel/romfs.c：把 .TNCR 文件嵌入内核初始文件系统。
清单格式（每行）：<本地TNCR路径> <VFS目标路径>
生成的 romfs.c 采用 vfs_load_romfs 解析的格式：
  [u32 namelen][name][u32 datalen][data] ... 结束于 namelen=0
"""
import sys, struct

def main():
    if len(sys.argv) < 3:
        print("用法: mkromfs.py <清单.txt> <输出.c>"); sys.exit(1)
    manifest, out = sys.argv[1], sys.argv[2]
    entries = []
    with open(manifest, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'): continue
            parts = line.split(None, 1)
            if len(parts) != 2: continue
            entries.append((parts[0], parts[1]))
    blob = bytearray()
    for local, vfs in entries:
        with open(local, 'rb') as fh:
            data = fh.read()
        name = vfs.encode('utf-8')
        blob += struct.pack('<I', len(name))
        blob += name
        blob += struct.pack('<I', len(data))
        blob += data
    blob += struct.pack('<I', 0)  # 结束标记
    with open(out, 'w', encoding='utf-8') as f:
        f.write('#include "types.h"\n')
        f.write('/* 由 tools/mkromfs.py 自动生成，请勿手改 */\n')
        f.write('const u8 romfs_data[] = {\n')
        for i in range(0, len(blob), 16):
            chunk = blob[i:i+16]
            f.write('  ' + ','.join('0x%02X' % b for b in chunk) + ',\n')
        f.write('};\n')
        f.write('const u32 romfs_len = %d;\n' % len(blob))
    print("romfs.c 已生成: %d 字节, %d 个文件" % (len(blob), len(entries)))

if __name__ == "__main__":
    main()
