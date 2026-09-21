#!/usr/bin/env bash
# ============================================================================
#  run_tests.sh —— 编译并运行 tests/ 下的离线自检
# ----------------------------------------------------------------------------
#  为什么需要这个脚本：
#
#    tests/ 目录**不属于应用工程** —— ZlPhotoEdit.pro 与 CMakeLists.txt
#    都只收集 src/ 下的文件。测试程序是独立编译的小可执行文件，
#    需要手工给编译器一串 Qt 的 include/lib 路径。
#    把这条命令固化下来，避免每次现查路径。
#
#  前提：先 source build_env.sh 构造 MSVC 环境（cl.exe 在 PATH 里）。
#
#  用法：
#      source build_env.sh
#      ./run_tests.sh              # 跑全部（含一条不编译的静态对账）
#      ./run_tests.sh brush        # 只跑某一个
#      ./run_tests.sh fill         # 只跑油漆桶填充（扫描线 / 容差 / 连通性）
#      ./run_tests.sh toolpaint    # 只跑吸管 + 油漆桶（判据来源 / 撤销粒度 / 不透明度）
#      ./run_tests.sh shape        # 只跑直线 / 矩形 / 椭圆（蒙版形状 / 拖影 / 撤销粒度）
#      ./run_tests.sh handzoom     # 只跑抓手 / 缩放（位移坐标系 / 阈值 / 框选矩形）
#      ./run_tests.sh text         # 只跑文字排版引擎（对齐 / 行距 / 画布裁剪 / 抗锯齿）
#      ./run_tests.sh texttool     # 只跑文字工具（浮层协议 / 落像素 / 撤销粒度）
#      ./run_tests.sh crop         # 只跑裁剪工具（含离屏像素量化）
#      ./run_tests.sh icon         # 只跑 SVG 图标（逐图标量像素 + 接触表）
#      ./run_tests.sh layer_panel # 只跑图层面板（行序映射 / 热点 / 拖拽 / 自绘像素）
#      ./run_tests.sh tabs        # 只跑多标签容器（三件套绑定 / 摘页账本 / 每页独立撤销栈）
#      ./run_tests.sh mainwin      # 只跑主窗口装配（最慢：要编整个 src/）
#      ./run_tests.sh actions      # 只跑静态对账（菜单动作，秒回）
#
#  新增测试要改**三处**，少改一处就会静默地"少做事"：
#      ① run_one() 里加一个短名分支，写清它依赖哪些 src 文件；
#         （依赖只列**真正需要编译的 .cpp** —— 测试链接的是源码而不是 lib，
#           多余的依赖只会让编译变慢。）
#      ② 如果测试名（可执行文件名）需要 moc 或 Qt5Widgets/Qt5Svg/qrc，
#         在 moc_headers_for / extra_libs_for / needs_resources_for 里各加一行；
#      ③ 文件末尾那段"全量清单"里加一句 run_one <短名>，
#         否则 ./run_tests.sh（不带参数）不会跑它。
# ============================================================================

set -e

QT_DIR="${QT_DIR:-/c/Qt/5.15.16-VS2022-rel}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── MSYS 路径 → Windows 路径 ──
#
# ★ 这一步不能省 ★
#   cl.exe 完全不认识 MSYS 形式的 "/d/Code/..." —— 它会把整个路径
#   当成一个命令行选项，报出极具误导性的错误：
#       cl: 命令行 warning D9002 :忽略未知选项"/d/Code/.../Brush.cpp"
#       cl: 命令行 error D8003 :缺少源文件名
#   看起来像"没给源文件"，实际是路径形式不对。
#   这与 build_env.sh 里 INCLUDE/LIB 必须用 C:/... 是同一个坑。
#
#   cygpath -m 给出的 "D:/Code/ZlPhotoEdit" 是混合形式：
#   盘符用 Windows 写法、分隔符用正斜杠。正斜杠 MSVC 接受，
#   而且不用像反斜杠那样在 shell 里转义。
to_win() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        # 没有 cygpath 时的降级：把 /c/... 换成 C:/...
        echo "$1" | sed -E 's|^/([a-zA-Z])/|\U\1:/|'
    fi
}

QT_WIN="$(to_win "$QT_DIR")"
MOC_WIN="$QT_WIN/bin/moc.exe"
RCC_WIN="$QT_WIN/bin/rcc.exe"
ROOT_WIN="$(to_win "$ROOT")"
OUT_DIR="$ROOT/tests/build"

if ! command -v cl >/dev/null 2>&1; then
    echo "错误：找不到 cl.exe。请先执行： source build_env.sh" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

# 全局计数器：全部测试跑完后据此决定退出码
TOTAL_PASSED=0
TOTAL_FAILED=0

