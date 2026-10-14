# ============================================================================
#  ZlPhotoEdit.pro —— qmake 工程文件
# ----------------------------------------------------------------------------
#  本工程同时提供 qmake（本文件）与 CMake（CMakeLists.txt）两套构建配置，
#  且两者都同时兼容 Qt 5.15 与 Qt 6.x.修改源文件列表时，请同步修改
#  CMakeLists.txt —— 不过 CMake 那边用 GLOB_RECURSE 自动收集，通常不需要动.
#
#  构建方式（VS 2022 x64 Native Tools 命令提示符）：
#    call "...\VC\Auxiliary\Build\vcvars64.bat"
#    set PATH=C:\Qt\5.15.16-VS2022-rel\bin;%PATH%
#    qmake ZlPhotoEdit.pro && nmake release
# ============================================================================

QT += core gui widgets
QT += svg concurrent printsupport

TEMPLATE = app
TARGET   = ZlPhotoEdit

# C++17：Qt6 强制要求该标准，Qt5 也完全支持，因此统一用 C++17 两版都安全
CONFIG += c++17

# 打开弃用 API 警告.这是 Qt6 迁移最有价值的开关 ——
# 它会在 Qt5 下就警告那些"将来会被删除"的用法，让我们提前发现问题，
# 而不是等真的换到 Qt6 时再面对一堆编译错误.
DEFINES += QT_DEPRECATED_WARNINGS

# ---------------------------------------------------------------------------
#  MSVC 中文编码
# ---------------------------------------------------------------------------
#  源文件统一保存为 UTF-8 with BOM，并且必须在这里显式告诉编译器.
#  否则 MSVC 会按本地代码页（GBK）解释源码中的中文字符串常量和中文注释，
#  导致运行时出乱码、甚至编译报 C4819 警告后中断.
#
#  这是中文 Qt 项目在 MSVC 下的第一个坑，务必保留.
# ---------------------------------------------------------------------------
msvc {
    QMAKE_CFLAGS   += /utf-8
    QMAKE_CXXFLAGS += /utf-8
}

INCLUDEPATH += $$PWD/src

# ---------------------------------------------------------------------------
#  源文件列表
# ---------------------------------------------------------------------------
#  这里逐个列举而不用通配符，原因：qmake 的通配符不会感知新增文件，
#  也就是说新建了 .cpp 之后必须重新跑一次 qmake，否则编译器根本不知道
#  它的存在，你会对着"未定义符号"的链接错误发呆.逐个列举虽然啰嗦，
#  但依赖关系完全可追踪，改了什么一目了然.
# ---------------------------------------------------------------------------

SOURCES += \
    src/main.cpp \
    src/compat/GlobalHotkey.cpp \
    src/compat/ScreenCapture.cpp \
    src/core/Layer.cpp \
    src/core/LayerList.cpp \
    src/core/Document.cpp \
    src/core/DocumentManager.cpp \
    src/core/Selection.cpp \
    src/core/ClipboardData.cpp \
    src/core/ImageTypes.cpp \
    src/core/UiLanguage.cpp \
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
    src/engine/GridMetrics.cpp \
    src/engine/Outline.cpp \
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
    src/tools/MagicWandTool.cpp \
    src/tools/ShapeTool.cpp \
    src/tools/TextTool.cpp \
    src/tools/HandTool.cpp \
    src/tools/ZoomTool.cpp \
    src/tools/ToolManager.cpp \
    src/ui/MainWindow.cpp \
    src/ui/MainWindowActions.cpp \
    src/ui/MainWindowCommands.cpp \
    src/ui/CanvasView.cpp \
    src/ui/DocumentTabs.cpp \
    src/ui/RulerWidget.cpp \
    src/ui/LayerPanel.cpp \
    src/ui/IconLibrary.cpp \
    src/ui/AppInfo.cpp \
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
    src/ui/GridSettingsDialog.cpp \
    src/ui/RotateImageDialog.cpp \
    src/ui/ToolOptionsBar.cpp \
    src/ui/StatusBarWidget.cpp

