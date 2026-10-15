#!/usr/bin/env bash
# ============================================================================
#  make-icns.sh —— 从 resources/logo.png 生成 logo.icns（macOS 应用图标）
# ----------------------------------------------------------------------------
#  用法： bash packaging/macos/make-icns.sh
#  只在 macOS 上跑（依赖系统自带的 sips 与 iconutil）。
#
#  ★ 为什么从 .png 转 ★
#
#     2026-10-15 起，应用图标统一用一张 512×512 的成品位图 logo.png：
#     qrc 里的窗口图标、Linux 的 256×256 桌面图标、本脚本的 .icns
#     全部由它派生 —— 只有一个真相源。它比 logo.ico 里那张 256×256
#     大一倍，512 档是原生尺寸、不用放大。
#
#     （历史上这里从 .ico 转：那时没有现成的 PNG 栅格化工具链，
#      而 .ico 里恰好已经有一张 256 位图。现在 logo.png 就在手边。）
#
#  ★ iconutil 的输入约定 ★
#
#    它要一个 `xxx.iconset/` 目录，文件名必须是 icon_<尺寸>x<尺寸>.png 或
#    icon_<尺寸>x<尺寸>@2x.png。少一张不会报错，但对应尺寸在系统里就糊。
# ============================================================================

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC_PNG="$ROOT/resources/logo.png"
OUT_ICNS="$HERE/logo.icns"

if [ ! -f "$SRC_PNG" ]; then
    echo "[错误] 找不到 $SRC_PNG" >&2
    exit 1
fi

WORK="$(mktemp -d)"
ICONSET="$WORK/ZlPhotoEdit.iconset"
mkdir -p "$ICONSET"
trap 'rm -rf "$WORK"' EXIT

# ── 源图就是 logo.png（512×512），直接交给 sips 缩各档 ──
BASE="$SRC_PNG"

echo "源图标尺寸：$(sips -g pixelWidth -g pixelHeight "$BASE" | tail -2 | tr -d ' \n')"

# ── 生成 iconutil 需要的各尺寸 ──
gen() {  # gen <目标像素> <文件名>
    sips -z "$1" "$1" "$BASE" --out "$ICONSET/$2" >/dev/null
}
gen 16   "icon_16x16.png"
gen 32   "icon_16x16@2x.png"
gen 32   "icon_32x32.png"
gen 64   "icon_32x32@2x.png"
gen 128  "icon_128x128.png"
gen 256  "icon_128x128@2x.png"
gen 256  "icon_256x256.png"
gen 512  "icon_256x256@2x.png"
gen 512  "icon_512x512.png"
# 1024 会从 512 放大 —— 会有点糊，但 macOS 需要这一档，宁可糊也别缺
gen 1024 "icon_512x512@2x.png"

iconutil -c icns "$ICONSET" -o "$OUT_ICNS"
echo "已生成 $OUT_ICNS  ($(stat -f%z "$OUT_ICNS") bytes)"