# ---------------------------------------------------------------------------
#  按测试名提供"额外需要"的两样东西
# ---------------------------------------------------------------------------
#  默认情况下测试只编译明确列出的 .cpp、只链接 Qt5Core + Qt5Gui。
#  但 ui 层的测试需要两样额外的东西：
#
#    · moc：带 Q_OBJECT 的类，其 metaObject()/qt_metacall() 由 moc 生成的
#      .cpp 提供。漏了这一趟会在链接时报"无法解析的外部符号
#      public: virtual struct QMetaObject const * ..."，看起来像
#      "少编译了某个 .cpp"，实际是漏了 moc。
#    · Qt5Widgets.lib：ui 层当然需要它。
#
#  ★ 把这两样做成"按测试名按需开启"而不是全局打开 ★
#    是为了保住那条纪律：**engine / core 层的测试永远只链接 Core + Gui**。
#    哪天有人往 engine 里塞进 QWidget 依赖，那些测试会立刻编译失败，
#    而不是等到运行期才发现。这份守门人的价值见 docs/自检与验证.md。
# ---------------------------------------------------------------------------
#  ★ 这里按的是**可执行文件名**（build_and_run 的第一个参数），
#    不是 run_one 分派时的短名 —— 两者写得不一致就会静默地少做事
#    （moc 没跑 → 链接报一堆 QMetaObject 相关的未解析符号；
#     lib 没加 → 链接报一堆 QComboBox/QFormLayout 的未解析符号）。
moc_headers_for() {
    case "$1" in
        ui_smoke_test)
            echo "src/ui/AdjustmentDialog.h \
                  src/ui/BrightnessContrastDialog.h \
                  src/ui/HueSaturationDialog.h \
                  src/ui/CurveEditor.h \
                  src/ui/CurvesDialog.h \
                  src/ui/LevelsWidget.h \
                  src/ui/LevelsDialog.h \
                  src/ui/SizeChangeDialogBase.h \
                  src/ui/ImageSizeDialog.h \
                  src/ui/CanvasSizeDialog.h \
                  src/ui/RotateImageDialog.h"
            ;;
        geometry_test)
            # Document 带 Q_OBJECT：构造一个 Document 就需要它的
            # vtable，也就需要 moc 生成的 metaObject()/qt_metacall()。
            # 其余被 include 的类型（Layer / LayerList / Selection）
            # 都是纯值类型，不需要 moc。
            echo "src/core/Document.h"
            ;;
        layer_panel_test)
            # 面板这一侧有三个带 Q_OBJECT 的类（LayerPanel / LayerListWidget /
            # LayerItemDelegate）都在同一个头文件里；再加 Document
            # （要真的建文档来验"面板 ⇄ 文档同步"）。
            echo "src/ui/LayerPanel.h \
                  src/core/Document.h"
            ;;
        view_test)
            # CanvasView / Document / AbstractTool 都带 Q_OBJECT
            # （CanvasView 与 Document 用信号槽，AbstractTool 是 QObject 子类）。
            # 前面几个测试没用到它们，所以只有这个测试需要 moc。
            echo "src/ui/CanvasView.h \
                  src/core/Document.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolOptions.h"
            ;;
        tool_paint_test)
            # BucketTool / ColorPickerTool 都是 QObject 子类（有信号要有 vtable），
            # AbstractTool 是它们的基类、ToolOptions 里有 BucketOptions /
            # PickerOptions 的 changed / colorChanged 信号、Document 有信号槽、
            # **CommandStack 也带 Q_OBJECT**（它转发 QUndoStack 的信号）。
            # 漏掉任何一个都只在**链接期**报"无法解析的外部符号 … metaObject"，
            # 看起来像"少编译了某个 .cpp"，实际只是没跑 moc。
            echo "src/tools/BucketTool.h \
                  src/tools/ColorPickerTool.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolOptions.h \
                  src/commands/CommandStack.h \
                  src/core/Document.h"
            ;;
        crop_test)
            # CropTool 是 QObject 子类（有 cropRequested 信号）；
            # AbstractTool 是它的基类、ToolOptions 里 CropOptions 有 changed 信号、
            # Document 有信号槽 —— 四者的 vtable 都要 moc 生成的
            # metaObject()/qt_metacall()。漏掉任何一个都会在**链接期**报
            # "无法解析的外部符号 ... vtable"，而不是编译期。
            echo "src/tools/CropTool.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolOptions.h \
                  src/core/Document.h"
            ;;
        shape_tool_test)
            # ShapeTool 是 QObject 子类（三个 Kind 一个类）；基类 AbstractTool、
            # ToolOptions（LineOptions / ShapeOptions 两个子类都有自己的信号）、
            # 以及 **CommandStack**（撤销粒度那组断言要看 count()，它带 Q_OBJECT）
            # 和 Document 都要 moc。漏一个只在**链接期**报未解析符号。
            echo "src/tools/ShapeTool.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolOptions.h \
                  src/commands/CommandStack.h \
                  src/core/Document.h"
            ;;
        hand_zoom_test)
            # HandTool / ZoomTool 都是 QObject 子类（要发视图请求信号）；
            # ZoomTool 的选项对象 ZoomOptions 也有信号（changed），
            # 基类 AbstractTool、以及"不碰文档"那组要构造的
            # CommandStack / Document 都要 moc。
            # 漏一个只在**链接期**报未解析符号（vtable / metaObject）。
            echo "src/tools/HandTool.h \
                  src/tools/ZoomTool.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolOptions.h \
                  src/commands/CommandStack.h \
                  src/core/Document.h"
            ;;
        text_tool_test)
            # TextTool 是 QObject 子类（要发 requestTextEditor 信号）；
            # 基类 AbstractTool、ToolOptions（TextOptions 有 colorChanged 信号）、
            # **CommandStack**（撤销粒度那组要看 count()）与 Document 都要 moc。
            # 漏一个只在**链接期**报未解析符号（vtable / metaObject），
            # 看起来像"少编译了某个 .cpp"，其实只是没跑 moc。
            echo "src/tools/TextTool.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolOptions.h \
                  src/commands/CommandStack.h \
                  src/core/Document.h"
            ;;
        document_tabs_test)
            # 容器自己带 Q_OBJECT（closeTabRequested 信号）；它 new 出来的
            # 三类对象也都带 Q_OBJECT：CanvasView、CommandStack、Document，
            # 外加 document_tabs_test.cpp 里那个撤销命令的基类 QUndoCommand
            # （Qt 自带，不用 moc）。
            # ★ DocumentManager.h 也在列表里 ★ 它是 QObject 子类
            #   （currentChanged / documentRemoved 等信号），
            #   而本组要真的构造它来验 indexOf。漏掉只在**链接期**报
            #   "无法解析的外部符号 … vtable / metaObject"，
            #   看起来像"少编译了某个 .cpp"，其实只是没跑 moc。
            # 另外 CanvasView.h 里按值用到了 ToolEvent，AbstractTool.h /
            # ToolOptions.h 是它的基类与选项对象 —— 一起 moc 上。
            echo "src/ui/DocumentTabs.h \
                  src/ui/CanvasView.h \
                  src/commands/CommandStack.h \
                  src/core/Document.h \
                  src/core/DocumentManager.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolOptions.h"
            ;;
        mainwin_test)
            # ★ 唯一真正构造 MainWindow 的测试 ★
            #   它把整个应用的头文件都引到了（MainWindow 的成员类型），
            #   所以**所有带 Q_OBJECT 的头都要 moc**。
            #   漏掉任何一个都只在**链接期**报
            #   "无法解析的外部符号 … vtable / metaObject"，
            #   看起来像"少编译了某个 .cpp"，其实只是没跑 moc。
            echo "src/core/Document.h \
                  src/core/DocumentManager.h \
                  src/commands/CommandStack.h \
                  src/tools/AbstractTool.h \
                  src/tools/ToolManager.h \
                  src/tools/ToolOptions.h \
                  src/tools/MoveTool.h \
                  src/tools/BrushTool.h \
                  src/tools/SelectionTool.h \
                  src/tools/CropTool.h \
                  src/tools/BucketTool.h \
                  src/tools/ColorPickerTool.h \
                  src/tools/ShapeTool.h \
                  src/tools/HandTool.h \
                  src/tools/ZoomTool.h \
                  src/tools/TextTool.h \
                  src/ui/MainWindow.h \
                  src/ui/CanvasView.h \
                  src/ui/DocumentTabs.h \
                  src/ui/RulerWidget.h \
                  src/ui/ToolOptionsBar.h \
                  src/ui/StatusBarWidget.h \
                  src/ui/LayerPanel.h \
                  src/ui/AdjustmentDialog.h \
                  src/ui/BrightnessContrastDialog.h \
                  src/ui/HueSaturationDialog.h \
                  src/ui/CurveEditor.h \
                  src/ui/CurvesDialog.h \
                  src/ui/LevelsWidget.h \
                  src/ui/LevelsDialog.h \
                  src/ui/SizeChangeDialogBase.h \
                  src/ui/ImageSizeDialog.h \
                  src/ui/CanvasSizeDialog.h \
                  src/ui/RotateImageDialog.h"
            ;;
        *)
            echo ""
            ;;
    esac
}

