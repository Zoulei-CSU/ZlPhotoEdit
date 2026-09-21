# ============================================================================
#  ZlPhotoEdit.pro —— qmake 工程文件
# ----------------------------------------------------------------------------
#  本工程同时提供 qmake（本文件）与 CMake（CMakeLists.txt）两套构建配置，
#  且两者都同时兼容 Qt 5.15 与 Qt 6.x。修改源文件列表时，请同步修改
#  CMakeLists.txt —— 不过 CMake 那边用 GLOB_RECURSE 自动收集，通常不需要动。
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

# 打开弃用 API 警告。这是 Qt6 迁移最有价值的开关 ——
# 它会在 Qt5 下就警告那些"将来会被删除"的用法，让我们提前发现问题，
# 而不是等真的换到 Qt6 时再面对一堆编译错误。
DEFINES += QT_DEPRECATED_WARNINGS

# ---------------------------------------------------------------------------
#  MSVC 中文编码
# ---------------------------------------------------------------------------
#  源文件统一保存为 UTF-8 with BOM，并且必须在这里显式告诉编译器。
#  否则 MSVC 会按本地代码页（GBK）解释源码中的中文字符串常量和中文注释，
#  导致运行时出乱码、甚至编译报 C4819 警告后中断。
#
#  这是中文 Qt 项目在 MSVC 下的第一个坑，务必保留。
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
#  它的存在，你会对着"未定义符号"的链接错误发呆。逐个列举虽然啰嗦，
#  但依赖关系完全可追踪，改了什么一目了然。
# ---------------------------------------------------------------------------

SOURCES += \
    src/main.cpp \
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
    src/ui/ToolOptionsBar.cpp \
    src/ui/StatusBarWidget.cpp

HEADERS += \
    src/compat/QtCompat.h \
    src/core/Layer.h \
    src/core/LayerList.h \
    src/core/Document.h \
    src/core/DocumentManager.h \
    src/core/Selection.h \
    src/core/ClipboardData.h \
    src/core/ImageTypes.h \
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
    src/ui/RotateImageDialog.h \
    src/ui/ToolOptionsBar.h \
    src/ui/StatusBarWidget.h \
    src/ui/LayerPanel.h \
    src/ui/IconLibrary.h

# ---------------------------------------------------------------------------
#  资源文件（SVG 图标）
# ---------------------------------------------------------------------------
#  图标本体在 resources/icons/*.svg，由 resources/gen_icons.py 生成；
#  resources.qrc 同样由它生成（加/删图标后重跑一次即可，不要手改 qrc）。
#  ★ CMake 那边不需要动 ★：CMakeLists.txt 早就写着
#    `if(EXISTS resources/resources.qrc)` 并在 AUTORCC 打开时会自动处理。
# ---------------------------------------------------------------------------

RESOURCES += \
    resources/resources.qrc


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
