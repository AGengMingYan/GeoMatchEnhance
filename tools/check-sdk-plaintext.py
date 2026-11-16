"""检查 SDK 目录里的 .h/.cpp 是否仍是本机 DLP 驱动的密文。

背景（见 .clinerules）：本机源码在磁盘上是"进程相关加密"的，
PowerShell 的 Copy-Item / cmake --install 复制到的是加密态字节；
打进 zip 后到别的机器上就是乱码。1.1.0 / 1.2.0 的 SDK 都中了这个坑。

用法: python tools/check-sdk-plaintext.py <sdk_dir>
退出码 0 = 全部明文；1 = 存在密文。
"""
import glob
import os
import sys

MAGIC = b'\xe0\xa8\x91\xe7'


def main(path):
    bad = []
    total = 0
    for p in glob.glob(os.path.join(path, '**', '*'), recursive=True):
        if not p.lower().endswith(('.h', '.cpp')):
            continue
        total += 1
        with open(p, 'rb') as f:
            if f.read(4) == MAGIC:
                bad.append(p)
    if bad:
        print('CIPHER(%d/%d):' % (len(bad), total))
        for p in bad:
            print('  ' + p)
        return 1
    print('PLAINTEXT_OK (%d files)' % total)
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('usage: check-sdk-plaintext.py <sdk_dir>')
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