extra_libs_for() {
    case "$1" in
        # 图标要用 QSvgRenderer 自己光栅化 → 必须真链 Qt5Svg
        icon_test|mainwin_test|layer_panel_test)
                                           echo "Qt5Widgets.lib Qt5Svg.lib" ;;
        ui_smoke_test|view_test|crop_test|tool_paint_test|shape_tool_test|hand_zoom_test|text_tool_test|document_tabs_test)
                                           echo "Qt5Widgets.lib" ;;
        *)                                 echo "" ;;
    esac
}

# ---------------------------------------------------------------------------
#  哪些测试需要把 resources.qrc 编进去
# ---------------------------------------------------------------------------
#  ★ 为什么按测试名按需开启，而不是所有测试都带上 ★
#    qrc 编进来意味着那个测试会"看见"资源 —— 这本身是不该被随便放宽的前提。
#    只有真正会去取图标的测试才需要它：
#      · icon_test    —— 全部断言都在图标上
#      · mainwin_test —— 断言"每个已注册工具都有真实 SVG 图标，不是字母占位"
#    其余测试（engine / core / 对话框）不该依赖资源存在。
#
#  rcc 的输出是一个普通的 .cpp（二进制数组 + 一段静态注册代码），
#  把它当作一个源文件编进去即可 —— 等价于 CMake 的 AUTORCC。
# ---------------------------------------------------------------------------
needs_resources_for() {
    case "$1" in
        # 要读 qrc 里的 SVG：图标自检、主窗口装配、图层面板
        # （面板行上的眼睛/锁图标同样来自 resources.qrc）
        icon_test|mainwin_test|layer_panel_test) return 0 ;;
        *)                                       return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
#  防假绿的通用工具：用"修改时间变新"代替"先删旧文件"
# ---------------------------------------------------------------------------
#  ★ 为什么不用 rm ★
#    `rm -f 旧产物` 依赖"允许删除"这个前提。在受限/沙箱环境里批量删除
#    会被安全策略整体拦截，整条流水线当场停摆 —— 自检脚本恰恰是最不该
#    依赖环境的那一类脚本。而且删除只是"事后无法伪装"，不够直接。
#
#  ★ 为什么 mtime 更严格 ★
#    moc / rcc / cl 都是**只在成功时才写出文件**。记下跑之前的 mtime，
#    跑完必须变新：没变新 = 这次没写出新产物 = 接下来用的是旧的。
#    这一条同时覆盖两种伪装：① 工具失败但旧文件还在；② 工具"成功"
#    但内容其实是上一版（被别的进程锁住没覆盖等）。
#
#  唯一前提：NTFS 的 mtime 至少是秒级精度 —— 同秒内重新生成会读不出来，
#  所以这里用 `-le`（不大于）判失败，而不是 `-ne`：只要"没有变新"就报。
#  实测一整轮全量自检远不止一秒，不会误报。
# ---------------------------------------------------------------------------

# 取文件修改时间（秒）；不存在时返回 0
file_mtime() {
    if [ -f "$1" ]; then
        stat -c %Y "$1" 2>/dev/null || echo 0
    else
        echo 0
    fi
}

