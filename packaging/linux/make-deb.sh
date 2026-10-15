#!/usr/bin/env bash
# ============================================================================
#  make-deb.sh —— 把已构建好的 ZlPhotoEdit 打成 .deb
# ----------------------------------------------------------------------------
#  用法： make-deb.sh <版本号> <可执行文件路径> <输出目录>
#  例：   packaging/linux/make-deb.sh 0.1.0 build/ZlPhotoEdit dist
#
#  ★ 设计要点 ★
#
#    ① **依赖系统 Qt，不打包任何 Qt 库**
#       Depends 由 dpkg-shlibdeps 扫二进制自动生成 —— 它会读到 libQt6Core.so.6
#       里的 Qt_6.10 这类符号版本，自动写出 "libqt6core6 (>= 6.10.2)" 这种
#       **精确到版本**的依赖。手写很容易漏。
#
#    ② 包内容只有三样：可执行文件 / .desktop / 图标
#       这与用户的要求一致（"包里只有我们的程序"）。
#
#    ③ 用 dpkg-deb 手工装包，不引 debhelper
#       debhelper 是"从源码包构建"的重装备（要 dh_make、debian/rules 一堆），
#       而我们的二进制已经编好了，只需要"按 deb 的目录约定摆好 + 写 control"。
#       dpkg-deb -b 一行就够，依赖也少。
# ============================================================================

set -euo pipefail

VER="${1:?用法: make-deb.sh <版本> <可执行文件> <输出目录>}"
BIN="${2:?缺少可执行文件路径}"
OUTDIR="${3:-dist}"

HERE="$(cd "$(dirname "$0")" && pwd)"

# ★★ OUTDIR 必须先转成绝对路径 ★★
#   下面为跑 dpkg-shlibdeps 会 `cd "$STAGE"`，之后再写相对路径就会落到
#   临时目录里去 —— 而 STAGE 在脚本退出时被 trap 删掉，产物**静默消失**。
#   症状：脚本打印"已生成 dist/xxx.deb"，但 CI 的 upload-artifact 说
#   "No files were found with the provided path: dist/*.deb"。
#   这是 CI 上真实撞到的。
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# ── 目录结构（FHS）──
install -Dm755 "$BIN"                          "$STAGE/usr/bin/ZlPhotoEdit"
install -Dm644 "$HERE/zlphotoedit.desktop"     "$STAGE/usr/share/applications/zlphotoedit.desktop"
install -Dm644 "$HERE/zlphotoedit.png"         "$STAGE/usr/share/icons/hicolor/256x256/apps/zlphotoedit.png"

# ── 自动算依赖 ──
#  dpkg-shlibdeps 必须在"有 debian/control 上下文"或显式 -O 下运行。
#  这里用 -O 直接输出到 stdout，避开 debian/ 目录。
cd "$STAGE"
: > /tmp/zlpe-shlibdeps.txt
dpkg-shlibdeps -O "usr/bin/ZlPhotoEdit" > /tmp/zlpe-shlibdeps.txt 2>/dev/null || true
DEPS="$(sed -n 's/^shlibs:Depends=//p' /tmp/zlpe-shlibdeps.txt)"
# 兜底：万一 dpkg-shlibdeps 没跑成（缺 dev 包等），至少保证这几个核心依赖在
if [ -z "$DEPS" ]; then
    echo "  [警告] dpkg-shlibdeps 未产出依赖，回落到手写清单" >&2
    DEPS="libc6 (>= 2.34), libqt6core6t64, libqt6gui6, libqt6widgets6, libqt6svg6, libqt6concurrent6, libqt6printsupport6, libgcc-s1, libstdc++6"
fi

# ── control ──
#  Installed-Size 单位是 KB
SIZE_KB=$(du -sk "$STAGE" | cut -f1)
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: zlphotoedit
Version: $VER
Section: graphics
Priority: optional
Architecture: amd64
Maintainer: Zoulei-CSU <zoulei.csu@gmail.com>
Installed-Size: $SIZE_KB
Depends: $DEPS
Homepage: https://github.com/Zoulei-CSU/ZlPhotoEdit
Description: 逐鹿 图片编辑器 —— 轻量级图片编辑器
 基于 Qt 的轻量级图片编辑器：多图层与 22 种混合模式、矩形/椭圆/羽化选区、
 魔术棒、15 个工具、曲线/色阶/亮度对比度、旋转翻转裁剪缩放、多标签页、
 标尺/参考线/网格、另存为 ICO、屏幕截图。
 .
 纯 Qt 实现，不依赖任何第三方图像库；主程序不到 2 MB（不含 Qt 库）。
EOF

# ── 打包 ──
#  ★ OUTDIR 已在开头转成绝对路径（见那段注释）★
DEB="$OUTDIR/zlphotoedit_${VER}_amd64.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
echo "已生成 $DEB"
ls -lh "$DEB"
dpkg-deb -I "$DEB" | sed -n '1,30p'
