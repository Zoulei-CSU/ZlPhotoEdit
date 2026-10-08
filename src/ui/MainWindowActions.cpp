// ============================================================================
//  MainWindowActions.cpp —— MainWindow 的菜单 / 工具栏 / 停靠面板搭建
// ----------------------------------------------------------------------------
//  ★ 为什么把 MainWindow 拆成多个 .cpp ★
//
//    完整实现有 2000 行以上，放在一个文件里会很难导航 ——
//    想找"另存为的实现"需要在几千行里翻找，而且改动容易互相冲突。
//
//    按"逻辑职责"拆成三个文件，每个都只包含 MainWindow 的一部分成员函数：
//      MainWindow.cpp          构造、初始化顺序、工具装配、标题
//      MainWindowActions.cpp   动作定义 + 菜单 + 工具栏 + 停靠面板 + 状态栏 + 设置
//      MainWindowCommands.cpp  各菜单命令的实际实现（文件/编辑/图像/图层）
//
//  ★ C++ 不要求一个类的成员函数都在同一个文件里 ★
//    只要每个函数只定义一次即可。这是大型 Qt 项目的常见做法
//    （Qt 自己的 QWidget 就分在 qwidget.cpp / qwidget_win.cpp / qwidget_mac.mm 里）。
// ============================================================================

#include "ui/MainWindow.h"
#include "ui/IconLibrary.h"
#include "ui/AppInfo.h"
#include "compat/QtCompat.h"
#include "compat/GlobalHotkey.h"
#include "ui/CanvasView.h"
#include "ui/ToolOptionsBar.h"
#include "ui/StatusBarWidget.h"
#include "ui/LayerPanel.h"

#include "core/DocumentManager.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "core/ClipboardData.h"

#include "commands/CommandStack.h"

#include "engine/RulerMetrics.h"

#include "tools/ToolManager.h"
#include "tools/AbstractTool.h"
#include "tools/ToolOptions.h"

#include <QAction>
#include <QActionGroup>
#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <QDockWidget>
#include <QStatusBar>
#include <QListWidget>
#include <QLabel>
#include <QUndoView>
#include <QToolButton>
#include <QPushButton>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QPainter>
#include <QPixmap>
#include <QDesktopServices>
#include <QUrl>

#include <functional>   // std::function（工具箱按钮的 lambda 包装）

namespace zl {

// ============================================================================
//  setupActions —— 定义全部 QAction
// ============================================================================
//
//  ★ 为什么全部用成员变量而不是局部变量 ★
//
//    同一个动作会出现在多个地方：
//      · "复制" 在 编辑菜单 + 画布右键菜单
//      · "撤销" 在 编辑菜单 + 工具栏 + 快捷键
//    如果每次 addAction 都新建一个 QAction，那么：
//      ① 它们的 enabled 状态需要分别维护（极易漏掉，表现为
//         "菜单里是灰的但工具栏按钮还能点"）
//      ② 快捷键会冲突（两个 QAction 抢同一个键，行为不确定：
//         Qt 会随机触发其中一个）
//      ③ 无法统一改文本（例如"撤销 画笔描边"）
//    用一个 QAction 加到多个地方则完全没有这些问题。
//
//  ★ objectName 的作用 ★
//    CanvasView 的右键菜单需要通过 findChild<QAction*>("actionCopy")
//    找到这些动作。这样 CanvasView 不需要知道 MainWindow 的类型，
//    保持了 ui 层内部的松耦合 —— 将来把右键菜单拆成独立类也不用改。
// ============================================================================

// ---------------------------------------------------------------------------
//  标尺单位的中文名（只给菜单用）
// ---------------------------------------------------------------------------
//  ★ 这里只做"枚举 → 名字"的映射，**不做排序** ★
//    "第几个单位是什么"由 rulermetrics::unitAt() 单独决定（一份协议）。
//    两处分头维护顺序，迟早出现"菜单写着厘米、选出来是毫米"——
//    而且它看起来完全正常，只是单位不对。
static QString rulerUnitName(rulermetrics::Unit unit)
{
    switch (unit) {
    case rulermetrics::Unit::Pixel:      return QStringLiteral("像素");
    case rulermetrics::Unit::Centimeter: return QStringLiteral("厘米");
    case rulermetrics::Unit::Millimeter: return QStringLiteral("毫米");
    case rulermetrics::Unit::Inch:       return QStringLiteral("英寸");
    case rulermetrics::Unit::Point:      return QStringLiteral("点");
    }
    return QString();
}

// ---------------------------------------------------------------------------
//  网格间距的预设档位（只给菜单用；**单一来源**）
// ---------------------------------------------------------------------------
//  ★ 为什么是这几个值 ★
//    8/16 用于像素级对齐（图标排版、像素画），32/64 是通用档，
//    128/256 用于大图分块。这六档覆盖九成场景；其余值走「自定义…」。
//
//  ★ 为什么做成一张表、而不是在菜单循环里现写 ★
//    菜单下标 → 毫米值的映射只允许有一份。与「标尺单位」同一条纪律：
//    两处分头维护顺序，会出现"菜单写着 64、选出来是 32"——
//    而且看起来完全正常，只是间距不对。
static const int kGridPresets[] = { 8, 16, 32, 64, 128, 256 };
static const int kGridPresetCount = int(sizeof(kGridPresets) / sizeof(kGridPresets[0]));

void MainWindow::setupActions()
{
    // ─────────────────────────────────────────────────────────────────────
    //  文件
    // ─────────────────────────────────────────────────────────────────────

    m_actionNew = new QAction(tr("新建(&N)..."), this);
    m_actionNew->setObjectName(QStringLiteral("actionNew"));
    m_actionNew->setIcon(icons::icon(QStringLiteral("file_new")));
    m_actionNew->setShortcut(QKeySequence::New);
    m_actionNew->setStatusTip(tr("创建一个新的空白图像"));
    connect(m_actionNew, &QAction::triggered, this, &MainWindow::newDocument);

    m_actionOpen = new QAction(tr("打开(&O)..."), this);
    m_actionOpen->setObjectName(QStringLiteral("actionOpen"));
    m_actionOpen->setIcon(icons::icon(QStringLiteral("file_open")));
    m_actionOpen->setShortcut(QKeySequence::Open);
    m_actionOpen->setStatusTip(tr("打开一个已存在的图像文件"));
    connect(m_actionOpen, &QAction::triggered, this, &MainWindow::openDocument);

    // ★ 与「打开」并列，但语义完全不同 ★
    //   「打开」换文档（旧文档被关闭、撤销栈清空、标题与最近文件都变）；
    //   「打开为图层」只往**当前**文档里加一层。
    //   破坏力差得远，所以必须是两个菜单项，不能合并成一个。
    //
    // ★ 刻意不给快捷键 ★
    //   它是个不常用的"重度操作"，而合适的组合键都已名花有主
    //   （Ctrl+O 是打开、Ctrl+Shift+V 是粘贴到当前层）。
    //   硬塞一个进去只会在键盘上误触，进而悄悄改掉画布尺寸。
    m_actionOpenAsLayer = new QAction(tr("打开为图层(&L)..."), this);
    m_actionOpenAsLayer->setObjectName(QStringLiteral("actionOpenAsLayer"));
    m_actionOpenAsLayer->setIcon(icons::icon(QStringLiteral("file_open_as_layer")));
    m_actionOpenAsLayer->setStatusTip(
        tr("把另一张图片作为新图层加入当前图像（不切换文档）"));
    connect(m_actionOpenAsLayer, &QAction::triggered, this, &MainWindow::openAsLayer);

    // ── 屏幕截图：四个动作（子菜单在 setupMenus 里挂到「文件」下）──
    //
    // ★ 四个都写成显式的四件套，不搞表驱动循环 ★
    //   tools/action_check.py 是**文本级**对账：它靠
    //   `m_xxx = new QAction` 与 `m_xxx->setObjectName(` 两条正则确认
    //   "创建 ⇄ 进菜单 ⇄ 有名字"三件事成对。写成
    //   `*pa = new QAction(...)` 或循环里 `a->setObjectName(...)` 的话，
    //   对账脚本一条都匹配不到 —— 而它给出的结论是"菜单项凭空消失"这类
    //   真实故障的守门人。四行重复换一条守门，值。
    //
    // ★ 为什么"立即"带快捷键、其它三个不带 ★
    //   延迟截图总得有人先点菜单，那时程序一定在前台，普通的应用内快捷键
    //   就够用。而"立即截图"的典型用法恰恰是**程序在后台/最小化**，
    //   随手截一下屏幕上的东西 —— 那种场景 Qt 层的快捷键收不到按键，
    //   必须向系统登记（见 compat/GlobalHotkey.h）。
    //
    // ★ 同时 setShortcut 是全局登记失败时的兜底 ★
    //   系统里已经有人占着这个组合键时 RegisterHotKey 会失败（QQ / 微信 /
    //   各种截图工具都会抢），那时"程序在前台"这条路仍然要能用。
    //   ★ 两个都生效时不会各截一张 ★ —— RegisterHotKey 是在系统层把这个键
    //   **截走**的，那一次按键不会到达 Qt，所以只会触发一次。
    //
    // ★ 这里先按"首选键"显示，稍后可能被改写 ★
    //   建动作时还没向系统登记，只能先显示候选表第一项；
    //   setupScreenCapture() 登记完若发现首选键被别的程序占着、换用了别的键，
    //   会调 applyCaptureHotkey() 把它改过来 —— 菜单上写的必须就是能按的。
    // ★ 这里必须用花括号初始化 ★
    //   写成 `const QKeySequence captureSeq(QLatin1String(kCaptureHotkey));`
    //   会被解析成**函数声明**（"最令人烦恼的解析"）：
    //   它声明了一个接受 QLatin1String、返回 QKeySequence 的函数，
    //   报错出现在 setShortcut() 那一行（"无法把函数指针转成 QKeySequence"），
    //   完全看不出根因在初始化写法上。
    const QKeySequence captureSeq{QLatin1String(kCaptureHotkey)};

    m_actionCaptureNow = new QAction(tr("立即截图"), this);
    m_actionCaptureNow->setObjectName(QStringLiteral("actionCaptureNow"));
    m_actionCaptureNow->setIcon(icons::icon(QStringLiteral("action_screen_capture")));
    applyCaptureHotkey(captureSeq);
    connect(m_actionCaptureNow, &QAction::triggered, this,
            [this] { captureScreenNow(); });

    // 延迟三项：同一件事的三个档。
    // ★ 菜单上写"秒"、代码里给"毫秒" —— 换算只发生在这三个 lambda 里 ★
    //   散出去就会有人把 3000 写成 3。
    m_actionCaptureDelay3 = new QAction(tr("延迟 3 秒"), this);
    m_actionCaptureDelay3->setObjectName(QStringLiteral("actionCaptureDelay3"));
    m_actionCaptureDelay3->setIcon(icons::icon(QStringLiteral("action_capture_delay")));
    m_actionCaptureDelay3->setStatusTip(
        tr("3 秒后截图：本窗口会先最小化，请切到要截取的画面"));
    connect(m_actionCaptureDelay3, &QAction::triggered, this,
            [this] { scheduleScreenCapture(3000); });

    m_actionCaptureDelay5 = new QAction(tr("延迟 5 秒"), this);
    m_actionCaptureDelay5->setObjectName(QStringLiteral("actionCaptureDelay5"));
    m_actionCaptureDelay5->setIcon(icons::icon(QStringLiteral("action_capture_delay")));
    m_actionCaptureDelay5->setStatusTip(
        tr("5 秒后截图：本窗口会先最小化，请切到要截取的画面"));
    connect(m_actionCaptureDelay5, &QAction::triggered, this,
            [this] { scheduleScreenCapture(5000); });

    m_actionCaptureDelay10 = new QAction(tr("延迟 10 秒"), this);
    m_actionCaptureDelay10->setObjectName(QStringLiteral("actionCaptureDelay10"));
    m_actionCaptureDelay10->setIcon(icons::icon(QStringLiteral("action_capture_delay")));
    m_actionCaptureDelay10->setStatusTip(
        tr("10 秒后截图：本窗口会先最小化，请切到要截取的画面"));
    connect(m_actionCaptureDelay10, &QAction::triggered, this,
            [this] { scheduleScreenCapture(10000); });

    m_actionSave = new QAction(tr("保存(&S)"), this);
    m_actionSave->setObjectName(QStringLiteral("actionSave"));
    m_actionSave->setIcon(icons::icon(QStringLiteral("file_save")));
    m_actionSave->setShortcut(QKeySequence::Save);
    m_actionSave->setStatusTip(tr("保存到当前文件（未命名时会先询问路径）"));
    connect(m_actionSave, &QAction::triggered, this, &MainWindow::saveDocument);

    m_actionSaveAs = new QAction(tr("另存为(&A)..."), this);
    m_actionSaveAs->setObjectName(QStringLiteral("actionSaveAs"));
    m_actionSaveAs->setIcon(icons::icon(QStringLiteral("file_save_as")));
    m_actionSaveAs->setShortcut(QKeySequence::SaveAs);
    m_actionSaveAs->setStatusTip(tr("保存到新文件，格式由扩展名决定"));
    connect(m_actionSaveAs, &QAction::triggered, this, &MainWindow::saveDocumentAs);

    m_actionClose = new QAction(tr("关闭(&W)"), this);
    m_actionClose->setObjectName(QStringLiteral("actionClose"));
    m_actionClose->setIcon(icons::icon(QStringLiteral("file_close")));
    m_actionClose->setShortcut(QKeySequence::Close);
    m_actionClose->setStatusTip(tr("关闭当前图像"));
    connect(m_actionClose, &QAction::triggered, this, &MainWindow::closeCurrentDocument);

    m_actionQuit = new QAction(tr("退出(&Q)"), this);
    m_actionQuit->setObjectName(QStringLiteral("actionQuit"));
    m_actionQuit->setIcon(icons::icon(QStringLiteral("file_quit")));
    m_actionQuit->setShortcut(QKeySequence::Quit);
    m_actionQuit->setMenuRole(QAction::QuitRole);
    m_actionQuit->setStatusTip(tr("退出 %1").arg(zl::appinfo::displayName()));
    connect(m_actionQuit, &QAction::triggered, this, &QWidget::close);

    // ─────────────────────────────────────────────────────────────────────
    //  编辑
    // ─────────────────────────────────────────────────────────────────────

    m_actionUndo = new QAction(tr("撤销(&U)"), this);
    m_actionUndo->setObjectName(QStringLiteral("actionUndo"));
    m_actionUndo->setIcon(icons::icon(QStringLiteral("edit_undo")));
    m_actionUndo->setShortcut(QKeySequence::Undo);
    m_actionUndo->setStatusTip(tr("撤销上一步操作"));
    m_actionUndo->setEnabled(false);
    connect(m_actionUndo, &QAction::triggered, this, &MainWindow::undo);

    m_actionRedo = new QAction(tr("重做(&R)"), this);
    m_actionRedo->setObjectName(QStringLiteral("actionRedo"));
    m_actionRedo->setIcon(icons::icon(QStringLiteral("edit_redo")));
    // ★ 重做给两个快捷键 ★
    //   QKeySequence::Redo 在 Windows 上解析为 Ctrl+Y。
    //   但很多用户习惯 Ctrl+Shift+Z（Linux/Mac 的约定），
    //   从其他平台迁移过来的用户会反复按它。
    //   Qt 支持一个 QAction 有多个快捷键，两个都给最省事。
    m_actionRedo->setShortcuts({ QKeySequence::Redo,
                                 QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z) });
    m_actionRedo->setStatusTip(tr("重做被撤销的操作"));
    m_actionRedo->setEnabled(false);
    connect(m_actionRedo, &QAction::triggered, this, &MainWindow::redo);