HEADERS += \
    src/compat/QtCompat.h \
    src/compat/GlobalHotkey.h \
    src/compat/ScreenCapture.h \
    src/core/Layer.h \
    src/core/LayerList.h \
    src/core/Document.h \
    src/core/DocumentManager.h \
    src/core/Selection.h \
    src/core/ClipboardData.h \
    src/core/ImageTypes.h \
    src/core/UiLanguage.h \
    src/engine/Blend.h \
    src/engine/Lut.h \
    src/engine/Curves.h \
    src/engine/Resample.h \
    src/engine/Geometry.h \
    src/engine/Transform.h \
    src/engine/Adjust.h \
    src/engine/Filters.h \
    src/engine/Histogram.h \
    src/engine/Brush.h \
    src/engine/Fill.h \
    src/engine/Shape.h \
    src/engine/Text.h \
    src/engine/ImageMeta.h \
    src/engine/RulerMetrics.h \
    src/engine/GridMetrics.h \
    src/engine/Outline.h \
    src/engine/IcoWriter.h \
    src/commands/CommandStack.h \
    src/commands/DrawCommand.h \
    src/commands/LayerCommand.h \
    src/commands/SelectionCommand.h \
    src/commands/ImageCommand.h \
    src/commands/GeometryChangeCommand.h \
    src/tools/AbstractTool.h \
    src/tools/ToolEvent.h \
    src/tools/ToolParams.h \
    src/tools/ToolOptions.h \
    src/tools/MarchingAnts.h \
    src/tools/MoveTool.h \
    src/tools/BrushTool.h \
    src/tools/SelectionTool.h \
    src/tools/CropTool.h \
    src/tools/BucketTool.h \
    src/tools/ColorPickerTool.h \
    src/tools/MagicWandTool.h \
    src/tools/ShapeTool.h \
    src/tools/TextTool.h \
    src/tools/HandTool.h \
    src/tools/ZoomTool.h \
    src/tools/ToolManager.h \
    src/ui/MainWindow.h \
    src/ui/CanvasView.h \
    src/ui/DocumentTabs.h \
    src/ui/RulerWidget.h \
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
    src/ui/GridSettingsDialog.h \
    src/ui/RotateImageDialog.h \
    src/ui/ToolOptionsBar.h \
    src/ui/StatusBarWidget.h \
    src/ui/LayerPanel.h \
    src/ui/IconLibrary.h \
    src/ui/AppInfo.h

# ---------------------------------------------------------------------------
#  资源文件（SVG 图标）
# ---------------------------------------------------------------------------
#  图标本体在 resources/icons/*.svg，由 resources/gen_icons.py 生成；
#  resources.qrc 同样由它生成（加/删图标后重跑一次即可，不要手改 qrc）.
#  ★ CMake 那边不需要动 ★：CMakeLists.txt 早就写着
#    `if(EXISTS resources/resources.qrc)` 并在 AUTORCC 打开时会自动处理.
# ---------------------------------------------------------------------------

RESOURCES += \
    resources/resources.qrc

# ---------------------------------------------------------------------------
#  多语言（i18n）
# ---------------------------------------------------------------------------
#  ★ 硬约束（用户明确要求）：翻译文件内嵌进可执行文件 ★
#    不发布独立的翻译文件夹 —— 运行时从 ":/i18n/..." 读.
#
#  链路（lrelease 与 qrc 生成都在 qmake 解析期完成，rcc 留给 nmake）：
#    translations/zlphotoedit_zh_CN.ts
#      → lrelease  → build/i18n/zlphotoedit_zh_CN.qm
#      → write_file 生成 build/i18n/i18n.qrc（列出 .qm 清单）
#      → RESOURCES 收进来 → rcc 编进二进制 → 运行时 :/i18n/xxx.qm
#
#  ★ 改完 .ts 必须重跑一次 qmake ★
#    Makefile 里没有 .ts → .qm 的依赖（在 qmake 体系里加它要动
#    QMAKE_EXTRA_TARGETS，为一年改两次的翻译文件不值当）：
#    编辑翻译内容后，重新 qmake + nmake.
#
#  ★ 新增一种语言要同步三处 ★
#    下面的 I18N_LANG_CODES、translations/ 下的 .ts 文件、
#    src/core/UiLanguage.cpp 的 kEmbeddedLanguages.
#    漏一个不报编译错，只表现为"菜单里少一种语言"，靠人对账.
#
#  ★ CMake 侧的同一套逻辑见 CMakeLists.txt「多语言」段 ★
#    两边同一批 .ts、同一个 prefix，否则换构建路径等于换语言集.
#
#  ★ 本文件注释一律用 ASCII "."，不许写全角句号「。」★
#    lupdate 自带的 qmake 解析器遇到注释行里的 U+3002 会吞掉紧随的换行，
#    把下一行并进注释（两行即可复现）—— 排在「。」注释后面的代码行会
#    整体消失：TRANSLATIONS 曾经就这样被吞掉，报 "no TS files specified"，
#    而 qmake 本体解析完全正常（构建一直是好的），只有 lupdate 这一个
#    消费者受害，所以这个坑在阶段 1 潜伏了一整轮（当时那几条由来不明的
#    WARNING 即源于此）. 2026-10-13 全量替换 + 实测确认后立此规矩.
# ---------------------------------------------------------------------------

# 语言代码清单（.ts / .qm 文件名去掉前缀与后缀就是它）
I18N_LANG_CODES = zh_CN

# lupdate/lrelease 的正规入口（Qt 约定变量）.加上它之后：
#   · lupdate ZlPhotoEdit.pro     扫 SOURCES/HEADERS 刷新 .ts（源码英文化后必跑）
#   · lrelease ZlPhotoEdit.pro    重新产出 .qm（qmake 解析期也已自动做过一遍）
# 它只生成 Makefile 靶子，不参与编译；内嵌仍靠下面的 i18n.qrc.
TRANSLATIONS = \
    translations/zlphotoedit_zh_CN.ts

I18N_DIR = $$PWD/build/i18n

