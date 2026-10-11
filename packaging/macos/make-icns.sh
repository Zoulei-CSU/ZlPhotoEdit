#!/usr/bin/env bash
# ============================================================================
#  make-icns.sh —— 从 resources/logo.ico 生成 logo.icns（macOS 应用图标）
# ----------------------------------------------------------------------------
#  用法： bash packaging/macos/make-icns.sh
#  只在 macOS 上跑（依赖系统自带的 sips 与 iconutil）。
#
#  ★ 为什么从 .ico 转而不是从 .svg ★
#
#    本机与 CI 里都没有可靠的 SVG → PNG 栅格化工具链
#    （rsvg-convert / cairosvg / ImageMagick 都要额外装，且版本差异大）。
#    而 logo.ico 里**已经有一张 256×256 的位图**（用户提供的成品），
#    用它来生成各尺寸 PNG 最省事、也最可复现 —— 跑在两个平台上结果一致。
#
#  ★ iconutil 的输入约定 ★
#
#    它要一个 `xxx.iconset/` 目录，文件名必须是 icon_<尺寸>x<尺寸>.png 或
#    icon_<尺寸>x<尺寸>@2x.png。少一张不会报错，但对应尺寸在系统里就糊。
# ============================================================================

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC_ICO="$ROOT/resources/logo.ico"
OUT_ICNS="$HERE/logo.icns"

if [ ! -f "$SRC_ICO" ]; then
    echo "[错误] 找不到 $SRC_ICO" >&2
    exit 1
fi

WORK="$(mktemp -d)"
ICONSET="$WORK/ZlPhotoEdit.iconset"
mkdir -p "$ICONSET"
trap 'rm -rf "$WORK"' EXIT

# ── .ico → 一张 256×256 PNG（用 macOS 自带的 sips）──
#  sips 直接读 .ico 的多尺寸资源，-s format png 转出来即可。
BASE="$WORK/base.png"
sips -s format png "$SRC_ICO" --out "$BASE" >/dev/null

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
# 1024 会从 256 放大 —— 会有点糊，但 macOS 需要这一档，宁可糊也别缺
gen 1024 "icon_512x512@2x.png"

iconutil -c icns "$ICONSET" -o "$OUT_ICNS"
echo "已生成 $OUT_ICNS  ($(stat -f%z "$OUT_ICNS") bytes)"