    m_actionCut = new QAction(tr("剪切(&T)"), this);
    m_actionCut->setObjectName(QStringLiteral("actionCut"));
    m_actionCut->setIcon(icons::icon(QStringLiteral("edit_cut")));
    m_actionCut->setShortcut(QKeySequence::Cut);
    m_actionCut->setStatusTip(tr("剪切选区内容到剪贴板"));
    m_actionCut->setEnabled(false);
    connect(m_actionCut, &QAction::triggered, this, &MainWindow::cutSelection);

    m_actionCopy = new QAction(tr("复制(&C)"), this);
    m_actionCopy->setObjectName(QStringLiteral("actionCopy"));
    m_actionCopy->setIcon(icons::icon(QStringLiteral("edit_copy")));
    m_actionCopy->setShortcut(QKeySequence::Copy);
    m_actionCopy->setStatusTip(tr("复制选区内容到剪贴板（无选区时复制整个图层）"));
    m_actionCopy->setEnabled(false);
    connect(m_actionCopy, &QAction::triggered, this, &MainWindow::copySelection);

    m_actionPaste = new QAction(tr("粘贴为新图层(&P)"), this);
    m_actionPaste->setObjectName(QStringLiteral("actionPaste"));
    m_actionPaste->setIcon(icons::icon(QStringLiteral("edit_paste")));
    m_actionPaste->setShortcut(QKeySequence::Paste);
    m_actionPaste->setStatusTip(tr("把剪贴板内容粘贴为一个新图层"));
    connect(m_actionPaste, &QAction::triggered, this, &MainWindow::pasteAsNewLayer);

