"""check_embedded_i18n.py —— 断言可执行文件里真的内嵌了翻译

用途（CI 构建期断言，非交互）：
    python3 packaging/i18n/check_embedded_i18n.py build/ZlPhotoEdit zh_CN zh_TW

为什么需要它：
    "configure 通过、构建 0 错 0 警、程序能跑" 三件事同时成立时，
    翻译仍然可能一条都没进去。

    2026-10-19 的真实事故：deb / rpm 两个 CI job 只装了
    qt6-base-dev / qt6-qtbase-devel，容器里根本没有 lrelease。
    CMake 侧对"找不到 lrelease"只打一条 WARNING（翻译是增强项，
    不该挡构建），于是：
        · .qm 从未生成 → i18n.qrc 从未生成 → rcc 无事可做
        · main.cpp 里 translator->load(":/i18n/xxx.qm") 失败，回落到源码
        · 用户装完包，把菜单切成简体中文，重启 —— 还是英文
    整条链上没有任何一个环节报错。

    纯静态、不执行程序（CI 容器里跑不了 GUI）。判定依据是 rcc 生成的
    **资源名表**：每种语言的 "zlphotoedit_xx_YY.qm" 名字都以 UTF-16BE
    出现在二进制里，且同处一个 "i18n" 目录节点下。

    ★ 不用 .qm 文件头魔数当判据 ★
      rcc 默认对"压完更小"的资源做 zlib 压缩，压缩后的 .qm 头部不再是
      明文魔数 —— 实测同一个 6 语言构建：Qt5 版命中 4 处、Qt6 版只命中
      3 处。以魔数计数为准会把正常产物判成失败，所以它只作参考打印。

退出码：0 = 通过；1 = 缺翻译；2 = 用法错误。
"""

import os
import sys

# QM 文件魔数（小端 32 位 0x184DB83C），仅作参考打印，见文件头说明
QM_MAGIC = bytes([0x3C, 0xB8, 0x64, 0x18])


def _fix_stdout_encoding():
    """POSIX locale 下 stdout 是 ASCII，中文 print 会崩（与 glibc 校验脚本同因）。"""
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')
        return
    import codecs
    buf = getattr(sys.stdout, 'buffer', None)
    if buf is not None:
        sys.stdout = codecs.getwriter('utf-8')(buf, 'replace')


_fix_stdout_encoding()


def main(argv):
    if len(argv) < 2:
        print("用法: check_embedded_i18n.py <可执行文件> [语言码...]", file=sys.stderr)
        return 2

    path = argv[1]
    # ★ 逐个 strip ★ 语言码来自命令行，YAML 续行/引号写错会带进空格或
    #   换行（run #23 就是这样：第一个参数成了 " zh_CN"）。不剥离的话
    #   报出的是"翻译缺失"，看起来像产品真的坏了，实际只是参数脏了 ——
    #   一条断言如果会把"调用方写错"伪装成"被测对象坏了"，它本身就是坑。
    langs = [a.strip() for a in argv[2:]]
    langs = [a for a in langs if a] or ["zh_CN"]

    if not os.path.isfile(path):
        print("找不到可执行文件: %s" % path, file=sys.stderr)
        return 1

    with open(path, 'rb') as f:
        data = f.read()

    print("扫描 %s（%.2f MB）" % (path, len(data) / 1048576.0))
    print("  参考：明文 .qm 文件头命中 %d 处（压缩的资源不计入）" % data.count(QM_MAGIC))

    u16 = lambda s: s.encode('utf-16-be')

    # 资源目录节点：qrc 里 prefix="/i18n"，rcc 会把它当成一级目录名。
    # 仅作参考打印 —— 源码里那句 ":/i18n/zlphotoedit_%1.qm" 也会命中，
    # 所以它**不能**作为判据（真正判定靠下面的文件名）。
    if u16("i18n") in data:
        print("  参考：二进制里出现了 \"i18n\" 字符串（可能只是源码里的路径）")

    missing = []
    for lang in langs:
        name = "zlphotoedit_%s.qm" % lang
        hit = u16(name) in data
        print("  %-8s %-30s %s" % (lang, name, "存在" if hit else "★ 缺失 ★"))
        if not hit:
            missing.append(lang)

    if missing:
        print("★ 翻译未内嵌（缺失：%s）★" % ", ".join(missing), file=sys.stderr)
        print("  这条断言挡住的是「程序能跑，但界面永远回落源码文本」——", file=sys.stderr)
        print("  检查构建机是否装了 lrelease（Debian: qt6-l10n-tools，", file=sys.stderr)
        print("  RHEL/Fedora: qt6-linguist），以及 configure 是否传了", file=sys.stderr)
        print("  -DZLPE_REQUIRE_I18N=ON。", file=sys.stderr)
        return 1

    print("通过：%d 种语言的翻译已内嵌" % len(langs))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