# .qm 文件名清单（喂给下面的 qrc 内容）
I18N_QM_NAMES =
for(lang, I18N_LANG_CODES) {
    I18N_QM_NAMES += zlphotoedit_$${lang}.qm
}

# 生成 qrc.write_file 只接受**变量名**（传字面量会写出空文件），
# 且会自动创建父目录 —— 正好替 lrelease 把 build/i18n 建好：
# lrelease 自己不会建目录，缺目录时它直接报错退出（实测确认）.
# ★ 下面两行必须用单引号 ★
#   qmake 不识别双引号串里的 \" 转义：会把 \" 当成收尾引号，继而报告
#   "Missing closing quote" + "Unexpected else"（2026-10-13 实测），
#   并**静默丢掉后面的 TRANSLATIONS 注册** —— 症状是 lupdate 找不到
#   .ts 文件.单引号在 qmake 里是纯字面量（不做变量展开），这两行
#   恰好没有 $$变量，用单引号包住双引号即可.
I18N_QRC = $$I18N_DIR/i18n.qrc
I18N_QRC_CONTENT = '<!DOCTYPE RCC><RCC version="1.0">'
I18N_QRC_CONTENT += '<qresource prefix="/i18n">'
for(name, I18N_QM_NAMES): I18N_QRC_CONTENT += "<file>$$name</file>"
I18N_QRC_CONTENT += "</qresource></RCC>"
write_file($$I18N_QRC, I18N_QRC_CONTENT)

# lrelease 的位置：优先 Qt 的 bin（与 qmake 同目录），找不到回落 PATH
# （build_env.sh 已把 Qt bin 放进 PATH）.
# ★ exists() 必须带 .exe 后缀 ★
#   Windows 上 QFileInfo::exists 不做 PATHEXT 解析，写 "lrelease"
#   会被判成不存在（实测踩过：于是白白走了一次 PATH 回落）.
win32 {
    I18N_LRELEASE = $$[QT_INSTALL_BINS]/lrelease.exe
} else {
    I18N_LRELEASE = $$[QT_INSTALL_BINS]/lrelease
}
!exists($$I18N_LRELEASE) {
    win32: I18N_LRELEASE = lrelease.exe
    else:  I18N_LRELEASE = lrelease
}

# 逐个 .ts → .qm.任何一步没产出 .qm 就整体跳过内嵌（与 CMake 侧
# "lrelease 找不到只警告"的降级一致）：翻译是增强项，不该让一个
# 可选的 Linguist 组件把构建挡死；界面会回落到源码文本.
I18N_OK =
for(lang, I18N_LANG_CODES) {
    ts = $$PWD/translations/zlphotoedit_$${lang}.ts
    qm = $$I18N_DIR/zlphotoedit_$${lang}.qm
    system($$I18N_LRELEASE $$ts -qm $$qm)
    exists($$qm): I18N_OK = true
}
isEmpty(I18N_OK) {
    message("警告：lrelease 未产出 .qm，跳过翻译内嵌（界面将回落到源码文本）")
} else {
    RESOURCES += $$I18N_QRC
}

# ---------------------------------------------------------------------------
#  Windows 可执行文件图标（exe 本身的图标）
# ---------------------------------------------------------------------------
#  ★ 与上面的 RESOURCES 是两回事，不要合并 ★
#    · resources.qrc → rcc.exe → 编进二进制，运行时用 ":/…" 取；
#      管的是**窗口/任务栏**图标（QApplication::setWindowIcon）.
#    · resources/app.rc → rc.exe → 编成资源段，**运行时不经过 Qt**；
#      管的是**资源管理器里那个 exe 文件的图标**.
#
#  ★ 为什么用 RC_FILE 而不是 RC_ICONS ★
#    RC_ICONS = xx.ico 会让 qmake 自己合成一个 .rc —— 简单，但那个 .rc
#    的内容不受我们控制，也不能写注释解释"为什么 ID 必须是 1".
#    RC_FILE 指向我们自己的 .rc，两条构建路径（qmake / CMake）指向同一个
#    文件，行为一致，改起来只有一处.
#
#  ★ 只在 win32 下加 ★
#    rc.exe 是 MSVC 的工具链；非 Windows 平台加了会直接构建失败.
win32 {
    RC_FILE = resources/app.rc

    # AppInfo::hasExeIcon() 的编译期开关.CMake 侧在同一个位置定义
    # （见 CMakeLists.txt 的 target_compile_definitions），两边必须一致 ——
    # 否则自检在一条构建路径上断言"有 exe 图标"、另一条上断言"没有".
    DEFINES += ZLPE_HAS_EXE_ICON=1
}


# ---------------------------------------------------------------------------
#  输出目录整理 —— 避免构建中间产物散落在工程根目录
# ---------------------------------------------------------------------------
CONFIG(debug, debug|release) {
    DESTDIR = $$PWD/build/debug
} else {
    DESTDIR = $$PWD/build/release
}
OBJECTS_DIR = $$PWD/build/obj
MOC_DIR     = $$PWD/build/moc
RCC_DIR     = $$PWD/build/rcc
UI_DIR      = $$PWD/build/ui