    m_actionPasteInto = new QAction(tr("粘贴到当前图层(&I)"), this);
    m_actionPasteInto->setObjectName(QStringLiteral("actionPasteInto"));
    m_actionPasteInto->setIcon(icons::icon(QStringLiteral("edit_paste_into")));
    m_actionPasteInto->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_V));
    m_actionPasteInto->setStatusTip(tr("把剪贴板内容合并到当前图层"));
    connect(m_actionPasteInto, &QAction::triggered, this, &MainWindow::pasteIntoLayer);

    m_actionDelete = new QAction(tr("删除(&D)"), this);
    m_actionDelete->setObjectName(QStringLiteral("actionDelete"));
    m_actionDelete->setIcon(icons::icon(QStringLiteral("edit_delete")));
    m_actionDelete->setShortcut(QKeySequence::Delete);
    m_actionDelete->setStatusTip(tr("删除选区内的内容（无选区时清空整个图层）"));
    connect(m_actionDelete, &QAction::triggered, this, &MainWindow::onDeleteSelection);

    // ─────────────────────────────────────────────────────────────────────
    //  选择
    // ─────────────────────────────────────────────────────────────────────

    m_actionSelectAll = new QAction(tr("全选(&A)"), this);
    m_actionSelectAll->setObjectName(QStringLiteral("actionSelectAll"));
    m_actionSelectAll->setIcon(icons::icon(QStringLiteral("select_all")));
    m_actionSelectAll->setShortcut(QKeySequence::SelectAll);
    m_actionSelectAll->setStatusTip(tr("选中整个画布"));
    connect(m_actionSelectAll, &QAction::triggered, this, &MainWindow::selectAll);

    m_actionDeselect = new QAction(tr("取消选择(&D)"), this);
    m_actionDeselect->setObjectName(QStringLiteral("actionDeselect"));
    m_actionDeselect->setIcon(icons::icon(QStringLiteral("select_none")));
    // ★ 为什么不用 QKeySequence::Deselect ★
    //   Qt 没有提供这个标准键（SelectAll 有，Deselect 没有）。
    //   Ctrl+D 是 Photoshop 的约定，用户肌肉记忆很强，直接用。
    m_actionDeselect->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_D));
    m_actionDeselect->setStatusTip(tr("取消当前选区"));
    connect(m_actionDeselect, &QAction::triggered, this, &MainWindow::deselect);

    m_actionInvertSelection = new QAction(tr("反选(&I)"), this);
    m_actionInvertSelection->setObjectName(QStringLiteral("actionInvertSelection"));
    m_actionInvertSelection->setIcon(icons::icon(QStringLiteral("select_invert")));
    m_actionInvertSelection->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_I));
    m_actionInvertSelection->setStatusTip(tr("选中当前选区之外的全部区域"));
    connect(m_actionInvertSelection, &QAction::triggered, this, &MainWindow::invertSelection);

    // ─────────────────────────────────────────────────────────────────────
    //  调整（Phase 1 只接不需要参数对话框的那些）
    // ─────────────────────────────────────────────────────────────────────

    m_actionGrayscale = new QAction(tr("去色(&G)"), this);
    m_actionGrayscale->setObjectName(QStringLiteral("actionGrayscale"));
    m_actionGrayscale->setIcon(icons::icon(QStringLiteral("adjust_grayscale")));
    // Ctrl+Shift+U 是 Photoshop 的约定
    m_actionGrayscale->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_U));
    m_actionGrayscale->setStatusTip(tr("按亮度权重转换为灰度（0.299R + 0.587G + 0.114B）"));
    connect(m_actionGrayscale, &QAction::triggered, this, &MainWindow::applyGrayscale);

    m_actionInvert = new QAction(tr("反相(&I)"), this);
    m_actionInvert->setObjectName(QStringLiteral("actionInvert"));
    m_actionInvert->setIcon(icons::icon(QStringLiteral("adjust_invert")));
    m_actionInvert->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_I));
    m_actionInvert->setStatusTip(tr("反转所有颜色通道（如同底片）"));
    connect(m_actionInvert, &QAction::triggered, this, &MainWindow::applyInvert);

    // ── 需要参数对话框的调整（Phase 4）──
    //
    // 快捷键沿用 Photoshop 的约定：色相/饱和度 Ctrl+U、曲线 Ctrl+M。
    // 亮度/对比度在 PS 里没有快捷键，这里也不设 —— 与其硬塞一个
    // 容易和别的功能冲突的组合键，不如保持"无快捷键"。
    m_actionBrightnessContrast = new QAction(tr("亮度/对比度(&B)..."), this);
    m_actionBrightnessContrast->setObjectName(QStringLiteral("actionBrightnessContrast"));
    m_actionBrightnessContrast->setIcon(icons::icon(QStringLiteral("adjust_brightness")));
    m_actionBrightnessContrast->setStatusTip(tr("调整图像的明暗与反差"));
    connect(m_actionBrightnessContrast, &QAction::triggered,
            this, &MainWindow::openBrightnessContrast);

    m_actionHueSaturation = new QAction(tr("色相/饱和度(&H)..."), this);
    m_actionHueSaturation->setObjectName(QStringLiteral("actionHueSaturation"));
    m_actionHueSaturation->setIcon(icons::icon(QStringLiteral("adjust_hue")));
    m_actionHueSaturation->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_U));
    m_actionHueSaturation->setStatusTip(tr("调整颜色的色相、饱和度与明度"));
    connect(m_actionHueSaturation, &QAction::triggered, this, &MainWindow::openHueSaturation);

    m_actionCurves = new QAction(tr("曲线(&C)..."), this);
    m_actionCurves->setObjectName(QStringLiteral("actionCurves"));
    m_actionCurves->setIcon(icons::icon(QStringLiteral("adjust_curves")));
    m_actionCurves->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_M));
    m_actionCurves->setStatusTip(tr("用控制点曲线精细调整各通道的明暗映射"));
    connect(m_actionCurves, &QAction::triggered, this, &MainWindow::openCurves);

    m_actionLevels = new QAction(tr("色阶(&L)..."), this);
    m_actionLevels->setObjectName(QStringLiteral("actionLevels"));
    m_actionLevels->setIcon(icons::icon(QStringLiteral("adjust_levels")));
    // Ctrl+L 也是 Photoshop 的约定，且当前未被占用（已核对全部快捷键）
    m_actionLevels->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_L));
    m_actionLevels->setStatusTip(tr("用直方图的黑白场与中间调调整明暗分布"));
    connect(m_actionLevels, &QAction::triggered, this, &MainWindow::openLevels);

    // ─────────────────────────────────────────────────────────────────────
    //  图像尺寸（图像大小 / 画布大小）
    // ─────────────────────────────────────────────────────────────────────

    // ★ 快捷键对齐 Photoshop ★
    //   Ctrl+Alt+I —— Image Size
    //   Ctrl+Alt+C —— Canvas Size
    //   这两个组合在 PS 用户手里是肌肉记忆，换掉会持续制造误操作。
    //   （已核对本工程其余快捷键，无冲突。）
    m_actionImageSize = new QAction(tr("图像大小(&I)..."), this);
    m_actionImageSize->setObjectName(QStringLiteral("actionImageSize"));
    m_actionImageSize->setIcon(icons::icon(QStringLiteral("image_size")));
    m_actionImageSize->setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_I));
    m_actionImageSize->setStatusTip(tr("缩放像素内容（需要重采样，图像会变软或变锐）"));
    connect(m_actionImageSize, &QAction::triggered, this, &MainWindow::openImageSize);

    m_actionCanvasSize = new QAction(tr("画布大小(&C)..."), this);
    m_actionCanvasSize->setObjectName(QStringLiteral("actionCanvasSize"));
    m_actionCanvasSize->setIcon(icons::icon(QStringLiteral("canvas_size")));
    m_actionCanvasSize->setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_C));
    m_actionCanvasSize->setStatusTip(tr("改变画布范围：不缩放像素，缩小时裁切、放大时补底色"));
    connect(m_actionCanvasSize, &QAction::triggered, this, &MainWindow::openCanvasSize);

    // ─────────────────────────────────────────────────────────────────────
    //  图像旋转 / 翻转
    // ─────────────────────────────────────────────────────────────────────

    m_actionRotate90CW = new QAction(tr("顺时针旋转 90°"), this);
    m_actionRotate90CW->setObjectName(QStringLiteral("actionRotate90CW"));
    m_actionRotate90CW->setIcon(icons::icon(QStringLiteral("rotate_cw")));
    m_actionRotate90CW->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_R));
    connect(m_actionRotate90CW, &QAction::triggered, this, &MainWindow::rotate90CW);

    m_actionRotate90CCW = new QAction(tr("逆时针旋转 90°"), this);
    m_actionRotate90CCW->setObjectName(QStringLiteral("actionRotate90CCW"));
    m_actionRotate90CCW->setIcon(icons::icon(QStringLiteral("rotate_ccw")));
    m_actionRotate90CCW->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_R));
    connect(m_actionRotate90CCW, &QAction::triggered, this, &MainWindow::rotate90CCW);

    m_actionRotate180 = new QAction(tr("旋转 180°"), this);
    m_actionRotate180->setObjectName(QStringLiteral("actionRotate180"));
    m_actionRotate180->setIcon(icons::icon(QStringLiteral("rotate_180")));
    connect(m_actionRotate180, &QAction::triggered, this, &MainWindow::rotate180);

    // ★ 这一块曾经"漏写"过一次 ★
    //   菜单里 addAction(m_actionRotateArbitrary) 在，而 new QAction 不在 ——
    //   结果 imageMenu->addAction(nullptr) 静默什么都不做，菜单项凭空消失。
    //   编译器不报错（nullptr 是合法实参）、离线自检也测不到（自检不建菜单），
    //   只有真正跑起来看菜单才暴露。改动作定义时务必"new + objectName + connect +
    //   addAction"四处齐全。
    m_actionRotateArbitrary = new QAction(tr("任意角度旋转(&A)…"), this);
    m_actionRotateArbitrary->setObjectName(QStringLiteral("actionRotateArbitrary"));
    m_actionRotateArbitrary->setIcon(icons::icon(QStringLiteral("rotate_arbitrary")));
    connect(m_actionRotateArbitrary, &QAction::triggered,
            this, &MainWindow::openRotateArbitrary);

    m_actionFlipH = new QAction(tr("水平翻转"), this);
    m_actionFlipH->setObjectName(QStringLiteral("actionFlipH"));
    m_actionFlipH->setIcon(icons::icon(QStringLiteral("flip_h")));
    m_actionFlipH->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_H));
    connect(m_actionFlipH, &QAction::triggered, this, &MainWindow::flipHorizontal);

    m_actionFlipV = new QAction(tr("垂直翻转"), this);
    m_actionFlipV->setObjectName(QStringLiteral("actionFlipV"));
    m_actionFlipV->setIcon(icons::icon(QStringLiteral("flip_v")));
    connect(m_actionFlipV, &QAction::triggered, this, &MainWindow::flipVertical);

    // ── 裁剪（裁到当前选区的包围盒）──
    //
    // ★ 与工具箱里的裁剪工具是两条不同的路 ★
    //   菜单项一次点击就完成，操作数取**当前选区的包围盒** —— 用户已经用
    //   选框表达过"我要这一块"，再让他拿裁剪工具重框一遍是重复劳动。
    //   没有选区时置灰（不用内容包围盒兜底，那是「裁切到内容」的语义）。
    //   完整理由见 docs/画布与文件操作设计.md 6.8。
    m_actionCrop = new QAction(tr("裁剪"), this);
    m_actionCrop->setObjectName(QStringLiteral("actionCrop"));
    m_actionCrop->setIcon(icons::icon(QStringLiteral("tool_crop")));
    m_actionCrop->setStatusTip(tr("把画布裁剪到当前选区的包围盒"));
    connect(m_actionCrop, &QAction::triggered, this, &MainWindow::cropToSelection);

    // ─────────────────────────────────────────────────────────────────────
    //  图层
    // ─────────────────────────────────────────────────────────────────────

    m_actionLayerNew = new QAction(tr("新建图层(&N)"), this);
    m_actionLayerNew->setObjectName(QStringLiteral("actionLayerNew"));
    m_actionLayerNew->setIcon(icons::icon(QStringLiteral("layer_new")));
    m_actionLayerNew->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_N));
    connect(m_actionLayerNew, &QAction::triggered, this, &MainWindow::addNewLayer);

    m_actionLayerDuplicate = new QAction(tr("复制图层(&D)"), this);
    m_actionLayerDuplicate->setObjectName(QStringLiteral("actionLayerDuplicate"));
    m_actionLayerDuplicate->setIcon(icons::icon(QStringLiteral("layer_duplicate")));
    m_actionLayerDuplicate->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_J));
    connect(m_actionLayerDuplicate, &QAction::triggered,
            this, &MainWindow::duplicateCurrentLayer);

    m_actionLayerDelete = new QAction(tr("删除图层"), this);
    m_actionLayerDelete->setObjectName(QStringLiteral("actionLayerDelete"));
    m_actionLayerDelete->setIcon(icons::icon(QStringLiteral("layer_delete")));
    connect(m_actionLayerDelete, &QAction::triggered, this, &MainWindow::deleteCurrentLayer);

    m_actionLayerMoveUp = new QAction(tr("上移图层"), this);
    m_actionLayerMoveUp->setObjectName(QStringLiteral("actionLayerMoveUp"));
    m_actionLayerMoveUp->setIcon(icons::icon(QStringLiteral("layer_up")));
    m_actionLayerMoveUp->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_BracketRight));
    connect(m_actionLayerMoveUp, &QAction::triggered, this, &MainWindow::moveLayerUp);

    m_actionLayerMoveDown = new QAction(tr("下移图层"), this);
    m_actionLayerMoveDown->setObjectName(QStringLiteral("actionLayerMoveDown"));
    m_actionLayerMoveDown->setIcon(icons::icon(QStringLiteral("layer_down")));
    m_actionLayerMoveDown->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_BracketLeft));
    connect(m_actionLayerMoveDown, &QAction::triggered, this, &MainWindow::moveLayerDown);

    m_actionMergeDown = new QAction(tr("向下合并(&M)"), this);
    m_actionMergeDown->setObjectName(QStringLiteral("actionMergeDown"));
    m_actionMergeDown->setIcon(icons::icon(QStringLiteral("layer_merge_down")));
    m_actionMergeDown->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_E));
    connect(m_actionMergeDown, &QAction::triggered, this, &MainWindow::mergeLayerDown);

    m_actionFlatten = new QAction(tr("拼合图像(&F)"), this);
    m_actionFlatten->setObjectName(QStringLiteral("actionFlatten"));
    m_actionFlatten->setIcon(icons::icon(QStringLiteral("layer_flatten")));
    connect(m_actionFlatten, &QAction::triggered, this, &MainWindow::flattenImage);

    m_actionLayerMergeVisible = new QAction(tr("合并可见图层(&V)"), this);
    m_actionLayerMergeVisible->setObjectName(QStringLiteral("actionLayerMergeVisible"));
    m_actionLayerMergeVisible->setIcon(icons::icon(QStringLiteral("layer_merge_visible")));
    // Ctrl+Shift+E：与"向下合并"的 Ctrl+E 成对，Shift 表示"范围更大"。
    m_actionLayerMergeVisible->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_E));
    connect(m_actionLayerMergeVisible, &QAction::triggered,
            this, &MainWindow::mergeVisibleLayers);

    m_actionLayerRename = new QAction(tr("重命名图层(&R)"), this);
    m_actionLayerRename->setObjectName(QStringLiteral("actionLayerRename"));
    m_actionLayerRename->setIcon(icons::icon(QStringLiteral("layer_rename")));
    // ★ 用 F2 而不是回车 ★
    //   回车在图层面板里已经被"确认重命名"占用了；F2 是各平台通用的
    //   "重命名当前项"，而且它不会和列表的键盘导航冲突。
    m_actionLayerRename->setShortcut(QKeySequence(Qt::Key_F2));
    connect(m_actionLayerRename, &QAction::triggered,
            this, &MainWindow::renameActiveLayer);

    // ─────────────────────────────────────────────────────────────────────
    //  视图
    // ─────────────────────────────────────────────────────────────────────

    m_actionZoomIn = new QAction(tr("放大(&I)"), this);
    m_actionZoomIn->setObjectName(QStringLiteral("actionZoomIn"));
    m_actionZoomIn->setIcon(icons::icon(QStringLiteral("view_zoom_in")));
    // QKeySequence::ZoomIn 会解析成 Ctrl++（同时接受 Ctrl+= 的变体）
    m_actionZoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(m_actionZoomIn, &QAction::triggered, this, &MainWindow::zoomIn);

    m_actionZoomOut = new QAction(tr("缩小(&O)"), this);
    m_actionZoomOut->setObjectName(QStringLiteral("actionZoomOut"));
    m_actionZoomOut->setIcon(icons::icon(QStringLiteral("view_zoom_out")));
    m_actionZoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(m_actionZoomOut, &QAction::triggered, this, &MainWindow::zoomOut);

    m_actionZoomFit = new QAction(tr("适合窗口(&F)"), this);
    m_actionZoomFit->setObjectName(QStringLiteral("actionZoomFit"));
    m_actionZoomFit->setIcon(icons::icon(QStringLiteral("view_zoom_fit")));
    m_actionZoomFit->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_0));
    connect(m_actionZoomFit, &QAction::triggered, this, &MainWindow::zoomToFit);

    m_actionZoomActual = new QAction(tr("实际大小(&A)"), this);
    m_actionZoomActual->setObjectName(QStringLiteral("actionZoomActual"));
    m_actionZoomActual->setIcon(icons::icon(QStringLiteral("view_zoom_100")));
    m_actionZoomActual->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_1));
    connect(m_actionZoomActual, &QAction::triggered, this, &MainWindow::zoomToActualSize);

    m_actionShowGrid = new QAction(tr("显示网格(&G)"), this);
    m_actionShowGrid->setObjectName(QStringLiteral("actionShowGrid"));
    m_actionShowGrid->setIcon(icons::icon(QStringLiteral("view_grid")));
    m_actionShowGrid->setCheckable(true);
    m_actionShowGrid->setChecked(false);
    connect(m_actionShowGrid, &QAction::toggled, this, &MainWindow::toggleGrid);

    m_actionShowCheckerboard = new QAction(tr("显示透明背景"), this);
    m_actionShowCheckerboard->setObjectName(QStringLiteral("actionShowCheckerboard"));
    m_actionShowCheckerboard->setIcon(icons::icon(QStringLiteral("view_checkerboard")));
    m_actionShowCheckerboard->setCheckable(true);
    m_actionShowCheckerboard->setChecked(true);
    connect(m_actionShowCheckerboard, &QAction::toggled,
            this, &MainWindow::toggleCheckerboard);

    // ── 网格的间距与吸附（阶段七收尾）──
    //
    //  ★ 快捷键的取舍 ★
    //    参考线「显示」占了 Ctrl+;、「锁定」占了 Ctrl+Alt+; ——
    //    吸附与它俩同族，于是顺势用 **Ctrl+Shift+;**（三连）。
    //    "显示网格"本身没给快捷键（它是纯呈现，用得没吸附频繁）。
    m_actionSnapToGrid = new QAction(tr("对齐到网格(&S)"), this);
    m_actionSnapToGrid->setObjectName(QStringLiteral("actionSnapToGrid"));
    m_actionSnapToGrid->setIcon(icons::icon(QStringLiteral("view_grid_snap")));
    m_actionSnapToGrid->setCheckable(true);
    m_actionSnapToGrid->setChecked(false);
    m_actionSnapToGrid->setShortcut(
        QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Semicolon));
    connect(m_actionSnapToGrid, &QAction::toggled, this, &MainWindow::toggleSnapToGrid);

    // 「网格间距」子菜单：6 个预设 + 1 个「自定义…」
    //
    //  ★ 预设项**不设 objectName** ★
    //    与本工程"有名字的菜单动作必须有图标"那条规则一致 —— 牵连进来
    //    就得给 6 项各配一张图（纯噪音）。定位它们靠子菜单本身 + 下标。
    {
        auto *spacingGroup = new QActionGroup(this);
        spacingGroup->setExclusive(true);

        for (int i = 0; i < kGridPresetCount; ++i) {
            QAction *act = new QAction(tr("%1 像素").arg(kGridPresets[i]), this);
            act->setCheckable(true);
            act->setChecked(kGridPresets[i] == 64);   // 与 CanvasView 的默认值一致
            // ★ 把"这一项代表哪个间距"存进 data() ★
            //   退出/自定义设置后要靠它把勾选态同步回来。存值而不是解析
            //   显示文本 —— 文本是翻译过的，拿它当协议换语言就静默失效。
            act->setData(kGridPresets[i]);
            connect(act, &QAction::triggered, this,
                    [this, i] { setGridSpacing(kGridPresets[i]); });
            spacingGroup->addAction(act);
            m_actionGridSpacing.append(act);
        }
    }

    m_actionGridCustom = new QAction(tr("自定义(&U)..."), this);
    m_actionGridCustom->setObjectName(QStringLiteral("actionGridCustom"));
    m_actionGridCustom->setIcon(icons::icon(QStringLiteral("view_grid_spacing")));
    connect(m_actionGridCustom, &QAction::triggered, this, &MainWindow::editGridSettings);

    // ── 标尺与参考线（阶段七：视图辅助）──
    //
    //  ★ 快捷键的取舍 ★
    //    Photoshop 里"标尺"是 Ctrl+R，但本工程的 Ctrl+R 已经是
    //    「顺时针旋转 90°」—— 那更常用，而且是**破坏性操作**（会改像素），
    //    不该给一个纯视图开关让位。于是标尺挪到 Ctrl+Alt+R。
    //    参考线的 Ctrl+; / Ctrl+Alt+; 在本工程没有占用，按惯例直接用。
    m_actionShowRuler = new QAction(tr("显示标尺(&R)"), this);
    m_actionShowRuler->setObjectName(QStringLiteral("actionShowRuler"));
    m_actionShowRuler->setIcon(icons::icon(QStringLiteral("view_ruler")));
    m_actionShowRuler->setCheckable(true);
    m_actionShowRuler->setChecked(true);              // 默认显示
    m_actionShowRuler->setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_R));
    connect(m_actionShowRuler, &QAction::toggled, this, &MainWindow::toggleRuler);

    m_actionShowGuides = new QAction(tr("显示参考线(&G)"), this);
    m_actionShowGuides->setObjectName(QStringLiteral("actionShowGuides"));
    m_actionShowGuides->setIcon(icons::icon(QStringLiteral("view_guides")));
    m_actionShowGuides->setCheckable(true);
    m_actionShowGuides->setChecked(true);
    m_actionShowGuides->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Semicolon));
    connect(m_actionShowGuides, &QAction::toggled, this, &MainWindow::toggleGuides);

    m_actionLockGuides = new QAction(tr("锁定参考线(&L)"), this);
    m_actionLockGuides->setObjectName(QStringLiteral("actionLockGuides"));
    m_actionLockGuides->setIcon(icons::icon(QStringLiteral("view_guides_lock")));
    m_actionLockGuides->setCheckable(true);
    m_actionLockGuides->setChecked(false);
    m_actionLockGuides->setShortcut(
        QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_Semicolon));
    connect(m_actionLockGuides, &QAction::toggled,
            this, &MainWindow::toggleGuidesLocked);

    m_actionClearGuides = new QAction(tr("清除参考线(&C)"), this);
    m_actionClearGuides->setObjectName(QStringLiteral("actionClearGuides"));
    m_actionClearGuides->setIcon(icons::icon(QStringLiteral("view_guides_clear")));
    connect(m_actionClearGuides, &QAction::triggered, this, &MainWindow::clearGuides);

    // ── 「标尺单位」子菜单：5 个互斥项 ──
    //
    //  ★ 刻意**不设 objectName** ★
    //    对账脚本与自检有一条"有名字的菜单动作必须有图标"的规则；把它们
    //    牵连进来，就得给 5 个单选项各配一张图标（或共用一个）—— 纯噪音。
    //    定位它们靠子菜单本身 + setRulerUnit(下标)。
    //
    //  ★ 顺序完全由 rulermetrics::unitAt() 决定 ★
    //    菜单标签顺序、QSettings 里存的下标、枚举值三者同源，不会错位。
    {
        auto *unitGroup = new QActionGroup(this);
        unitGroup->setExclusive(true);

        const int defaultIndex =
            rulermetrics::unitIndex(rulermetrics::Unit::Pixel);
        for (int i = 0; i < rulermetrics::kUnitCount; ++i) {
            QAction *act =
                new QAction(rulerUnitName(rulermetrics::unitAt(i)), this);
            act->setCheckable(true);
            act->setChecked(i == defaultIndex);
            connect(act, &QAction::triggered, this,
                    [this, i] { setRulerUnit(i); });
            unitGroup->addAction(act);
            m_actionRulerUnit.append(act);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  颜色
    // ─────────────────────────────────────────────────────────────────────

    m_actionForegroundColor = new QAction(tr("设置前景色..."), this);
    m_actionForegroundColor->setObjectName(QStringLiteral("actionForegroundColor"));
    m_actionForegroundColor->setIcon(icons::icon(QStringLiteral("color_foreground")));
    connect(m_actionForegroundColor, &QAction::triggered,
            this, &MainWindow::chooseForegroundColor);

    m_actionBackgroundColor = new QAction(tr("设置背景色..."), this);
    m_actionBackgroundColor->setObjectName(QStringLiteral("actionBackgroundColor"));
    m_actionBackgroundColor->setIcon(icons::icon(QStringLiteral("color_background")));
    connect(m_actionBackgroundColor, &QAction::triggered,
            this, &MainWindow::chooseBackgroundColor);

    // ★ 关于 X / D 这两个无修饰键的快捷键 ★
    //
    // 它们是 Photoshop 的约定，但"无修饰键"意味着任何输入焦点在
    // 文本框里的时候都可能被误触发（用户想输入字母 X，结果改了颜色）。
    //
    // 处理方式：不在 QAction 上设快捷键（那样 Qt 会做全局拦截，
    // 无法排除文本框场景），而是在 MainWindow::keyPressEvent 里
    // 手动判断 —— 只有焦点不在输入控件上时才处理。
    // 见 MainWindowCommands.cpp 的 keyPressEvent。
    m_actionSwapColors = new QAction(tr("交换前景/背景色"), this);
    m_actionSwapColors->setObjectName(QStringLiteral("actionSwapColors"));
    m_actionSwapColors->setIcon(icons::icon(QStringLiteral("color_swap")));
    connect(m_actionSwapColors, &QAction::triggered, this, &MainWindow::swapColors);

    m_actionResetColors = new QAction(tr("恢复默认颜色"), this);
    m_actionResetColors->setObjectName(QStringLiteral("actionResetColors"));
    m_actionResetColors->setIcon(icons::icon(QStringLiteral("color_reset")));
    connect(m_actionResetColors, &QAction::triggered, this, &MainWindow::resetColors);

    // ─────────────────────────────────────────────────────────────────────
    //  帮助
    // ─────────────────────────────────────────────────────────────────────

    m_actionAbout = new QAction(tr("关于 %1(&A)...").arg(zl::appinfo::displayName()), this);
    m_actionAbout->setObjectName(QStringLiteral("actionAbout"));
    m_actionAbout->setIcon(icons::icon(QStringLiteral("help_about")));
    m_actionAbout->setMenuRole(QAction::AboutRole);
    connect(m_actionAbout, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, tr("关于 %1").arg(zl::appinfo::displayName()),
            tr("<h3>%1</h3>"
               "<p>一个基于 Qt 的轻量级图片编辑器。</p>"
               "<p><b>技术栈</b><br>"
               "Qt %2（编译期版本 %3）<br>"
               "纯 Qt 实现，不依赖任何第三方图像库</p>"
               "<p><b>核心特性</b><br>"
               "多图层与 22 种混合模式 · 灰度蒙版选区（支持羽化）<br>"
               "曲线 / 色阶 / 亮度对比度 · 自实现二次立方与 Lanczos 重采样<br>"
               "脏矩形与整层快照双撤销策略 · EXIF 自动旋转</p>")
                .arg(zl::appinfo::titleSuffix())
                .arg(zl::compat::qtVersionString())
                .arg(QStringLiteral(QT_VERSION_STR)));
    });

    m_actionAboutQt = new QAction(tr("关于 Qt(&Q)..."), this);
    m_actionAboutQt->setObjectName(QStringLiteral("actionAboutQt"));
    m_actionAboutQt->setIcon(icons::icon(QStringLiteral("help_qt")));
    m_actionAboutQt->setMenuRole(QAction::AboutQtRole);
    connect(m_actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
}