# require_fresh <产物路径> <跑之前的 mtime> <名称> <日志文件>
# 产物必须①存在 ②mtime 变新；否则打印日志并把失败计数加一。
require_fresh() {
    local path="$1" before="$2" what="$3" logf="$4"
    if [ ! -f "$path" ]; then
        echo "$what 失败，日志：" >&2
        cat "$logf" >&2
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        return 1
    fi
    local now
    now="$(file_mtime "$path")"
    if [ "$now" -le "$before" ]; then
        echo "★ 可疑：$what 没有写出新文件（mtime 未变新：$before → $now）" >&2
        echo "  旧产物会被当成本次结果，属于假绿隐患。日志：" >&2
        cat "$logf" >&2
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
#  build_and_run <测试名> <源文件相对路径...>
# ---------------------------------------------------------------------------
#  只链接 Qt5Core 与 Qt5Gui —— 被测的都是 engine / core 层的纯逻辑，
#  不该也不需要 QtWidgets。这也顺带验证了分层没有被破坏：
#  如果哪天有人往 engine 或 core 的头文件里塞进 QWidget 依赖，
#  这些测试会立刻编译失败，而不是等到某天有人抱怨"engine 层编译变慢"。
#
#  例外见上面的 moc_headers_for / extra_libs_for（ui 层测试需要 Widgets）。
# ---------------------------------------------------------------------------
build_and_run() {
    local name="$1"; shift
    local srcs=("$@")

    echo ""
    echo "================================================================"
    echo "  编译并运行 $name"
    echo "================================================================"

    local args=()
    for s in "${srcs[@]}"; do
        args+=("$ROOT_WIN/$s")
    done

    # ── moc 预处理（只对声明了的测试做）──
    local moc_args=()
    local h
    for h in $(moc_headers_for "$name"); do
        local moc_out="$OUT_DIR/moc_$(basename "$h" .h).cpp"
        echo "  moc $(basename "$h")"
        # 同样用 mtime 判"这次真的重新生成了"，不删旧文件（见文件上方的说明）
        local moc_before
        moc_before="$(file_mtime "$moc_out")"
        "$MOC_WIN" \
            "-I$ROOT_WIN/src" \
            "-I$QT_WIN/include" \
            "-I$QT_WIN/include/QtCore" \
            "-I$QT_WIN/include/QtGui" \
            "-I$QT_WIN/include/QtWidgets" \
            "$ROOT_WIN/$h" -o "$(to_win "$moc_out")" \
            > "$OUT_DIR/$name.moc.log" 2>&1 || true

        require_fresh "$moc_out" "$moc_before" \
            "moc（$h）" "$OUT_DIR/$name.moc.log" || return 1
        moc_args+=("$(to_win "$moc_out")")
    done

    # ── 资源（rcc）预处理，同样只对声明了的测试做 ──
    local rcc_args=()
    if needs_resources_for "$name"; then
        local rcc_out="$OUT_DIR/qrc_resources.cpp"
        echo "  rcc resources.qrc"
        # 与 moc 同理：用 mtime 判"重新生成了"，不删旧产物
        local rcc_before
        rcc_before="$(file_mtime "$rcc_out")"
        "$RCC_WIN" "$ROOT_WIN/resources/resources.qrc" \
            -o "$(to_win "$rcc_out")" > "$OUT_DIR/$name.rcc.log" 2>&1 || true
        require_fresh "$rcc_out" "$rcc_before" \
            "rcc" "$OUT_DIR/$name.rcc.log" || return 1
        rcc_args+=("$(to_win "$rcc_out")")
    fi

    # ── ★ 编译前先让旧的可执行文件"失去时效" ★ ──────────────────────
    #
    #  没有这一步，"编译失败"会伪装成"测试通过"：
    #    cl 用 /Fe: 指定输出，**只有编译链接成功才会写出 .exe**。
    #    如果上一次构建留下的 .exe 还在，编译失败后下面那句
    #    `[ ! -f "$name.exe" ]` 判定为假（文件存在），于是脚本高高兴兴地
    #    去运行**上一次的旧二进制**，打印出上一次的"通过 N 项"，
    #    整个套件显示全绿 —— 而这次改的代码其实一行都没编进去。
    #
    #  这是自检脚本最危险的一类 bug：它不报错，它给你假的好消息。
    #  实测踩过：新加 13 条断言后仍显示原来的 93 项通过。
    #
    #  ── 为什么这里不写 `rm -f` ──
    #    `rm` 用起来最直接，但它**依赖"允许删除"这个前提**：
    #    在受限环境里批量删除会被安全策略整体拦截（实测一次全量跑要删
    #    6 个 exe + 若干 moc/rcc 中间件，直接触发批量删除确认、
    #    整条流水线当场停摆）。自检脚本必须能在"什么都不能删"的环境里
    #    照常工作，否则它就成了最不该成为的东西 —— 一个环境敏感的脚本。
    #
    #  ── 替代方案：比对修改时间（更直接，还更严格）──
    #    `rm` 只是"事后无法伪装"；记时间戳是"当场识破"。
    #    编译前记一次 mtime，编译后必须**变新**，否则说明这次根本没写出
    #    新二进制，"通过 N 项"是旧的 —— 这时才判失败。
    #    顺带覆盖了另一种伪装：exe 存在、但内容还是上一版
    #    （例如 /Fe: 成功写了个旧缓存，或文件被别的进程锁住没覆盖）。
    local exe_path="$OUT_DIR/$name.exe"
    local mtime_before
    mtime_before="$(file_mtime "$exe_path")"

    cl /nologo /utf-8 /std:c++17 /EHsc /MD /O2 /DNDEBUG \
       /DWIN32 /DUNICODE /D_UNICODE \
       /I "$ROOT_WIN/src" \
       /I "$QT_WIN/include" \
       /I "$QT_WIN/include/QtCore" \
       /I "$QT_WIN/include/QtGui" \
       /I "$QT_WIN/include/QtWidgets" \
       /I "$QT_WIN/include/QtSvg" \
       "${args[@]}" \
       "${moc_args[@]}" \
       "${rcc_args[@]}" \
       "/Fo:$(to_win "$OUT_DIR")/" \
       "/Fe:$(to_win "$OUT_DIR")/$name.exe" \
       /link "/LIBPATH:$QT_WIN/lib" Qt5Core.lib Qt5Gui.lib $(extra_libs_for "$name") \
       > "$OUT_DIR/$name.build.log" 2>&1 || true

    # ★ 防假绿：exe 必须①存在 ②比编译前更新 ★
    #   时间没变 = 这次没写出新二进制 = 接下来跑的是旧的那一份，
    #   它打印的"通过 N 项"不能代表本次改动。
    if [ ! -f "$exe_path" ]; then
        echo "编译失败，日志如下：" >&2
        cat "$OUT_DIR/$name.build.log" >&2
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        return 1
    fi
    if [ "$(file_mtime "$exe_path")" -le "$mtime_before" ]; then
        echo "★ 可疑：$name.exe 的修改时间没有变新（$mtime_before → $(file_mtime "$exe_path")）" >&2
        echo "  说明这次编译没有写出新二进制，下面要跑的是上一次的旧文件 ——" >&2
        echo "  它打印的「通过 N 项」不能代表本次改动。判定为失败。" >&2
        echo "  构建日志末尾：" >&2
        tail -20 "$OUT_DIR/$name.build.log" >&2
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        return 1
    fi

    # 需要 Qt 的 DLL 在 PATH 里
    export PATH="$QT_DIR/bin:$PATH"
    # TEST_ARGS_ARR 允许把参数透传给测试程序本身
    #   （./run_tests.sh view -- "/path/有 空格 的图.jpg"）
    # ★ 必须用数组而不是把 "$*" 拼成一个字符串 ★
    #   拼字符串后再展开会按空格重新分词，路径里带空格或中文的图片
    #   会被拆成好几个参数，测试程序收到的是一个残缺路径。
    # shellcheck disable=SC2086
    if "$OUT_DIR/$name.exe" "${TEST_ARGS_ARR[@]}"; then
        : # 成功，计数由测试程序自己打印
    else
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
    fi
    return 0
}

# ---------------------------------------------------------------------------
#  actions —— 静态对账，不编译（所以不走 build_and_run）
# ---------------------------------------------------------------------------
#  ★ 为什么需要一条"不编译"的检查 ★
#    有一类错误编译器**永远抓不到**：菜单里 addAction 了某个动作，
#    却没有对应的 `= new QAction(...)`。
#    QMenu::addAction(nullptr) 是合法的、静默什么都不做 —— 菜单项凭空消失，
#    编译 0 错 0 警，ui_smoke_test 也测不到（它不链接 MainWindow）。
#    实测踩过：imageMenu->addAction(m_actionRotateArbitrary) 在，
#    `m_actionRotateArbitrary = new QAction(...)` 不在，双构建全绿。
#    只能靠对账。见 tests/action_check.py 的注释。
run_action_check() {
    echo ""
    echo "================================================================"
    echo "  静态对账 主窗口菜单动作（不编译）"
    echo "================================================================"

    if ! command -v python >/dev/null 2>&1; then
        echo "错误：找不到 python，无法运行 tests/action_check.py" >&2
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        return 1
    fi

    # ★ 路径必须转成 Windows 形式 ★
    #   python.exe 是 Windows 程序，不认 MSYS 的 "/d/Code/..."，
    #   会把它当成"当前盘根目录下的 d/Code/..."，报
    #   "can't open file 'D:\d\Code\...'"。与 cl.exe 是同一个坑。
    if python "$(to_win "$ROOT/tests/action_check.py")"; then
        TOTAL_PASSED=$((TOTAL_PASSED + 1))
    else
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
    fi
    return 0
}

# ---------------------------------------------------------------------------
#  测试清单
# ---------------------------------------------------------------------------
#  每个分支列出该测试**真正需要编译**的依赖（测试文件 + 被测源码）。
#  Selection.cpp 需要 Filters.cpp（羽化的实现搬到了 engine 层），
#  而 Filters.cpp 又依赖 Lut.cpp。列少了会报"无法解析的外部符号"。
run_one() {
    case "$1" in
        brush)
            build_and_run brush_test \
                tests/brush_test.cpp \
                src/engine/Brush.cpp
            ;;
        fill)
            # 油漆桶填充。Fill.cpp 的 composeFill() 直接转调
            # composeStroke()（"填充与画笔共用同一条合成路径"，
            # 见 engine/Fill.h 顶部），所以必须一起链进来。
            # 这就是为什么本测试**不能**只链 Fill.cpp ——
            # 少列 Brush.cpp 会报"无法解析的外部符号 composeStroke"。
            build_and_run fill_test \
                tests/fill_test.cpp \
                src/engine/Fill.cpp \
                src/engine/Brush.cpp
            ;;
        selection)
            build_and_run selection_test \
                tests/selection_test.cpp \
                src/core/Selection.cpp \
                src/engine/Filters.cpp \
                src/engine/Lut.cpp
            ;;
        adjust)
            build_and_run adjust_test \
                tests/adjust_test.cpp \
                src/engine/Adjust.cpp \
                src/engine/Curves.cpp \
                src/engine/Lut.cpp \
                src/engine/Histogram.cpp \
                src/engine/Filters.cpp
            ;;
        ruler)
            # 标尺刻度与单位换算。**本工程里最轻的一个测试** —— 它只链
            # engine/RulerMetrics.cpp（无 Q_OBJECT、无 Widgets），
            # 所以既不需要 moc 也不需要 Qt5Widgets.lib。
            # 控件那一半（刻度真的画在哪、从标尺拖出参考线）在 mainwin_test
            # 里验 —— 那里本来就真建了 MainWindow，能直接拿到两个标尺控件。
            build_and_run ruler_test \
                tests/ruler_test.cpp \
                src/engine/RulerMetrics.cpp
            ;;
        ico)
            # ICO 图标编码。与 ruler 一样是"最轻的一档"：只链
            # engine/IcoWriter.cpp（无 Q_OBJECT、无 Widgets），
            # 所以既不需要 moc 也不需要 Qt5Widgets.lib。
            #
            # ★ 它只链 Core + Gui，却会真的读写文件、还要用 Qt 自带的
            #   ico 读取器做往返比对 —— 这同时是一条"编码器不依赖界面"
            #   的证据：哪天有人在 IcoWriter 里 include 了 QWidget，
            #   这里会立刻编译失败。
            #
            # ★ 界面那一半（另存为能不能选到 ICO、超过 512 有没有拦住用户）
            #   在 mainwin_test 里验 —— 那里本来就真建了 MainWindow。
            build_and_run ico_test \
                tests/ico_test.cpp \
                src/engine/IcoWriter.cpp
            ;;
        ui_smoke)
            build_and_run ui_smoke_test \
                tests/ui_smoke_test.cpp \
                src/ui/AdjustmentDialog.cpp \
                src/ui/BrightnessContrastDialog.cpp \
                src/ui/HueSaturationDialog.cpp \
                src/ui/CurveEditor.cpp \
                src/ui/CurvesDialog.cpp \
                src/ui/LevelsWidget.cpp \
                src/ui/LevelsDialog.cpp \
                src/ui/SizeChangeDialogBase.cpp \
                src/ui/ImageSizeDialog.cpp \
                src/ui/CanvasSizeDialog.cpp \
                src/ui/RotateImageDialog.cpp \
                src/engine/Adjust.cpp \
                src/engine/Curves.cpp \
                src/engine/Lut.cpp \
                src/engine/Histogram.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Resample.cpp \
                src/core/ImageTypes.cpp
            ;;
        geometry)
            # 几何变换与重采样测试。依赖与 view 相同的 core+engine 集合 ——
            # Document.cpp 一个 TU 里就把合成、变换、重采样全引用到了，
            # 少列一个就会报"无法解析的外部符号"。
            build_and_run geometry_test \
                tests/geometry_test.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;

        view)
            # 画布视图测试。CanvasView 的依赖比前面几个测试重得多：
            # 它要真的建一个 Document、合成图层、算视图矩阵。
            # 因此这里把 core + engine 全带上，并用 moc 处理 qobject 头。
            #
            # ★ 它是唯一会用到**真实图像文件**的测试 ★
            #   不给参数时用合成图（CI 友好）；给了参数就加载真实文件。
            #   用法： ./run_tests.sh view -- <图片路径>
            build_and_run view_test \
                tests/view_test.cpp \
                src/ui/CanvasView.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ClipboardData.cpp \
                src/core/ImageTypes.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/MarchingAnts.cpp \
                src/tools/ToolEvent.cpp \
                src/tools/ToolOptions.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        toolpaint)
            # 吸管 / 油漆桶。它是**第一个同时需要 tools/ + commands/ 的测试**：
            #   · 油漆桶要构造真正的 DrawCommand 并压栈才能验"撤销粒度"，
            #     所以 commands/ 那两个 .cpp 必须带上；
            #   · 判据、覆盖度蒙版、合成都走 engine/Fill + engine/Brush；
            #   · Document.cpp 一个 TU 里引用了合成/变换/重采样，
            #     core + engine 那一串一个都不能少（少列报"无法解析的外部符号"）。
            #
            # ★ 它盯的是 fill_test 管不着的那一半 ★
            #   fill_test 把算法钉死了（连通性 / 容差 / 透明黑），
            #   本组管"算法之外"：判据取自哪张图、撤销粒度、不透明度语义。
            #   详见 tests/tool_paint_test.cpp 的文件头。
            #
            # ★ 必须链接 Qt5Widgets ★
            #   ToolOptions.cpp 要造控件（即使这里不调 createWidget，
            #   符号也会被引进来）。
            build_and_run tool_paint_test \
                tests/tool_paint_test.cpp \
                src/tools/BucketTool.cpp \
                src/tools/ColorPickerTool.cpp \
                src/tools/ToolOptions.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/ToolEvent.cpp \
                src/commands/CommandStack.cpp \
                src/commands/DrawCommand.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Fill.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        shape)
            # 直线 / 矩形 / 椭圆。依赖与 toolpaint 几乎相同，多了一个新引擎：
            #   · engine/Shape.cpp —— 光栅化成覆盖度蒙版（本组 A 类断言的靶子）；
            #   · engine/Brush.cpp —— composeStroke，形状的合成就走它
            #     （"选区限制 / 预乘合成 / 颜色 alpha"全工程只有这一份实现）；
            #   · commands/DrawCommand.cpp + CommandStack.cpp —— 撤销粒度与
            #     逐条还原那两组断言要真的压栈；缺少它们会报未解析符号；
            #   · Document.cpp 一个 TU 里引用了合成/变换/重采样，
            #     所以 core + engine 那一串一个都不能少。
            #
            # ★ 它必须链接 Qt5Widgets ★
            #   ToolOptions.cpp 要造控件（即使这里不调 createWidget，
            #   符号也会被引进来）。
            build_and_run shape_tool_test \
                tests/shape_tool_test.cpp \
                src/tools/ShapeTool.cpp \
                src/tools/ToolOptions.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/ToolEvent.cpp \
                src/commands/CommandStack.cpp \
                src/commands/DrawCommand.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Shape.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        text)
            # 文字排版引擎。本组**只链接 Qt5Core + Qt5Gui**，刻意不链
            # Qt5Widgets、也不链 tools/ —— 这是"engine 层不依赖界面"的守门人：
            #   · engine/Text.cpp 只用 QPainter + QFontMetricsF 排版，
            #     一个 QWidget 都不碰；
            #   · 用户参数对象（TextOptions）在 tools/ToolOptions.cpp 里、
            #     拖着整个 QtWidgets，所以"参数 → 字体 → 排版"那条链
            #     归 texttool 组，不在这里。
            #   若哪天有人往 engine/Text.* 里塞进界面依赖，这里会立刻编不过。
            #
            # ★ 它是唯一需要 QGuiApplication 的 engine 测试 ★
            #   排版要字体，而字体加载要 QFontDatabase —— 由测试程序自己建
            #   QGuiApplication（不是 QApplication，后者会拖进 QtWidgets）。
            build_and_run text_test \
                tests/text_test.cpp \
                src/engine/Text.cpp
            ;;
        texttool)
            # 文字工具。它是**最后一个工具**，也是依赖最"全"的一个：
            #   · tools/TextTool.cpp        —— 被测对象（浮层协议 + 落像素 + 撤销）；
            #   · tools/ToolOptions.cpp     —— TextOptions（颜色/对齐/行距/字距），
            #     它拖着整个 QtWidgets，所以本组必须链 Qt5Widgets；
            #   · engine/Text.cpp           —— 排版 + 光栅化（"参数 → 引擎"那条链）；
            #   · engine/Shape.cpp          —— embedMask（把局部蒙版嵌进区域坐标）；
            #   · engine/Brush.cpp          —— composeStroke，文字与画笔/油漆桶/形状
            #     共用同一条合成路径（选区限制 / 预乘 / 颜色 alpha 只有一份实现）；
            #   · commands/DrawCommand.cpp + CommandStack.cpp —— 撤销粒度那组要真压栈；
            #   · Document.cpp 一个 TU 里引用了合成/变换/重采样，
            #     所以 core + engine 那一串一个都不能少（少列报"无法解析的外部符号"）。
            #
            # ★ 它**刻意不链 ui/** ★
            #   浮层（QTextEdit）由 CanvasView 创建，属于装配 —— 归 mainwin_test 与
            #   view_test。本组只钉"工具发出的实参对不对、落像素对不对"。
            #   混在一起测的话，一处出错会同时弄红两组断言，反而更难定位。
            build_and_run text_tool_test \
                tests/text_tool_test.cpp \
                src/tools/TextTool.cpp \
                src/tools/ToolOptions.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/ToolEvent.cpp \
                src/commands/CommandStack.cpp \
                src/commands/DrawCommand.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Text.cpp \
                src/engine/Shape.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        handzoom)
            # 抓手 / 缩放。这两个工具**不碰文档、不压命令**，所以本组不链
            # engine/Shape.cpp 之类的绘制引擎，只带：
            #   · tools/HandTool.cpp + ZoomTool.cpp —— 被测对象；
            #   · tools/ToolOptions.cpp —— ZoomOptions（缩放方向 + Alt 提示）；
            #   · tools/AbstractTool.cpp + ToolEvent.cpp —— 基类与事件；
            #   · commands/CommandStack.cpp —— "不碰文档"那组要断言
            #     undoStack()->count() 不增长，必须有个真的栈；
            #   · Document.cpp 一个 TU 里引用了合成/变换/重采样，
            #     所以 core + engine 那一串一个都不能少（少列报未解析符号）。
            #
            # ★ 它**刻意不链 ui/** ★
            #   "信号有没有接到 panBy / setZoomAt / zoomToRect 上"属于装配，
            #   归 mainwin_test 与 view_test；本组只钉"工具发出的实参对不对"。
            #   混在一起测的话，一处出错会同时弄红两组断言，反而更难定位。
            #
            # ★ 必须链接 Qt5Widgets ★
            #   ToolOptions.cpp 要造控件（即使这里不调 createWidget，
            #   符号也会被引进来）。
            build_and_run hand_zoom_test \
                tests/hand_zoom_test.cpp \
                src/tools/HandTool.cpp \
                src/tools/ZoomTool.cpp \
                src/tools/ToolOptions.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/ToolEvent.cpp \
                src/commands/CommandStack.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        crop)
            # 裁剪工具测试。它是**第一个同时需要 tools/ 与 core/ 的纯逻辑测试**：
            #   · crops 的几何 helper（dragNewRect / resizeRect / moveRect / hitTest）
            #     是 static 纯函数，但 crop_test 还要走完整的
            #     press→move→release 事件流与 paintOverlay，所以得真的构造
            #     CropTool —— 于是 AbstractTool.cpp（基类的非内联方法）、
            #     ToolOptions.cpp（CropOptions 的读写）都要带上。
            #   · Document.cpp 一个 TU 里引用了合成/变换/重采样，
            #     所以 core + engine 那一串一个都不能少（少列报"无法解析的外部符号"）。
            #
            # ★ 它必须链接 Qt5Widgets ★
            #   ToolOptions.cpp 造控件、CropTool.cpp 画叠加层，
            #   offscreen 平台下再跑 QPainter —— 前者是编译期依赖，
            #   后者是运行期依赖（见 extra_libs_for）。
            build_and_run crop_test \
                tests/crop_test.cpp \
                src/tools/CropTool.cpp \
                src/tools/ToolOptions.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/ToolEvent.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        layer_panel)
            # 图层面板测试。它是**第一个既建控件又建文档**的测试：
            #   · 要真的构造 LayerPanel（自绘行、热点、拖拽、节流都在里面），
            #     所以要 Qt5Widgets；
            #   · 行上的眼睛/锁是走 IconLibrary 画的，所以还要 Qt5Svg + qrc；
            #   · 要真的建 Document 来验「面板 ⇄ 文档同步」，
            #     而 Document.cpp 一个 TU 里就引用了合成/变换/重采样，
            #     所以 core + engine 那一串一个都不能少（少列报"无法解析的外部符号"）。
            #
            # ★ 它刻意**不链接 commands/** ★
            #   面板只发信号、绝不自己压命令（那些命令由 MainWindow 包）。
            #   如果哪天有人在 LayerPanel.cpp 里 include 了 commands/，
            #   这里会立刻编译失败 —— 这正是想守的那个边界。
            build_and_run layer_panel_test \
                tests/layer_panel_test.cpp \
                src/ui/LayerPanel.cpp \
                src/ui/IconLibrary.cpp \
                src/core/Document.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        icon)
            # SVG 图标自检。依赖出奇地轻 —— 只有图标库自己。
            #   这同时是一条"图标渲染不依赖应用其余部分"的证据：
            #   若哪天有人让 IconLibrary 依赖 MainWindow / CanvasView，
            #   这里会立刻编不过。
            build_and_run icon_test \
                tests/icon_test.cpp \
                src/ui/IconLibrary.cpp
            ;;
        tabs)
            # 多标签容器。它是**第一个"既建控件又建文档又建撤销栈"**的测试：
            #   · DocumentTabs.cpp   —— 被测对象（账本、标题、摘页）；
            #   · CanvasView.cpp     —— 每页一块画布，要验"视图状态按页独立"，
            #     所以必须真的建出画布来；
            #   · CommandStack.cpp   —— 要验"撤销历史按页独立"，必须真压栈；
            #   · DocumentManager.cpp—— 要验"标签序号 → 文档序号"的翻译
            #     （indexOf），那是关页逻辑的地基；
            #   · Document.cpp 一个 TU 里引用了合成/变换/重采样，
            #     所以 core + engine 那一串一个都不能少（少列报未解析符号）。
            #
            # ★ 它**刻意不链 Qt5Svg、也不编 qrc** ★
            #   容器不画任何图标。这同时是一条"容器不依赖图标体系"的证据：
            #   哪天有人在 DocumentTabs.cpp 里引入 IconLibrary，这里立刻编不过。
            #
            # ★ 它**刻意不链 ui/MainWindow*** ★
            #   容器是被 MainWindow 使用的，不是反过来。混在一起测的话，
            #   一处出错会同时弄红两组断言，反而更难定位。
            build_and_run document_tabs_test \
                tests/document_tabs_test.cpp \
                src/ui/DocumentTabs.cpp \
                src/ui/CanvasView.cpp \
                src/commands/CommandStack.cpp \
                src/core/Document.cpp \
                src/core/DocumentManager.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Selection.cpp \
                src/core/ClipboardData.cpp \
                src/core/ImageTypes.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/MarchingAnts.cpp \
                src/tools/ToolEvent.cpp \
                src/tools/ToolOptions.cpp \
                src/engine/Adjust.cpp \
                src/engine/Blend.cpp \
                src/engine/Brush.cpp \
                src/engine/Curves.cpp \
                src/engine/Filters.cpp \
                src/engine/Geometry.cpp \
                src/engine/Histogram.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/Lut.cpp \
                src/engine/Resample.cpp \
                src/engine/Transform.cpp
            ;;
        mainwin)
            # 主窗口装配测试。它构造**真正的 MainWindow**，因此需要
            # 整个 src/（除 main.cpp 外的全部 .cpp）—— 这也是它成为
            # 全量清单里最慢一个的原因（放在最后跑）。
            #
            # ★ 为什么值得花这份编译时间 ★
            #   它盯着的是"整体失效却不报错"的那类 bug：工具箱一个按钮都没有、
            #   画布上的鼠标操作全部无人接收。编译器抓不到，其余每组自检
            #   也都不建 MainWindow，所以只有这里能钉住。
            #   完整清单见 .pro 的 SOURCES（手工同步，见文件末尾的说明）。
            build_and_run mainwin_test \
                tests/mainwin_test.cpp \
                src/core/Layer.cpp \
                src/core/LayerList.cpp \
                src/core/Document.cpp \
                src/core/DocumentManager.cpp \
                src/core/Selection.cpp \
                src/core/ClipboardData.cpp \
                src/core/ImageTypes.cpp \
                src/engine/Blend.cpp \
                src/engine/Lut.cpp \
                src/engine/Curves.cpp \
                src/engine/Resample.cpp \
                src/engine/Geometry.cpp \
                src/engine/Transform.cpp \
                src/engine/Adjust.cpp \
                src/engine/Filters.cpp \
                src/engine/Histogram.cpp \
                src/engine/Brush.cpp \
                src/engine/Fill.cpp \
                src/engine/Shape.cpp \
                src/engine/Text.cpp \
                src/engine/ImageMeta.cpp \
                src/engine/RulerMetrics.cpp \
                src/engine/IcoWriter.cpp \
                src/commands/CommandStack.cpp \
                src/commands/DrawCommand.cpp \
                src/commands/LayerCommand.cpp \
                src/commands/SelectionCommand.cpp \
                src/commands/ImageCommand.cpp \
                src/commands/GeometryChangeCommand.cpp \
                src/tools/AbstractTool.cpp \
                src/tools/ToolEvent.cpp \
                src/tools/ToolOptions.cpp \
                src/tools/MarchingAnts.cpp \
                src/tools/MoveTool.cpp \
                src/tools/BrushTool.cpp \
                src/tools/SelectionTool.cpp \
                src/tools/CropTool.cpp \
                src/tools/BucketTool.cpp \
                src/tools/ColorPickerTool.cpp \
                src/tools/ShapeTool.cpp \
                src/tools/HandTool.cpp \
                src/tools/ZoomTool.cpp \
                src/tools/TextTool.cpp \
                src/tools/ToolManager.cpp \
                src/ui/MainWindow.cpp \
                src/ui/MainWindowActions.cpp \
                src/ui/MainWindowCommands.cpp \
                src/ui/CanvasView.cpp \
                src/ui/DocumentTabs.cpp \
                src/ui/RulerWidget.cpp \
                src/ui/AdjustmentDialog.cpp \
                src/ui/BrightnessContrastDialog.cpp \
                src/ui/HueSaturationDialog.cpp \
                src/ui/CurveEditor.cpp \
                src/ui/CurvesDialog.cpp \
                src/ui/LevelsWidget.cpp \
                src/ui/LevelsDialog.cpp \
                src/ui/SizeChangeDialogBase.cpp \
                src/ui/ImageSizeDialog.cpp \
                src/ui/CanvasSizeDialog.cpp \
                src/ui/RotateImageDialog.cpp \
                src/ui/IconLibrary.cpp \
                src/ui/LayerPanel.cpp \
                src/ui/ToolOptionsBar.cpp \
                src/ui/StatusBarWidget.cpp
            ;;
        actions)
            run_action_check
            ;;
        *)
            echo "未知测试：$1" >&2
            return 1
            ;;
    esac
}

if [ $# -gt 0 ]; then
    # 支持 "测试名 -- 额外参数…" 的形式把参数透传给测试程序
    TEST_NAME="$1"
    shift
    if [ "${1:-}" = "--" ]; then
        shift
    fi
    TEST_ARGS_ARR=("$@")
    run_one "$TEST_NAME"
else
    TEST_ARGS_ARR=()
    run_action_check
    run_one brush
    run_one fill
    run_one selection
    run_one adjust
    run_one ui_smoke
    run_one geometry
    run_one ruler
    run_one ico
    run_one view
    run_one crop
    run_one toolpaint
    run_one shape
    run_one text
    run_one texttool
    run_one handzoom
    run_one icon
    run_one layer_panel
    run_one tabs
    run_one mainwin
fi

echo ""
echo "================================================================"
if [ "$TOTAL_FAILED" -eq 0 ]; then
    echo "  全部测试程序执行完毕，无编译或运行失败"
else
    echo "  有 $TOTAL_FAILED 个测试程序未通过"
fi
echo "================================================================"

exit "$TOTAL_FAILED"
