"""check_appimage_glibc.py —— 校验 AppImage 的 glibc 上限

用途（CI 构建期断言，非交互）：
    ./ZlPhotoEdit-*.AppImage --appimage-extract
    python3 packaging/linux/check_appimage_glibc.py squashfs-root 17

为什么需要它：
    "打出来了" 不等于 "低 glibc 打出来了"。
    AppImage 会把容器里的一堆系统库一起打包，只要其中一个（例如
    libsystemd.so.0）来自较新的发行版，整个包的 glibc 下限就被它顶上去，
    而 AppImage 文件本身**看不出来**。
    ⇒ 必须把产物解包、逐个 ELF 扫 GLIBC_2.x 字符串（纯静态，不执行），
      把上限做成构建期断言，否则这类回归会静默溜过去。

用法：
    python3 check_appimage_glibc.py <解包目录> [上限=$(echo 17 表示 2.17)]

退出码：0 = 通过；1 = 有文件超过上限；2 = 用法错误。
"""

import os
import re
import sys


def _fix_stdout_encoding():
    """POSIX locale 下（容器里几乎都是）stdout 编码是 ASCII，中文直接崩。

    第 12 轮 CI 实测：AppImage 构建成功、排除也生效，却崩在下面那句
    中文 print 上（UnicodeEncodeError），整个自证步骤变成假失败。
    """
    if hasattr(sys.stdout, 'reconfigure'):        # Python 3.7+
        sys.stdout.reconfigure(errors='replace')
        return
    import codecs                                  # Python 3.6（ubuntu:18.04 自带）
    buf = getattr(sys.stdout, 'buffer', None)
    if buf is not None:
        sys.stdout = codecs.getwriter('utf-8')(buf, 'replace')


_fix_stdout_encoding()

PAT = re.compile(rb'GLIBC_2\.(\d+)')


def scan_dir(root):
    """返回 [(最高2.x, 路径)]，只统计真正的 ELF。"""
    rows = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            try:
                with open(path, 'rb') as fh:
                    data = fh.read()
            except OSError:
                continue          # 符号链接、权限不足等一律跳过
            if len(data) < 4 or data[:4] != b'\x7fELF':
                continue
            versions = [int(m.group(1)) for m in PAT.finditer(data)]
            if versions:
                rows.append((max(versions), path))
    rows.sort(reverse=True)
    return rows


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    root = argv[1]
    limit = int(argv[2]) if len(argv) > 2 else 17

    if not os.path.isdir(root):
        print('错误：目录不存在: %s' % root)
        return 2

    rows = scan_dir(root)
    if not rows:
        print('错误：在 %s 里没扫到任何 ELF' % root)
        return 2

    top = rows[0][0]
    print('扫描到 %d 个 ELF；最高需求 = GLIBC_2.%d；上限 = GLIBC_2.%d' % (len(rows), top, limit))

    over = [r for r in rows if r[0] > limit]
    if over:
        print('')
        print('★ 以下文件超过上限 —— 它们会把整个 AppImage 的 glibc 下限抬上去：')
        for version, path in over:
            print('    GLIBC_2.%-3d %s' % (version, path))
        print('')
        print('处理思路：确认这些库是否由目标系统提供（多数是），是则用')
        print('  export LINUXDEPLOY_EXCLUDED_LIBRARIES="libfoo.so.0;libbar.so.0"')
        print('  ./linuxdeploy ... --exclude-library "libfoo.so.0" --exclude-library "libbar.so.0"')
        print('排除。注意 --exclude-library 只作用于 linuxdeploy 自身，'
              'qt 插件是独立子进程，需靠上面的环境变量才生效。')
        return 1

    print('OK：全部 <= GLIBC_2.%d，低 glibc 目标达成。' % limit)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))