// ============================================================================
//  setupMenus
// ============================================================================

void MainWindow::setupMenus()
{
    QMenuBar *mb = menuBar();

    // ─────────────────────────────────────────────────────────────────────
    //  文件
    // ─────────────────────────────────────────────────────────────────────
    QMenu *fileMenu = mb->addMenu(tr("文件(&F)"));
    fileMenu->addAction(m_actionNew);
    fileMenu->addAction(m_actionOpen);
    // ★ 紧跟「打开」之后 ★
    //   两者是"从文件拿东西进来"的一组，放一起用户才好对比：
    //   「打开」换掉当前这张，「打开为图层」保留当前这张、再叠一张上去。
    fileMenu->addAction(m_actionOpenAsLayer);

    // ── 屏幕截图（子菜单）──
    //
    // ★ 为什么放在「打开为图层」之后 ★
    //   它和上面两项同属"把外面的东西拿进来"这一组：
    //   新建 / 打开是"从磁盘拿"，截图是"从屏幕拿"。
    //   放一起用户才好对照"我要的东西从哪来"。
    //
    // ★ 子菜单自己也要图标 ★
    //   addMenu() 返回的 QMenu 自带一个 menuAction()，它就是菜单里那一行
    //   （与普通菜单项走同一套绘制），所以不挂图标的话它会在
    //   "一栏都有图标"的菜单里明显掉队。
    QMenu *captureMenu = fileMenu->addMenu(tr("屏幕截图(&C)"));
    captureMenu->setObjectName(QStringLiteral("menuScreenCapture"));
    captureMenu->setIcon(icons::icon(QStringLiteral("menu_screen_capture")));
    captureMenu->addAction(m_actionCaptureNow);
    // 三条延迟项归成一组：同一个动作的三个档，与"立即"之间用横线分开
    captureMenu->addSeparator();
    captureMenu->addAction(m_actionCaptureDelay3);
    captureMenu->addAction(m_actionCaptureDelay5);
    captureMenu->addAction(m_actionCaptureDelay10);

    // 最近打开（内容由 updateRecentFileActions 动态填充）
    QMenu *recentMenu = fileMenu->addMenu(tr("最近打开(&R)"));
    recentMenu->setObjectName(QStringLiteral("menuRecentFiles"));
    // ★ 子菜单也要图标 ★
    //   QMenu::addMenu() 返回的 QMenu 自带一个 menuAction()，
    //   setIcon() 就是给那个动作挂图 —— 它和普通菜单项走同一套绘制，
    //   所以子菜单在有图标的菜单里不会显得"掉队"。
    recentMenu->setIcon(icons::icon(QStringLiteral("menu_recent")));
    for (int i = 0; i < kMaxRecentFiles; ++i) {
        auto *action = new QAction(this);
        action->setVisible(false);
        // ★ 路径存在 QAction 的 data() 里，而不是用 lambda 捕获 ★
        //   捕获的话每次刷新列表都要重建全部 QAction（以及它们的连接），
        //   用 data() 则只改文本与 data，动作对象复用。
        connect(action, &QAction::triggered, this, [this, action]() {
            const QString path = action->data().toString();
            if (!path.isEmpty())
                openDocumentFromPath(path);
        });
        m_recentFileActions.append(action);
        recentMenu->addAction(action);
    }
    recentMenu->addSeparator();
    auto *clearRecentAction = recentMenu->addAction(tr("清空列表"));
    connect(clearRecentAction, &QAction::triggered, this, [this]() {
        QSettings settings;
        settings.remove(QStringLiteral("files/recent"));
        updateRecentFileActions();
    });

    fileMenu->addSeparator();
    fileMenu->addAction(m_actionSave);
    fileMenu->addAction(m_actionSaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actionClose);
    fileMenu->addAction(m_actionQuit);

    // ─────────────────────────────────────────────────────────────────────
    //  编辑
    // ─────────────────────────────────────────────────────────────────────
    QMenu *editMenu = mb->addMenu(tr("编辑(&E)"));
    editMenu->addAction(m_actionUndo);
    editMenu->addAction(m_actionRedo);
    editMenu->addSeparator();
    editMenu->addAction(m_actionCut);
    editMenu->addAction(m_actionCopy);
    editMenu->addAction(m_actionPaste);
    editMenu->addAction(m_actionPasteInto);
    editMenu->addSeparator();
    editMenu->addAction(m_actionDelete);

    // ─────────────────────────────────────────────────────────────────────
    //  选择
    // ─────────────────────────────────────────────────────────────────────
    QMenu *selectMenu = mb->addMenu(tr("选择(&S)"));
    selectMenu->addAction(m_actionSelectAll);
    selectMenu->addAction(m_actionDeselect);
    selectMenu->addAction(m_actionInvertSelection);

    // ─────────────────────────────────────────────────────────────────────
    //  调整
    // ─────────────────────────────────────────────────────────────────────
    QMenu *adjustMenu = mb->addMenu(tr("调整(&A)"));
    adjustMenu->addAction(m_actionBrightnessContrast);
    adjustMenu->addAction(m_actionHueSaturation);
    adjustMenu->addSeparator();
    adjustMenu->addAction(m_actionCurves);
    adjustMenu->addAction(m_actionLevels);
    adjustMenu->addSeparator();
    adjustMenu->addAction(m_actionGrayscale);
    adjustMenu->addAction(m_actionInvert);

    // ─────────────────────────────────────────────────────────────────────
    //  图像
    // ─────────────────────────────────────────────────────────────────────
    QMenu *imageMenu = mb->addMenu(tr("图像(&I)"));
    imageMenu->addAction(m_actionImageSize);
    imageMenu->addAction(m_actionCanvasSize);
    imageMenu->addSeparator();
    imageMenu->addAction(m_actionRotate90CW);
    imageMenu->addAction(m_actionRotate90CCW);
    imageMenu->addAction(m_actionRotate180);
    imageMenu->addAction(m_actionRotateArbitrary);
    imageMenu->addSeparator();
    imageMenu->addAction(m_actionFlipH);
    imageMenu->addAction(m_actionFlipV);
    imageMenu->addSeparator();
    imageMenu->addAction(m_actionCrop);

    // ★ 菜单弹出时刷新「裁剪」的可用性 ★
    //   文档目前没有"选区已变化"信号（选区在 core 里，发信号要改 Document），
    //   而菜单项只在弹出那一刻可见 —— 所以"弹出时问一次"就够，
    //   比为此新增一个信号更省，也不会出现状态过期。
    connect(imageMenu, &QMenu::aboutToShow, this, [this]() {
        if (m_actionCrop)
            m_actionCrop->setEnabled(m_doc && m_doc->selection().isActive());
    });

    // ─────────────────────────────────────────────────────────────────────
    //  图层
    // ─────────────────────────────────────────────────────────────────────
    QMenu *layerMenu = mb->addMenu(tr("图层(&L)"));
    layerMenu->addAction(m_actionLayerNew);
    layerMenu->addAction(m_actionLayerDuplicate);
    layerMenu->addAction(m_actionLayerDelete);
    layerMenu->addSeparator();
    layerMenu->addAction(m_actionLayerMoveUp);
    layerMenu->addAction(m_actionLayerMoveDown);
    layerMenu->addSeparator();
    layerMenu->addAction(m_actionMergeDown);
    layerMenu->addAction(m_actionLayerMergeVisible);
    layerMenu->addAction(m_actionFlatten);
    layerMenu->addSeparator();
    layerMenu->addAction(m_actionLayerRename);

    // ─────────────────────────────────────────────────────────────────────
    //  视图
    // ─────────────────────────────────────────────────────────────────────
    QMenu *viewMenu = mb->addMenu(tr("视图(&V)"));
    viewMenu->addAction(m_actionZoomIn);
    viewMenu->addAction(m_actionZoomOut);
    viewMenu->addAction(m_actionZoomFit);
    viewMenu->addAction(m_actionZoomActual);
    viewMenu->addSeparator();
    viewMenu->addAction(m_actionShowGrid);
    viewMenu->addAction(m_actionShowCheckerboard);

    // ── 网格的间距与吸附 ──
    viewMenu->addAction(m_actionSnapToGrid);

    QMenu *gridSpacingMenu = viewMenu->addMenu(tr("网格间距(&Z)"));
    gridSpacingMenu->setObjectName(QStringLiteral("menuGridSpacing"));
    gridSpacingMenu->setIcon(icons::icon(QStringLiteral("view_grid")));
    for (QAction *act : m_actionGridSpacing)
        gridSpacingMenu->addAction(act);
    gridSpacingMenu->addSeparator();
    gridSpacingMenu->addAction(m_actionGridCustom);

    // ── 视图辅助：标尺与参考线 ──
    viewMenu->addSeparator();
    viewMenu->addAction(m_actionShowRuler);

    // 「标尺单位」子菜单。★ 它自己带图标，5 个单选项不带 ★
    QMenu *unitMenu = viewMenu->addMenu(tr("标尺单位(&N)"));
    unitMenu->setObjectName(QStringLiteral("menuRulerUnit"));
    unitMenu->setIcon(icons::icon(QStringLiteral("view_ruler")));
    for (QAction *act : m_actionRulerUnit)
        unitMenu->addAction(act);

    viewMenu->addSeparator();
    viewMenu->addAction(m_actionShowGuides);
    viewMenu->addAction(m_actionLockGuides);
    viewMenu->addAction(m_actionClearGuides);
    viewMenu->addSeparator();

    // 面板显隐菜单。内容在 setupDocks 里填充
    // （因为那时 QDockWidget 才存在）。
    QMenu *panelMenu = viewMenu->addMenu(tr("面板(&P)"));
    panelMenu->setObjectName(QStringLiteral("menuPanels"));
    panelMenu->setIcon(icons::icon(QStringLiteral("menu_panels")));

    // ─────────────────────────────────────────────────────────────────────
    //  帮助
    // ─────────────────────────────────────────────────────────────────────
    QMenu *helpMenu = mb->addMenu(tr("帮助(&H)"));
    helpMenu->addAction(m_actionAbout);
    helpMenu->addAction(m_actionAboutQt);
}

