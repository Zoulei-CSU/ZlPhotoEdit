#!/usr/bin/env bash
# ============================================================================
#  build_env.sh —— 在 Git Bash 里手工构造 MSVC 编译环境
# ----------------------------------------------------------------------------
#  为什么不用 vcvars64.bat：
#    它是批处理脚本，靠 cmd.exe 执行并修改环境变量。在 Git Bash 里
#    "call" 它不会把变量导出来（子 shell 的环境不回流）。
#
#  所以这里手工拼出 cl.exe 需要的三样东西：
#    ① PATH  —— cl.exe / link.exe 所在目录
#    ② INCLUDE —— CRT + Windows SDK + MSVC 头文件目录
#    ③ LIB   —— 对应的 .lib 目录
#  这三样齐了，cl 就能工作（qmake 的 "Cannot run compiler 'cl'" 就是这个原因）。
#
#  用法： source build_env.sh
# ============================================================================

# ★ 第一件事：把 PortableGit 的 usr/bin 加进 PATH ★
#   这个 bash 会话的 PATH 里默认没有 dirname / cat / tr / xargs 等
#   基础工具（它们不在系统 PATH 里），所以下面所有的文本处理命令
#   都不可用。必须先把它们找回来。
GIT_USR_BIN="/c/Users/zoulei/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin"
if [ -d "$GIT_USR_BIN" ]; then
    export PATH="$GIT_USR_BIN:$PATH"
fi

VS_DIR="/c/Program Files/Microsoft Visual Studio/2022/Professional"
QT_DIR="/c/Qt/5.15.16-VS2022-rel"
MSVC_VER="$(cat "$VS_DIR/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" | tr -d '\r\n')"
# 取 SDK 版本号：直接用 ls -d 得到 ".../10.0.26100.0"，再截取最后一段。
# ★ 不能对含空格的路径用 xargs basename ★（"Program Files (x86)" 会被拆开）
SDK_VER="$(ls -d "/c/Program Files (x86)/Windows Kits/10/Include/"* 2>/dev/null | tail -1)"
SDK_VER="${SDK_VER##*/}"

MSVC_BIN="$VS_DIR/VC/Tools/MSVC/$MSVC_VER/bin/Hostx64/x64"
SDK_DIR="/c/Program Files (x86)/Windows Kits/10"

# ── PATH ──
export PATH="$MSVC_BIN:$QT_DIR/bin:/c/Windows/System32:/c/Windows:$PATH"

# ★ rc.exe（资源编译器）必须单独加进 PATH ★
#   链接阶段生成 .exe 时需要它（把 .rc 编成 .res）。
#   它不在 MSVC 的 bin 目录里，而在 Windows SDK 的 bin 下，
#   而且按主机架构分目录（x64\ 是 64 位版本）。
#   漏加的症状是 "LINK : fatal error LNK1158: 无法运行 rc.exe" ——
#   编译全都成功，只在最后一步链接时失败。
export PATH="$SDK_DIR/bin/$SDK_VER/x64:$PATH"

# ── INCLUDE ──
#  ★ 必须用 Windows 形式（D:\... 或 C:/...）★
#    cl.exe 不认识 MSYS 的 "/c/Program Files/..." 形式 ——
#    给出的报错是 "fatal error C1083: 无法打开包括文件 type_traits"，
#    看起来像是"缺了标准库"，实际是路径形式不对。
#    这里统一转成 "C:/..." 的混合形式（正斜杠 MSVC 可以接受，
#    反斜杠在 bash 里还要转义，所以用正斜杠）。
WIN_VS="C:/Program Files/Microsoft Visual Studio/2022/Professional"
WIN_SDK="C:/Program Files (x86)/Windows Kits/10"

export INCLUDE="$WIN_VS/VC/Tools/MSVC/$MSVC_VER/include"
export INCLUDE="$INCLUDE;$WIN_SDK/Include/$SDK_VER/ucrt"
export INCLUDE="$INCLUDE;$WIN_SDK/Include/$SDK_VER/shared"
export INCLUDE="$INCLUDE;$WIN_SDK/Include/$SDK_VER/um"
export INCLUDE="$INCLUDE;$WIN_SDK/Include/$SDK_VER/winrt"

# ── LIB ──
export LIB="$WIN_VS/VC/Tools/MSVC/$MSVC_VER/lib/x64"
export LIB="$LIB;$WIN_SDK/Lib/$SDK_VER/ucrt/x64"
export LIB="$LIB;$WIN_SDK/Lib/$SDK_VER/um/x64"

# ── CMake 与 Ninja ──
#  ★ 它们不单独安装，而是随 VS 2022 一起发布的（"使用 C++ 的桌面开发"工作负载）。
#    所以必须显式把这个目录加进 PATH，否则 cmake --build 会报 command not found。
#    同理，cmake 自己要能找到 ninja（在同一个目录里），生成器才可用。
CMAKE_DIR="$VS_DIR/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
NINJA_DIR="$VS_DIR/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
if [ -d "$CMAKE_DIR" ]; then
    export PATH="$CMAKE_DIR:$PATH"
fi
if [ -d "$NINJA_DIR" ]; then
    export PATH="$NINJA_DIR:$PATH"
fi

# ── Git Bash 特有：把编译器用到的路径形式转换关掉 ──
#   MSYS 会把 "/DX" 这类命令行参数误当成 Unix 路径改写，
#   加了这个环境变量可以避免（qmake 生成的命令里有很多 /D 选项）。
export MSYS2_ARG_CONV_EXCL="*"

echo "[env] MSVC  : $MSVC_VER"
echo "[env] SDK   : $SDK_VER"
echo "[env] Qt    : $QT_DIR"
echo "[env] cl     -> $(command -v cl || echo '未找到')"
echo "[env] nmake  -> $(command -v nmake || echo '未找到')"
echo "[env] qmake  -> $(command -v qmake || echo '未找到')"
echo "[env] cmake  -> $(command -v cmake || echo '未找到')"
echo "[env] ninja  -> $(command -v ninja || echo '未找到')"
