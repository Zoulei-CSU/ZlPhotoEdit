#!/usr/bin/env bash
# ============================================================================
#  make-rpm.sh —— 把已构建好的 ZlPhotoEdit 打成 .rpm
# ----------------------------------------------------------------------------
#  用法： make-rpm.sh <版本号> <可执行文件路径> <输出目录>
#
#  ★ 必须在 RPM 系容器里跑 ★（fedora:43）
#    rpmbuild 依赖 rpm-build / redhat-rpm-config 等，Ubuntu 上装不齐。
# ============================================================================

set -euo pipefail

VER="${1:?用法: make-rpm.sh <版本> <可执行文件> <输出目录>}"
BIN="${2:?缺少可执行文件路径}"
OUTDIR="${3:-dist}"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

# ── 汇总"待安装"的产物到一处，交给 spec 的 %{_stage} ──
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp "$BIN"                     "$STAGE/ZlPhotoEdit"
cp "$HERE/zlphotoedit.desktop" "$STAGE/zlphotoedit.desktop"
cp "$HERE/zlphotoedit.png"     "$STAGE/zlphotoedit.png"
cp "$ROOT/README.md"           "$STAGE/README.md"

# ── 一个空的 Source0（spec 里声明了它，但 %prep 不解压）──
TOPSRC="$(mktemp -d)"
mkdir -p "$TOPSRC/rpmbuild"
tar czf "$TOPSRC/rpmbuild/zlphotoedit-${VER}.tar.gz" --files-from /dev/null

mkdir -p "$OUTDIR"

rpmbuild -bb \
    --define "_ver ${VER}" \
    --define "_stage ${STAGE}" \
    --define "_topdir ${TOPSRC}/rpmbuild" \
    --define "_srcrpmdir ${TOPSRC}/rpmbuild/SRPMS" \
    --define "_rpmdir ${OUTDIR}" \
    "$HERE/zlphotoedit.spec"

echo "── 产物 ──"
find "$OUTDIR" -name '*.rpm' -exec ls -lh {} \;
echo "── 自动生成的依赖（前 20 条）──"
RPMFILE="$(find "$OUTDIR" -name '*.rpm' | head -1)"
rpm -qpR "$RPMFILE" 2>/dev/null | head -20 || true