// ============================================================================
//  setupToolBars
// ============================================================================

void MainWindow::setupToolBars()
{
    // ─────────────────────────────────────────────────────────────────────
    //  第一行：主工具栏（文件 + 编辑 + 视图）
    // ─────────────────────────────────────────────────────────────────────
    m_mainToolBar = addToolBar(tr("主工具栏"));
    m_mainToolBar->setObjectName(QStringLiteral("mainToolBar"));
    // ★ 不可移动 ★
    //   允许移动的话，用户很容易不小心把工具栏拖成浮动窗口，
    //   然后找不到怎么放回去（虽然双击能复位，但不是所有人都知道）。
    //   对一个"轻量级"编辑器，固定布局是更友好的选择。
    m_mainToolBar->setMovable(false);
    m_mainToolBar->setFloatable(false);

    m_mainToolBar->addAction(m_actionNew);
    m_mainToolBar->addAction(m_actionOpen);
    m_mainToolBar->addAction(m_actionSave);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actionUndo);
    m_mainToolBar->addAction(m_actionRedo);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actionZoomOut);
    m_mainToolBar->addAction(m_actionZoomFit);
    m_mainToolBar->addAction(m_actionZoomIn);

    // ─────────────────────────────────────────────────────────────────────
    //  第二行：工具选项条
    // ─────────────────────────────────────────────────────────────────────
    //
    // ★ addToolBarBreak 让这个工具栏换到下一行 ★
    //   不加的话，Qt 会尝试把两个工具栏并排放在同一行 ——
    //   选项条会被压得很窄，控件挤在一起。
    addToolBarBreak();

    QToolBar *optionsToolBar = addToolBar(tr("工具选项"));
    optionsToolBar->setObjectName(QStringLiteral("toolOptionsToolBar"));
    optionsToolBar->setMovable(false);
    optionsToolBar->setFloatable(false);

    m_optionsBar = new ToolOptionsBar(this);
    // ★ QToolBar::addWidget 会接管尺寸策略 ★
    //   它会设一个 Expanding 的尺寸策略，让控件撑满可用宽度。
    //   这也意味着 ToolOptionsBar 不需要自己处理"宽度不够时怎么办" ——
    //   工具栏会自动出现一个溢出按钮。
    optionsToolBar->addWidget(m_optionsBar);

    // ─────────────────────────────────────────────────────────────────────
    //  左侧：工具箱（竖排工具按钮）
    // ─────────────────────────────────────────────────────────────────────
    //
    // ★ 为什么在工具栏里而不是 Dock ★
    //   工具箱需要"窄、固定宽、不能浮动、不能关闭"，这正是工具栏的行为。
    //   放在左侧工具栏区域视觉上与 Dock 无异，
    //   但不需要处理 Dock 的浮动/关闭/嵌套逻辑。
    m_toolsToolBar = new QToolBar(tr("工具箱"), this);
    m_toolsToolBar->setObjectName(QStringLiteral("toolsToolBar"));
    m_toolsToolBar->setOrientation(Qt::Vertical);
    m_toolsToolBar->setMovable(false);
    m_toolsToolBar->setFloatable(false);
    m_toolsToolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_toolsToolBar->setIconSize(QSize(24, 24));

    addToolBar(Qt::LeftToolBarArea, m_toolsToolBar);

    // ★ 工具按钮的填充**不在这里** ★
    //
    //   buildToolBox() 需要 m_toolManager 已经建好、且工具都已注册，
    //  那是 setupTools() 做的事，而构造函数里 setupToolBars() 跑在
    //  它**前面**（见 MainWindow 构造函数的初始化顺序注释）。
    //   写在这里会因 m_toolManager == nullptr 而静默 return，
    //   结果工具箱一个按钮都没有 —— 而且编译器与离线自检都发现不了。
    //   调用点已挪到 setupTools() 里，紧跟在 loadAllOptions() 之后。
}

