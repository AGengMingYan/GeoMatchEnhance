"""把目录打成 .zip1（本机 .zip 落盘即密文，.zip1 是约定的明文集名）。

背景：PowerShell / .NET 的 ZipFile::CreateFromDirectory 在本机读到的是
源码的"加密态视图"，打出来的包里 .h/.cpp 是密文（1.1.0 / 1.2.0 的 SDK 都中招）。
python 读到的是明文，所以打包交给它。

用法: python tools/make-zip1.py <src_dir> <out.zip1>
src_dir 必须就是最终顶层目录（否则包里会多一层）。
"""
import os
import sys
import zipfile

MAGIC = b'\xe0\xa8\x91\xe7'


def main(src, out):
    if not os.path.isdir(src):
        print('src not a dir: ' + src)
        return 2
    top = os.path.basename(os.path.normpath(src))
    if os.path.exists(out):
        os.remove(out)

    n = 0
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, _, files in os.walk(src):
            for fn in files:
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, src)
                z.write(full, os.path.join(top, rel))
                n += 1

    with open(out, 'rb') as f:
        head = f.read(4)
    if head[:2] != b'PK':
        print('not plaintext zip: ' + out)
        return 3

    # 包内不能有密文源码
    bad = []
    with zipfile.ZipFile(out) as z:
        for name in z.namelist():
            if name.lower().endswith(('.h', '.cpp')) and z.read(name)[:4] == MAGIC:
                bad.append(name)
    if bad:
        print('CIPHER inside: ' + ' '.join(bad))
        return 4

    size_mb = os.path.getsize(out) / (1024.0 * 1024.0)
    print('  %-36s %7.2f MB  head=%s  files=%d' %
          (os.path.basename(out), size_mb, head.hex(' ').upper(), n))
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('usage: make-zip1.py <src_dir> <out.zip1>')
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