// ============================================================================
//  buildToolBox —— 生成工具按钮
// ============================================================================

void MainWindow::buildToolBox()
{
    if (!m_toolsToolBar || !m_toolManager)
        return;

    // 清空旧内容（理论上只会调用一次，但保持幂等性更安全）
    m_toolsToolBar->clear();
    m_toolActions.clear();

    // ★ 用 QActionGroup 实现互斥选中 ★
    //   手写"点了 B 就取消 A 的选中"很容易在边界情况下漏掉
    //   （例如通过快捷键切换工具时忘了同步按钮状态）。
    //   QActionGroup 自动处理这一切。
    auto *group = new QActionGroup(this);
    group->setExclusive(true);

    for (AbstractTool *tool : m_toolManager->tools()) {
        if (!tool)
            continue;

        auto *action = new QAction(tool->displayName(), this);
        action->setObjectName(QStringLiteral("toolAction_") + tool->id());
        action->setCheckable(true);

        // 图标：三级降级 —— 专用 SVG 图标 → 工具自带的图标 → 字母占位图。
        //
        //   ★ 最后那一级不是"以防万一"，而是必须留着 ★
        //     工具箱是 IconOnly 模式，一个没有图标的按钮是**完全空白**的 ——
        //     比显示一个字母更糟（用户根本不知道该点哪）。
        //     所以"新加了一个工具但忘了配图标"后果只是丑，
        //     而不是工具箱里凭空少一个按钮。
        //     至于"有没有配"，由 icon_test 逐个工具 id 去盯。
        QIcon icon = icons::iconForTool(tool->id());
        if (icon.isNull())
            icon = tool->icon();
        if (icon.isNull())
            icon = makeLetterIcon(tool->displayName());
        action->setIcon(icon);

        // 提示文字里带上快捷键
        QString tip = tool->displayName();
        const int key = tool->shortcutKey();
        if (key > 0) {
            tip += QStringLiteral("  (%1)")
                       .arg(QKeySequence(key).toString(QKeySequence::NativeText));
        }
        action->setToolTip(tip);
        action->setStatusTip(tr("切换到%1工具").arg(tool->displayName()));

        // ★ 点击 → 切换工具 ★
        //   用 QAction::triggered 而不是 toggled：
        //   toggled 在"取消选中"时也会触发（例如另一个按钮被点中时），
        //   那会导致工具被反复切换。
        const QString toolId = tool->id();
        connect(action, &QAction::triggered, this, [this, toolId]() {
            if (m_toolManager)
                m_toolManager->setActiveTool(toolId);
        });

        group->addAction(action);
        m_toolsToolBar->addAction(action);
        m_toolActions.insert(toolId, action);
    }

    // ── 底部加一个"颜色块"指示器 ──
    //
    // 这不是一个工具按钮，而是"当前前景色"的视觉提示，
    // 与右侧颜色面板里的色块联动。
    // 放在工具箱底部是 Photoshop 的经典布局，用户很熟悉。
    m_toolsToolBar->addSeparator();

    auto *colorIndicator = new QToolButton(this);
    colorIndicator->setObjectName(QStringLiteral("toolBoxColorIndicator"));
    colorIndicator->setFixedSize(28, 28);
    colorIndicator->setToolTip(tr("前景色（点击修改）"));
    connect(colorIndicator, &QToolButton::clicked,
            this, &MainWindow::chooseForegroundColor);
    m_toolsToolBar->addWidget(colorIndicator);

    // 初始颜色同步
    updateColorIndicators();
}

// ============================================================================
//  小工具：生成字母占位图标
// ============================================================================
//
//  Phase 7 会为每个工具提供 SVG 图标（resources/icons/*.svg）。
//  在那之前，用中文工具名的第一个字生成一个图标 ——
//  比一个空白的按钮好得多，用户至少能认出来是哪个工具。
QIcon MainWindow::makeLetterIcon(const QString &text) const
{
    const int size = 24;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 圆角背景。用 palette 的颜色，这样能跟随系统主题。
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Button));
    p.drawRoundedRect(QRect(1, 1, size - 2, size - 2), 4, 4);

    // 取第一个字符
    const QChar first = text.isEmpty() ? QChar('?') : text.at(0);
    QFont f = p.font();
    f.setPixelSize(14);
    f.setBold(true);
    p.setFont(f);
    p.setPen(palette().color(QPalette::ButtonText));
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QString(first));

    p.end();
    return QIcon(pm);
}

// ============================================================================
//  updateColorIndicators —— 同步所有颜色显示
// ============================================================================
//
//  颜色显示在三处：
//    · 工具箱底部的指示器
//    · 颜色面板的两个色块
//    · （Phase 2 之后）画笔工具的选项条
//
//  ★ 集中在一个函数里更新 ★
//    分散更新很容易漏掉一处 —— 表现为"改了颜色但工具箱里的色块没变"，
//    用户会以为颜色没改成功。
void MainWindow::updateColorIndicators()
{
    // 工具箱指示器
    if (auto *btn = findChild<QToolButton *>(QStringLiteral("toolBoxColorIndicator"))) {
        const QColor c = m_foregroundColor;
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { background-color: %1; border: 2px solid #666; border-radius: 3px; }")
                .arg(c.name(QColor::HexRgb)));
    }

    // 颜色面板的前景/背景色块
    if (auto *fg = findChild<QPushButton *>(QStringLiteral("foregroundSwatch"))) {
        fg->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; border: 2px solid #666; }")
                .arg(m_foregroundColor.name(QColor::HexRgb)));
    }
    if (auto *bg = findChild<QPushButton *>(QStringLiteral("backgroundSwatch"))) {
        bg->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; border: 2px solid #666; }")
                .arg(m_backgroundColor.name(QColor::HexRgb)));
    }
}

// ============================================================================
//  setupDocks
// ============================================================================
//
//  Phase 1 的面板：
//    · 历史记录（QUndoView）  —— 直接挂 QUndoStack，两行代码就有了
//    · 颜色（前景/背景色）
//    · 图层（Phase 5 完整实现，这里给一个最小可用版本）
//
//  ★ 每个面板都遵循约定 7 ★
//    通过 applyDocumentToPanels() 统一接收 setDocument。
//    Phase 1 里"颜色"面板其实不需要文档，但接口先留出来 ——
//    这样 Phase 5 实现完整图层面板时不需要改 MainWindow 的调用结构。
// ============================================================================

void MainWindow::setupDocks()
{
    setDockNestingEnabled(true);

    // ─────────────────────────────────────────────────────────────────────
    //  历史记录面板 —— 直接用 QUndoView
    // ─────────────────────────────────────────────────────────────────────
    //
    //  ★ 这是"用 Qt 原生组件"的典型收益 ★
    //    QUndoView 自动显示撤销栈的全部条目、自动高亮当前位置、
    //    点击任意条目自动跳转到那个状态（内部调 QUndoStack::setIndex）。
    //    自己写这套逻辑至少 300 行，而 QUndoView 只需要两行。
    m_historyDock = new QDockWidget(tr("历史记录"), this);
    m_historyDock->setObjectName(QStringLiteral("historyDock"));
    m_historyDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_undoView = new QUndoView(this);
    m_undoView->setEmptyLabel(tr("（已经是初始状态）"));
    // 不在这里 setStack —— applyDocumentToPanels 会在文档建立后绑定
    m_historyDock->setWidget(m_undoView);

    addDockWidget(Qt::RightDockWidgetArea, m_historyDock);

    // ─────────────────────────────────────────────────────────────────────
    //  颜色面板
    // ─────────────────────────────────────────────────────────────────────
    m_colorDock = new QDockWidget(tr("颜色"), this);
    m_colorDock->setObjectName(QStringLiteral("colorDock"));
    m_colorDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    {
        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(10);

        // ── 前景/背景色块 ──
        auto *swatchRow = new QHBoxLayout;
        swatchRow->setSpacing(8);

        // 用 QPushButton 承载色块：点击直接弹颜色对话框。
        // 比"色块 QLabel + 单独的按钮"少一次点击。
        auto *fgButton = new QPushButton(page);
        fgButton->setObjectName(QStringLiteral("foregroundSwatch"));
        fgButton->setFixedSize(48, 48);
        fgButton->setToolTip(tr("前景色（点击修改）"));
        fgButton->setCursor(Qt::PointingHandCursor);

        auto *bgButton = new QPushButton(page);
        bgButton->setObjectName(QStringLiteral("backgroundSwatch"));
        bgButton->setFixedSize(48, 48);
        bgButton->setToolTip(tr("背景色（点击修改）"));
        bgButton->setCursor(Qt::PointingHandCursor);

        swatchRow->addWidget(fgButton);
        swatchRow->addWidget(bgButton);

        // 交换 / 重置按钮（竖排放在色块右侧）
        auto *btnColumn = new QVBoxLayout;
        btnColumn->setSpacing(2);

        auto *swapBtn = new QToolButton(page);
        swapBtn->setText(QStringLiteral("⇄"));
        swapBtn->setToolTip(tr("交换前景色与背景色 (X)"));
        swapBtn->setAutoRaise(true);
        connect(swapBtn, &QToolButton::clicked, this, &MainWindow::swapColors);

        auto *resetBtn = new QToolButton(page);
        resetBtn->setText(QStringLiteral("◧"));
        resetBtn->setToolTip(tr("恢复默认颜色：黑 / 白 (D)"));
        resetBtn->setAutoRaise(true);
        connect(resetBtn, &QToolButton::clicked, this, &MainWindow::resetColors);

        btnColumn->addWidget(swapBtn);
        btnColumn->addWidget(resetBtn);
        btnColumn->addStretch(1);

        swatchRow->addLayout(btnColumn);
        swatchRow->addStretch(1);
        layout->addLayout(swatchRow);

        connect(fgButton, &QPushButton::clicked,
                this, &MainWindow::chooseForegroundColor);
        connect(bgButton, &QPushButton::clicked,
                this, &MainWindow::chooseBackgroundColor);

        // 分隔线
        auto *line = new QFrame(page);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        layout->addWidget(line);

        auto *hint = new QLabel(page);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
        hint->setText(tr("快捷键\n"
                         "  X    交换前景 / 背景色\n"
                         "  D    恢复默认颜色（黑 / 白）\n\n"
                         "吸管工具取到的颜色会进入前景色。"));
        layout->addWidget(hint);
        layout->addStretch(1);

        m_colorDock->setWidget(page);
    }

    addDockWidget(Qt::RightDockWidgetArea, m_colorDock);

    // ─────────────────────────────────────────────────────────────────────
    //  图层面板（阶段五）
    // ─────────────────────────────────────────────────────────────────────
    //
    //  真正的面板实现搬去了 src/ui/LayerPanel.{h,cpp}（自带缩略图、眼睛、
    //  锁、拖拽排序、多选、混合模式、不透明度）。这里只剩下"装配"——
    //  把它塞进 dock，并把它的信号接到 MainWindow 的槽上。
    //
    //  ★ 面板一个像素都不改 ★
    //    面板只发信号；所有改动都由下面这些槽包成 LayerCommand 压栈，
    //    撤销/重做因此只有一条路径，不可能漏。
    //    反过来，如果让面板自己去改 Document，就会出现
    //    "点了眼睛图标能隐藏、但 Ctrl+Z 没反应"这类静默漏网。
    m_layerDock = new QDockWidget(tr("图层"), this);
    m_layerDock->setObjectName(QStringLiteral("layerDock"));
    m_layerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_layerPanel = new LayerPanel(300, this);
    m_layerPanel->setObjectName(QStringLiteral("layerPanel"));
    m_layerDock->setWidget(m_layerPanel);

    // ── 面板信号 → MainWindow 槽 ──
    //
    //  ★ 一律用新式连接（成员函数指针），不用 SIGNAL/SLOT 宏 ★
    //    字符串式连接写错只会**运行时静默失败**：按钮点了没反应、
    //    不报错、不崩溃。新式连接下签名对不上直接编译报错。
    connect(m_layerPanel, &LayerPanel::addLayerRequested,
            this, &MainWindow::addNewLayer);
    connect(m_layerPanel, &LayerPanel::duplicateRequested,
            this, &MainWindow::duplicateLayersFromPanel);
    connect(m_layerPanel, &LayerPanel::deleteRequested,
            this, &MainWindow::deleteLayersFromPanel);
    connect(m_layerPanel, &LayerPanel::moveUpRequested,
            this, &MainWindow::moveLayerUp);
    connect(m_layerPanel, &LayerPanel::moveDownRequested,
            this, &MainWindow::moveLayerDown);
    connect(m_layerPanel, &LayerPanel::mergeDownRequested,
            this, &MainWindow::mergeLayerDownAt);
    connect(m_layerPanel, &LayerPanel::mergeVisibleRequested,
            this, &MainWindow::mergeVisibleLayers);
    connect(m_layerPanel, &LayerPanel::flattenRequested,
            this, &MainWindow::flattenImage);
    connect(m_layerPanel, &LayerPanel::reorderRequested,
            this, &MainWindow::reorderLayer);
    connect(m_layerPanel, &LayerPanel::visibilityToggled,
            this, &MainWindow::setLayerVisible);
    connect(m_layerPanel, &LayerPanel::lockToggled,
            this, &MainWindow::setLayerLocked);
    connect(m_layerPanel, &LayerPanel::renameRequested,
            this, &MainWindow::setLayerName);
    connect(m_layerPanel, &LayerPanel::blendModeChanged,
            this, &MainWindow::setLayerBlendMode);
    connect(m_layerPanel, &LayerPanel::opacityPreviewed,
            this, &MainWindow::previewLayerOpacity);
    connect(m_layerPanel, &LayerPanel::opacityCommitted,
            this, &MainWindow::commitLayerOpacity);

    // ★ 文档绑定不在这里做 ★
    //   它由 applyDocumentToPanels() 统一处理（见 MainWindow.cpp）。
    //   一旦在这里另外 connect 一次 currentChanged，就会有两条路径同时
    //   换文档 —— 多标签化时必定出错。

    addDockWidget(Qt::RightDockWidgetArea, m_layerDock);

    // ─────────────────────────────────────────────────────────────────────
    //  把面板的显隐动作填进"视图 → 面板"菜单
    // ─────────────────────────────────────────────────────────────────────
    if (QMenu *panelMenu = menuBar()->findChild<QMenu *>(QStringLiteral("menuPanels"))) {
        panelMenu->addAction(m_layerDock->toggleViewAction());
        panelMenu->addAction(m_historyDock->toggleViewAction());
        panelMenu->addAction(m_colorDock->toggleViewAction());
    }

    // ── 右侧三个面板竖排 ──
    //
    // ★ 不 split 的话 Qt 会把它们并排放在右侧，各占 1/3 宽度 ★
    //   图层列表需要宽度来显示图层名，而历史记录不需要那么宽。
    //   竖排（上下堆叠）是更合理的布局。
    splitDockWidget(m_layerDock, m_historyDock, Qt::Vertical);
    splitDockWidget(m_historyDock, m_colorDock, Qt::Vertical);

    // 面板的初始宽度（图层面板宽一些，历史与颜色窄一些）
    resizeDocks({ m_layerDock, m_historyDock, m_colorDock },
                { 240, 200, 180 }, Qt::Horizontal);
    // 初始高度（历史记录区块大一些，颜色面板只需要放两个色块）
    resizeDocks({ m_layerDock, m_historyDock, m_colorDock },
                { 300, 260, 150 }, Qt::Vertical);
}

// ============================================================================
//  setupStatusBar
// ============================================================================

void MainWindow::setupStatusBar()
{
    m_statusBar = new StatusBarWidget(this);

    // ★ 用 addWidget(widget, 1) 而不是 addPermanentWidget ★
    //   addPermanentWidget 会把控件放到最右侧，而 showMessage 显示在左侧。
    //   我们需要状态栏的内容**占满整条**，所以用 addWidget 并给 stretch=1。
    statusBar()->addWidget(m_statusBar, 1);

    // 状态栏顶部加一条细线，视觉上与画布区域分开
    statusBar()->setStyleSheet(QStringLiteral(
        "QStatusBar { border-top: 1px solid palette(mid); }"));

    connect(m_statusBar, &StatusBarWidget::zoomLevelRequested,
            this, &MainWindow::setZoomLevel);

    m_statusBar->showMessage(tr("就绪"), 3000);
}

// ============================================================================
//  captureHotkeyCandidates / applyCaptureHotkey
// ============================================================================

// 候选表（首选在前）。第一项必须就是 kCaptureHotkey —— 文档、菜单默认显示、
// 全部登记失败时的兜底，都以"产品定的那一个"为准。
//
// ★ 后面的选项怎么挑的 ★
//   全局热键是在**整个系统**里把这个键截走 —— 所以回落项也不能选
//   "别的程序常用的键"（例如 Ctrl+Shift+S 到处是"另存为"，
//   抢过来会让用户在别的软件里按不出那个功能）。
//   因此回落项一律往"多加一个 Shift"的方向走：可记忆、且极少有人占用。
QStringList MainWindow::captureHotkeyCandidates()
{
    return QStringList{
        QString::fromLatin1(kCaptureHotkey),   // Ctrl+Alt+S ← 必须留在第一位
        QStringLiteral("Ctrl+Alt+Shift+S"),
        QStringLiteral("Ctrl+Alt+Shift+A"),
    };
}

// 把"实际生效的键"写到「立即截图」动作上。
// ★ 只有这一个地方写这个动作的快捷键与提示文字 ★
//   两处各写一遍，就会出现"菜单上写着 A、按 B 才管用"。
void MainWindow::applyCaptureHotkey(const QKeySequence &sequence)
{
    if (!m_actionCaptureNow || sequence.isEmpty())
        return;

    m_actionCaptureNow->setShortcut(sequence);
    m_actionCaptureNow->setStatusTip(
        tr("抓取当前屏幕并开为新标签页（%1；程序最小化时也有效）")
            .arg(sequence.toString(QKeySequence::NativeText)));
}

// ============================================================================
//  setupScreenCapture —— 屏幕截图的全局热键与延迟定时器
// ============================================================================
//
//  ★ 内部顺序是定死的：先 connect，后 register ★
//    register 一旦成功，系统随时可能把 WM_HOTKEY 投进来 ——
//    也就是说"信号可能在下一行就发出"。连接还没建好就登记的话，那一次
//    按键落进真空：热键明明登记着，按下去却什么都不发生。
//    ★ 而且它只在"第一次按"时暴露 ★ 用户会以为是自己没按对，
//    再按一次 —— 第二次就好了（那时连接早就建好了），于是这个 bug
//    几乎不可能被报告出来。
//    这与铁律第九章"信号传不了现状"同源：凡是"连接建立之前就可能已经
//    发生的事"，顺序就必须钉死。
//
//  ★ 为什么放在 setupStatusBar() 之后 ★
//    登记失败要说出来（说给用户听的地方是状态栏）。而失败最常见的形态是
//    "这个组合键被别的程序先占了" —— 那正是"按了没反应"最容易被误认为
//    "程序坏了"的地方。这一句提示是这条功能**唯一**的失败出口。

void MainWindow::setupScreenCapture()
{
    // ── ① 延迟截图的定时器（单次）──
    //   只建一个：重复点"延迟 N 秒"= 重新计时（见 scheduleScreenCapture）
    m_captureTimer = new QTimer(this);
    m_captureTimer->setSingleShot(true);
    connect(m_captureTimer, &QTimer::timeout,
            this, &MainWindow::onCaptureTimerFired);

    // ── ② 全局热键：逐个试候选表，**第一个登记上的就是实际生效键** ──
    //   ★ 对象总会建出来 ★（哪怕候选表全部失败）——"注册失败"绝不能表现为
    //   "对象不存在"那种沉默，否则上层连"到底怎么了"都问不出来。
    m_hotkey = new GlobalHotkey(this);

    // ★ connect 必须在这之前 ★（理由见本节开头）
    connect(m_hotkey, &GlobalHotkey::activated, this, [this](int id) {
        if (id == kCaptureHotkeyId)
            captureScreenNow();
    });

    const QStringList candidates = captureHotkeyCandidates();
    QKeySequence effective;          // 实际登记上的那个键
    QStringList  tried;              // 试过哪些键、各自为什么不行

    for (const QString &text : candidates) {
        // ★ 同样用花括号 ★（"最令人烦恼的解析"，见 setupActions 的说明）
        const QKeySequence seq{text};
        if (m_hotkey->registerSequence(seq, kCaptureHotkeyId)) {
            effective = seq;
            break;
        }
        tried << tr("%1（%2）").arg(text, m_hotkey->lastError());
    }

    if (!effective.isEmpty()) {
        // ★ 把**真正生效**的键写回菜单 ★
        //   回落到第二个键时，菜单上若还写着首选键，用户拿到的就是一把错钥匙：
        //   他会一直按 Ctrl+Alt+S，然后认为这个功能是坏的。
        applyCaptureHotkey(effective);

        if (effective.toString(QKeySequence::PortableText)
            != QString::fromLatin1(kCaptureHotkey)) {
            // 回落不是错误，但**必须说出来**，否则用户按首选键毫无反应时
            // 只会怀疑程序。状态栏 + 日志两处都留。
            if (m_statusBar) {
                m_statusBar->showMessage(
                    tr("首选快捷键 %1 已被其它程序占用，本次改用 %2（程序最小化时也有效）")
                        .arg(QString::fromLatin1(kCaptureHotkey),
                             effective.toString(QKeySequence::NativeText)),
                    15000);
            }
            qWarning("首选全局快捷键 %s 被占用，已回落到 %s",
                     kCaptureHotkey,
                     qUtf8Printable(effective.toString(QKeySequence::NativeText)));
        }
        return;
    }

    // ── 候选表全试完都没成 ──
    //
    // ★ 失败也**照常可用**，只是要用户知道怎么用 ★
    //   动作上还挂着首选键的**应用内**快捷键（setupActions 设的，这里没有
    //   改写），所以"程序在前台"这条路径一点没受影响 —— 提示里必须把这一点
    //   说清，否则用户会以为整个截图功能都坏了。
    if (m_statusBar) {
        m_statusBar->showMessage(
            tr("全局快捷键不可用（%1）。程序最小化时按不了，"
               "但前台仍可用 %2，或走「文件 → 屏幕截图 → 立即截图」")
                .arg(tried.join(QStringLiteral("；")),
                     QString::fromLatin1(kCaptureHotkey)),
            20000);
    }
    // 日志里也留一条：自检与截图工具拿不到状态栏（窗口没显示）。
    qWarning("全局快捷键全部登记失败：%s",
             qUtf8Printable(tried.join(QStringLiteral("；"))));
}

// ============================================================================
//  setupConnections
// ============================================================================

void MainWindow::setupConnections()
{
    // ─────────────────────────────────────────────────────────────────────
    //  撤销栈 → 菜单项的可用状态与文本
    // ─────────────────────────────────────────────────────────────────────
    //
    //  ★ 这里**不再**有连接 ★
    //
    //    旧实现在这里监听 DocumentManager::currentChanged，每次换文档
    //    就断开旧栈、连上新栈，还得把"上一个连过的栈"记在一个动态属性里。
    //    那套东西在关掉一页的瞬间会踩到"已经析构但还没断开"的栈。
    //
    //    多标签之后改成了：**每个栈在它那一页创建时连一次**，
    //    回调里用 `stack != m_commandStack` 过滤掉非当前页的栈
    //    （见 MainWindow::wireCommandStack）。连接的寿命等于栈的寿命，
    //    于是"忘记断开"和"断到野指针"这两种失败模式都不存在了。
    //
    //    初值则由 activateTab() → syncUndoRedoActions() 显式推 ——
    //    信号只能传达变化，传达不了现状。

    // ─────────────────────────────────────────────────────────────────────
    //  标签页 ⇄ 当前文档
    // ─────────────────────────────────────────────────────────────────────
    //
    //  ★ 切页与文档切换的连接不在这里，而在 setupCanvas() 里 ★
    //    因为它们要在**任何一页被创建之前**就位：第一页是在构造函数
    //    末尾的 newDocument() 里开的，而 setupConnections() 比它早不了多少
    //    —— 但 setupCanvas() 更早且语义更贴（它就是建中央控件的那一步）。
    //    两个"随文档变化"的连接放在建控件的那一步，读起来是一条线。

    // ─────────────────────────────────────────────────────────────────────
    //  文档修改状态 → 标题
    // ─────────────────────────────────────────────────────────────────────
    //
    //  ★ 也不需要在这里连 ★
    //    每个文档的 modifiedChanged 由它那一页的撤销栈与标签页分别处理：
    //    · 窗口标题：CommandStack::cleanChanged → updateWindowTitle()
    //      （在 wireCommandStack 里，带"是不是当前页"的过滤）
    //    · 标签文字：同一个信号 → DocumentTabs::refreshTabTitle
    //
    //    旧代码在这里对"当前文档"连 modifiedChanged，再用 UniqueConnection
    //    防止重复 —— 那个写法能工作，但"当前文档换一个就得重连一次"
    //    这件事本身没有自检守着。

    // ─────────────────────────────────────────────────────────────────────
    //  剪贴板变化 → "粘贴"动作的可用状态
    // ─────────────────────────────────────────────────────────────────────
    //
    //  ★ 为什么要监听 ★
    //    不监听的话，"粘贴"永远可用，用户点了才发现剪贴板里没有可用内容
    //    （或者只有一个纯文本），得到一句"粘贴失败"的提示 ——
    //    比直接禁用差得多。
    //
    //  ★ 为什么在启动时也要更新一次 ★
    //    dataChanged 只在**变化**时发。程序启动时剪贴板里可能已经有
    //    一张图了（用户先截了图再打开本程序），但那时不会收到信号。
    //    所以初始化时手动调一次。
    connect(QApplication::clipboard(), &QClipboard::dataChanged,
            this, &MainWindow::updatePasteActionState);

    updatePasteActionState();
}

// ============================================================================
//  updatePasteActionState —— 根据剪贴板内容决定"粘贴"是否可用
// ============================================================================

void MainWindow::updatePasteActionState()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();

    bool usable = false;
    if (mime) {
        // 优先看自定义格式，其次看标准图像格式。
        // ★ 两种都要检查 ★
        //   只看自定义格式的话，从浏览器复制的图片会被误判为"不可粘贴"；
        //   只看 image 的话，本程序复制的"带选区蒙版的数据"会走
        //   降级路径（丢失蒙版精度）。
        usable = mime->hasFormat(QLatin1String(kClipboardMimeLayer))
                 || mime->hasImage();
    }

    if (m_actionPaste)
        m_actionPaste->setEnabled(usable);
    if (m_actionPasteInto)
        m_actionPasteInto->setEnabled(usable);
}

// ============================================================================
//  最近文件列表
// ============================================================================

QStringList MainWindow::recentFiles() const
{
    QSettings settings;
    return settings.value(QStringLiteral("files/recent")).toStringList();
}

void MainWindow::addRecentFile(const QString &path)
{
    if (path.isEmpty())
        return;

    // ★ 用 canonicalPath 归一化 ★
    //   用户可能通过不同的路径形式打开同一个文件
    //   （相对/绝对、".."、符号链接、大小写差异）。
    //   不归一化的话，列表里会出现同一个文件的多个条目。
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString key = canonical.isEmpty() ? path : canonical;

    QStringList list = recentFiles();

    // 已存在则先移除（保证它被移到最前面）
    list.removeAll(key);
    list.prepend(key);

    // 截断到上限
    while (list.size() > kMaxRecentFiles)
        list.removeLast();

    QSettings settings;
    settings.setValue(QStringLiteral("files/recent"), list);

    updateRecentFileActions();
}

void MainWindow::updateRecentFileActions()
{
    const QStringList list = recentFiles();

    for (int i = 0; i < m_recentFileActions.size(); ++i) {
        QAction *action = m_recentFileActions.at(i);

        if (i >= list.size()) {
            action->setVisible(false);
            action->setData(QString());
            continue;
        }

        const QString path = list.at(i);
        const QFileInfo fi(path);

        // 显示"序号 文件名"，把完整路径放在 tooltip 里 ——
        // 菜单项太长会撑宽整个菜单，而路径通常很长。
        // 用 &数字 做加速键（Alt+1 打开第 1 个）。
        const int number = i + 1;
        action->setText(QStringLiteral("&%1  %2").arg(number).arg(fi.fileName()));
        action->setToolTip(path);
        action->setData(path);
        action->setVisible(true);

        // ★ 文件已不存在时禁用 ★
        //   比让用户点了才发现"文件找不到，要不要从列表移除"要好。
        action->setEnabled(fi.exists());
    }
}

// ============================================================================
//  设置持久化
// ============================================================================

void MainWindow::loadSettings()
{
    QSettings settings;

    // ── 窗口几何 ──
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    // ── 停靠面板的位置与尺寸 ──
    const QByteArray dockState = settings.value(QStringLiteral("window/dockState")).toByteArray();
    if (!dockState.isEmpty())
        restoreState(dockState);

    // ── 视图选项 ──
    const bool showGrid = settings.value(QStringLiteral("view/showGrid"), false).toBool();
    m_actionShowGrid->setChecked(showGrid);   // toggled 会调用 toggleGrid

    // ── 网格的间距与吸附 ──
    //
    //  ★ 间距读回来要先夹到合法范围 ★
    //    存档可能是旧版本写的、或被人手改过。越界值会被 CanvasView
    //    qBound 掉（4..2000），但**菜单的勾选态**是按原值匹配的 ——
    //    于是用户看到"菜单里一个都没勾上"，像是设置丢了。
    //    先夹一次，让"画布的值"与"菜单的勾选"永远一致。
    const int gridSpacing =
        qBound(4, settings.value(QStringLiteral("view/gridSpacing"), 64).toInt(), 2000);
    m_gridSpacing = gridSpacing;
    for (int i = 0; i < m_actionGridSpacing.size(); ++i)
        m_actionGridSpacing.at(i)->setChecked(
            m_actionGridSpacing.at(i)->data().toInt() == gridSpacing);
    if (m_canvas)
        m_canvas->setGridSize(gridSpacing);

    // 线色：存的是 QColor（QVariant 原生支持），非法/缺失则保留默认值
    const QVariant gridColorVar = settings.value(QStringLiteral("view/gridColor"));
    if (gridColorVar.isValid() && gridColorVar.value<QColor>().isValid())
        m_gridColor = gridColorVar.value<QColor>();
    if (m_canvas)
        m_canvas->setGridColor(m_gridColor);

    const bool snapToGrid = settings.value(QStringLiteral("view/snapToGrid"), false).toBool();
    m_actionSnapToGrid->setChecked(snapToGrid);   // toggled 会调用 toggleSnapToGrid

    const bool showCheckerboard =
        settings.value(QStringLiteral("view/showCheckerboard"), true).toBool();
    m_actionShowCheckerboard->setChecked(showCheckerboard);

    // ── 视图辅助：标尺与参考线 ──
    //   与网格/棋盘格同类：都是"怎么看"的应用级偏好，值得记住。
    //   setChecked 会触发 toggled → 走到对应的 toggle 槽，不需要再手动调一次。
    const bool showRuler =
        settings.value(QStringLiteral("view/showRuler"), true).toBool();
    m_actionShowRuler->setChecked(showRuler);

    const bool showGuides =
        settings.value(QStringLiteral("view/showGuides"), true).toBool();
    m_actionShowGuides->setChecked(showGuides);

    const bool lockGuides =
        settings.value(QStringLiteral("view/lockGuides"), false).toBool();
    m_actionLockGuides->setChecked(lockGuides);

    //   单位存的是**下标**（与 rulermetrics::unitAt 的协议一致）。
    //   ★ 读回来要先夹到合法范围 ★ 存档可能是旧版本写的、或被人手改过；
    //     越界下标会让 5 个单选项**一个都不选中**（菜单上一片空白），
    //     而 setRulerUnit() 又会静默拒绝 —— 表现是"单位菜单坏了"。
    {
        const int unitIdx =
            settings.value(QStringLiteral("view/rulerUnit"), 0).toInt();
        const int safeIdx =
            (unitIdx >= 0 && unitIdx < rulermetrics::kUnitCount) ? unitIdx : 0;

        for (int i = 0; i < m_actionRulerUnit.size(); ++i)
            m_actionRulerUnit.at(i)->setChecked(i == safeIdx);
        setRulerUnit(safeIdx);
    }

    // ── 颜色 ──
    //   ★ 不持久化前景/背景色 ★
    //    理由：每次启动都恢复上次的颜色，会让用户困惑
    //    （"为什么画笔是红的？我明明没设过"）。
    //    大部分程序的做法是每次启动回到默认的黑/白。

    // ── 最近文件 ──
    updateRecentFileActions();

    // ── 工具参数 ──
    //   ★ 由 ToolManager::loadAllOptions() 负责 ★
    //    它在 setupTools() 里已经被调用过了。
    //    这里不重复调用 —— 重复调用会触发两次 optionChanged，
    //    导致工具重建两次内部缓存（纯浪费）。
    //
    //    注意顺序：loadAllOptions 必须在 setDocument 之前。
    //    见 MainWindow 构造函数里的初始化顺序注释。
}

void MainWindow::saveSettings()
{
    QSettings settings;

    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/dockState"), saveState());

    settings.setValue(QStringLiteral("view/showGrid"), m_actionShowGrid->isChecked());
    settings.setValue(QStringLiteral("view/showCheckerboard"),
                      m_actionShowCheckerboard->isChecked());

    // ── 网格的间距与吸附 ──
    //   间距从**画布读**（它才是真正被夹到 4..2000 之后的值），
    //   而不是从菜单勾选项推断 —— 自定义值不在预设里，菜单是无勾选态的，
    //   那样存下去会永远写回 64。
    settings.setValue(QStringLiteral("view/snapToGrid"),
                      m_actionSnapToGrid->isChecked());
    //   间距与线色从 MainWindow 的**单一来源**写（而不是读画布）——
    //   退出路径上画布可能已经析构，读它拿不到值。
    settings.setValue(QStringLiteral("view/gridSpacing"), m_gridSpacing);
    settings.setValue(QStringLiteral("view/gridColor"), m_gridColor);

    // ── 视图辅助：标尺与参考线 ──
    settings.setValue(QStringLiteral("view/showRuler"), m_actionShowRuler->isChecked());
    settings.setValue(QStringLiteral("view/showGuides"), m_actionShowGuides->isChecked());
    settings.setValue(QStringLiteral("view/lockGuides"), m_actionLockGuides->isChecked());

    //   单位存**下标**（协议见 rulermetrics::unitAt）。
    //   从"当前选中项"反查下标，而不是缓存一个 int 成员 ——
    //   缓存就有"两处真相"，用户从别处改了单位（比如以后加了快捷键）时不会同步。
    {
        int unitIdx = 0;
        for (int i = 0; i < m_actionRulerUnit.size(); ++i) {
            if (m_actionRulerUnit.at(i)->isChecked()) {
                unitIdx = i;
                break;
            }
        }
        settings.setValue(QStringLiteral("view/rulerUnit"), unitIdx);
    }

    // ★ 工具参数 ★
    //   必须在 MainWindow 析构**之前**保存 ——
    //   ToolManager 是 MainWindow 的子对象，析构后工具与它们的
    //   ToolOptions 就都消失了。所以这个调用放在 MainWindow::~MainWindow
    //   的**第一行**（见 MainWindow.cpp）。
    if (m_toolManager)
        m_toolManager->saveAllOptions();

    // ★ 显式 sync ★
    //   QSettings 的析构会写盘，但在程序退出路径上，
    //   如果退出流程里还有别的 QSettings 实例存活，或者进程被强杀，
    //   不 sync 就可能有数据丢失。代价是几十微秒，值得。
    settings.sync();
}

} // namespace zl
