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
#include "compat/ScreenCapture.h"
#include "ui/CanvasView.h"
#include "ui/ToolOptionsBar.h"
#include "ui/StatusBarWidget.h"
#include "ui/LayerPanel.h"
#include "ui/NavigatorPanel.h"

#include "core/DocumentManager.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "core/ClipboardData.h"
// ★ 界面语言 ★ 语言菜单的可用项 / 当前项 / 写设置都走它
//   （"当前是什么语言"这件事只有 core/UiLanguage.cpp 一个出处）。
#include "core/UiLanguage.h"
// ★ 界面主题 ★ 主题菜单的三项、当前项、写设置都走它
//   （"用哪个主题"只有 core/UiTheme.cpp 一个出处；菜单只管转调）。
#include "core/UiTheme.h"

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

    m_actionNew = new QAction(tr("New(&N)..."), this);
    m_actionNew->setObjectName(QStringLiteral("actionNew"));
    setActionIcon(m_actionNew, QStringLiteral("file_new"));
    m_actionNew->setShortcut(QKeySequence::New);
    m_actionNew->setStatusTip(tr("Create a new blank image"));
    connect(m_actionNew, &QAction::triggered, this, &MainWindow::newDocument);

    m_actionOpen = new QAction(tr("Open(&O)..."), this);
    m_actionOpen->setObjectName(QStringLiteral("actionOpen"));
    setActionIcon(m_actionOpen, QStringLiteral("file_open"));
    m_actionOpen->setShortcut(QKeySequence::Open);
    m_actionOpen->setStatusTip(tr("Open an existing image file"));
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
    m_actionOpenAsLayer = new QAction(tr("Open as Layer(&L)..."), this);
    m_actionOpenAsLayer->setObjectName(QStringLiteral("actionOpenAsLayer"));
    setActionIcon(m_actionOpenAsLayer, QStringLiteral("file_open_as_layer"));
    m_actionOpenAsLayer->setStatusTip(
        tr("Add another image as a new layer in the current document (without switching documents)"));
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

    m_actionCaptureNow = new QAction(tr("Capture Now"), this);
    m_actionCaptureNow->setObjectName(QStringLiteral("actionCaptureNow"));
    setActionIcon(m_actionCaptureNow, QStringLiteral("action_screen_capture"));
    applyCaptureHotkey(captureSeq);
    connect(m_actionCaptureNow, &QAction::triggered, this,
            [this] { captureScreenNow(); });

    // 延迟三项：同一件事的三个档。
    // ★ 菜单上写"秒"、代码里给"毫秒" —— 换算只发生在这三个 lambda 里 ★
    //   散出去就会有人把 3000 写成 3。
    m_actionCaptureDelay3 = new QAction(tr("Delay 3 Seconds"), this);
    m_actionCaptureDelay3->setObjectName(QStringLiteral("actionCaptureDelay3"));
    setActionIcon(m_actionCaptureDelay3, QStringLiteral("action_capture_delay"));
    m_actionCaptureDelay3->setStatusTip(
        tr("Capture after 3 seconds: this window will minimize first, switch to the target screen"));
    connect(m_actionCaptureDelay3, &QAction::triggered, this,
            [this] { scheduleScreenCapture(3000); });

    m_actionCaptureDelay5 = new QAction(tr("Delay 5 Seconds"), this);
    m_actionCaptureDelay5->setObjectName(QStringLiteral("actionCaptureDelay5"));
    setActionIcon(m_actionCaptureDelay5, QStringLiteral("action_capture_delay"));
    m_actionCaptureDelay5->setStatusTip(
        tr("Capture after 5 seconds: this window will minimize first, switch to the target screen"));
    connect(m_actionCaptureDelay5, &QAction::triggered, this,
            [this] { scheduleScreenCapture(5000); });

    m_actionCaptureDelay10 = new QAction(tr("Delay 10 Seconds"), this);
    m_actionCaptureDelay10->setObjectName(QStringLiteral("actionCaptureDelay10"));
    setActionIcon(m_actionCaptureDelay10, QStringLiteral("action_capture_delay"));
    m_actionCaptureDelay10->setStatusTip(
        tr("Capture after 10 seconds: this window will minimize first, switch to the target screen"));
    connect(m_actionCaptureDelay10, &QAction::triggered, this,
            [this] { scheduleScreenCapture(10000); });

    m_actionSave = new QAction(tr("Save(&S)"), this);
    m_actionSave->setObjectName(QStringLiteral("actionSave"));
    setActionIcon(m_actionSave, QStringLiteral("file_save"));
    m_actionSave->setShortcut(QKeySequence::Save);
    m_actionSave->setStatusTip(tr("Save to the current file (asks for a path if untitled)"));
    connect(m_actionSave, &QAction::triggered, this, &MainWindow::saveDocument);

    m_actionSaveAs = new QAction(tr("Save As(&A)..."), this);
    m_actionSaveAs->setObjectName(QStringLiteral("actionSaveAs"));
    setActionIcon(m_actionSaveAs, QStringLiteral("file_save_as"));
    m_actionSaveAs->setShortcut(QKeySequence::SaveAs);
    m_actionSaveAs->setStatusTip(tr("Save to a new file; the format is decided by the extension"));
    connect(m_actionSaveAs, &QAction::triggered, this, &MainWindow::saveDocumentAs);

    m_actionPrint = new QAction(tr("Print(&P)..."), this);
    m_actionPrint->setObjectName(QStringLiteral("actionPrint"));
    setActionIcon(m_actionPrint, QStringLiteral("file_print"));
    m_actionPrint->setShortcut(QKeySequence::Print);
    m_actionPrint->setStatusTip(tr("Print the current image"));
    connect(m_actionPrint, &QAction::triggered, this, &MainWindow::printDocument);

    // 「导出 PDF」不挂快捷键：Ctrl+Shift+P 之类容易与打印混，
    // 而它本身是低频动作，菜单里点一次就够。
    m_actionExportPdf = new QAction(tr("Export PDF(&F)..."), this);
    m_actionExportPdf->setObjectName(QStringLiteral("actionExportPdf"));
    setActionIcon(m_actionExportPdf, QStringLiteral("file_export_pdf"));
    m_actionExportPdf->setStatusTip(tr("Export the current image as a single-page PDF"));
    connect(m_actionExportPdf, &QAction::triggered, this, &MainWindow::saveAsPdf);

    m_actionClose = new QAction(tr("Close(&W)"), this);
    m_actionClose->setObjectName(QStringLiteral("actionClose"));
    setActionIcon(m_actionClose, QStringLiteral("file_close"));
    m_actionClose->setShortcut(QKeySequence::Close);
    m_actionClose->setStatusTip(tr("Close the current image"));
    connect(m_actionClose, &QAction::triggered, this, &MainWindow::closeCurrentDocument);

    m_actionQuit = new QAction(tr("Quit(&Q)"), this);
    m_actionQuit->setObjectName(QStringLiteral("actionQuit"));
    setActionIcon(m_actionQuit, QStringLiteral("file_quit"));
    m_actionQuit->setShortcut(QKeySequence::Quit);
    m_actionQuit->setMenuRole(QAction::QuitRole);
    m_actionQuit->setStatusTip(tr("Quit %1").arg(zl::appinfo::displayName()));
    connect(m_actionQuit, &QAction::triggered, this, &QWidget::close);

    // ── 「主题」的 3 个互斥项 ──
    //
    //  ★ 为什么自己建动作、不放进 m_actionTheme 之外的地方 ★
    //    与网格间距、标尺单位同套路：单选项不留 objectName（否则对账规则
    //    "有名字的菜单动作必须有图标"会牵连出 3 张纯噪音的图标）。
    //    定位它们靠子菜单本身 + 下标。
    //
    //  ★ 勾选态读"偏好"而不是"当前 palette 深浅" ★
    //    自动主题碰上深色系统时 palette 也是深的 —— 若按深浅勾，用户会看到
    //    "自动"没勾、"深色"勾着，以为程序擅自改了选择。
    //    与语言菜单读偏好（而不是 currentLanguage）是同一条道理。
    {
        auto *themeGroup = new QActionGroup(this);
        themeGroup->setExclusive(true);

        const uitheme::Mode pref = uitheme::preference();
        for (const uitheme::Mode mode : uitheme::allModes()) {
            QAction *act = new QAction(uitheme::displayName(mode), this);
            act->setCheckable(true);
            act->setChecked(mode == pref);
            // 存的是 Mode 的整数值：退出/外部改设置后要靠它同步勾选态。
            // 存值而不是比对文本 —— 文本是翻译过的，换语言就失配。
            act->setData(int(mode));
            connect(act, &QAction::triggered, this,
                    [this, mode] { setTheme(mode); });
            themeGroup->addAction(act);
            m_actionTheme.append(act);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  编辑
    // ─────────────────────────────────────────────────────────────────────

    m_actionUndo = new QAction(tr("Undo(&U)"), this);
    m_actionUndo->setObjectName(QStringLiteral("actionUndo"));
    setActionIcon(m_actionUndo, QStringLiteral("edit_undo"));
    m_actionUndo->setShortcut(QKeySequence::Undo);
    m_actionUndo->setStatusTip(tr("Undo the last operation"));
    m_actionUndo->setEnabled(false);
    connect(m_actionUndo, &QAction::triggered, this, &MainWindow::undo);

    m_actionRedo = new QAction(tr("Redo(&R)"), this);
    m_actionRedo->setObjectName(QStringLiteral("actionRedo"));
    setActionIcon(m_actionRedo, QStringLiteral("edit_redo"));
    // ★ 重做给两个快捷键 ★
    //   QKeySequence::Redo 在 Windows 上解析为 Ctrl+Y。
    //   但很多用户习惯 Ctrl+Shift+Z（Linux/Mac 的约定），
    //   从其他平台迁移过来的用户会反复按它。
    //   Qt 支持一个 QAction 有多个快捷键，两个都给最省事。
    m_actionRedo->setShortcuts({ QKeySequence::Redo,
                                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z) });
    m_actionRedo->setStatusTip(tr("Redo the undone operation"));
    m_actionRedo->setEnabled(false);
    connect(m_actionRedo, &QAction::triggered, this, &MainWindow::redo);

    m_actionCut = new QAction(tr("Cut(&T)"), this);
    m_actionCut->setObjectName(QStringLiteral("actionCut"));
    setActionIcon(m_actionCut, QStringLiteral("edit_cut"));
    m_actionCut->setShortcut(QKeySequence::Cut);
    m_actionCut->setStatusTip(tr("Cut the selection to the clipboard"));
    m_actionCut->setEnabled(false);
    connect(m_actionCut, &QAction::triggered, this, &MainWindow::cutSelection);

    m_actionCopy = new QAction(tr("Copy(&C)"), this);
    m_actionCopy->setObjectName(QStringLiteral("actionCopy"));
    setActionIcon(m_actionCopy, QStringLiteral("edit_copy"));
    m_actionCopy->setShortcut(QKeySequence::Copy);
    m_actionCopy->setStatusTip(tr("Copy the selection to the clipboard (copies the whole layer when there is no selection)"));
    m_actionCopy->setEnabled(false);
    connect(m_actionCopy, &QAction::triggered, this, &MainWindow::copySelection);

    m_actionPaste = new QAction(tr("Paste as New Layer(&P)"), this);
    m_actionPaste->setObjectName(QStringLiteral("actionPaste"));
    setActionIcon(m_actionPaste, QStringLiteral("edit_paste"));
    m_actionPaste->setShortcut(QKeySequence::Paste);
    m_actionPaste->setStatusTip(tr("Paste the clipboard content as a new layer"));
    connect(m_actionPaste, &QAction::triggered, this, &MainWindow::pasteAsNewLayer);

    m_actionPasteInto = new QAction(tr("Paste into Current Layer(&I)"), this);
    m_actionPasteInto->setObjectName(QStringLiteral("actionPasteInto"));
    setActionIcon(m_actionPasteInto, QStringLiteral("edit_paste_into"));
    m_actionPasteInto->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    m_actionPasteInto->setStatusTip(tr("Merge the clipboard content into the current layer"));
    connect(m_actionPasteInto, &QAction::triggered, this, &MainWindow::pasteIntoLayer);

    m_actionDelete = new QAction(tr("Delete(&D)"), this);
    m_actionDelete->setObjectName(QStringLiteral("actionDelete"));
    setActionIcon(m_actionDelete, QStringLiteral("edit_delete"));
    m_actionDelete->setShortcut(QKeySequence::Delete);
    m_actionDelete->setStatusTip(tr("Delete the content inside the selection (clears the whole layer when there is no selection)"));
    connect(m_actionDelete, &QAction::triggered, this, &MainWindow::onDeleteSelection);

    // 撤销步数：每页撤销栈保留多少条历史。
    // 不挂快捷键 —— 它是设置项，不是操作。
    m_actionUndoLimit = new QAction(tr("Undo Steps..."), this);
    m_actionUndoLimit->setObjectName(QStringLiteral("actionUndoLimit"));
    setActionIcon(m_actionUndoLimit, QStringLiteral("edit_undo_limit"));
    m_actionUndoLimit->setStatusTip(tr("How many undo steps each document keeps"));
    connect(m_actionUndoLimit, &QAction::triggered, this, &MainWindow::openUndoLimitDialog);

    // ─────────────────────────────────────────────────────────────────────
    //  选择
    // ─────────────────────────────────────────────────────────────────────

    m_actionSelectAll = new QAction(tr("Select All(&A)"), this);
    m_actionSelectAll->setObjectName(QStringLiteral("actionSelectAll"));
    setActionIcon(m_actionSelectAll, QStringLiteral("select_all"));
    m_actionSelectAll->setShortcut(QKeySequence::SelectAll);
    m_actionSelectAll->setStatusTip(tr("Select the whole canvas"));
    connect(m_actionSelectAll, &QAction::triggered, this, &MainWindow::selectAll);

    m_actionDeselect = new QAction(tr("Deselect(&D)"), this);
    m_actionDeselect->setObjectName(QStringLiteral("actionDeselect"));
    setActionIcon(m_actionDeselect, QStringLiteral("select_none"));
    // ★ 为什么不用 QKeySequence::Deselect ★
    //   Qt 没有提供这个标准键（SelectAll 有，Deselect 没有）。
    //   Ctrl+D 是 Photoshop 的约定，用户肌肉记忆很强，直接用。
    m_actionDeselect->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    m_actionDeselect->setStatusTip(tr("Cancel the current selection"));
    connect(m_actionDeselect, &QAction::triggered, this, &MainWindow::deselect);

    m_actionInvertSelection = new QAction(tr("Invert Selection(&I)"), this);
    m_actionInvertSelection->setObjectName(QStringLiteral("actionInvertSelection"));
    setActionIcon(m_actionInvertSelection, QStringLiteral("select_invert"));
    m_actionInvertSelection->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    m_actionInvertSelection->setStatusTip(tr("Select everything outside the current selection"));
    connect(m_actionInvertSelection, &QAction::triggered, this, &MainWindow::invertSelection);

    // ─────────────────────────────────────────────────────────────────────
    //  调整（Phase 1 只接不需要参数对话框的那些）
    // ─────────────────────────────────────────────────────────────────────

    m_actionGrayscale = new QAction(tr("Desaturate(&G)"), this);
    m_actionGrayscale->setObjectName(QStringLiteral("actionGrayscale"));
    setActionIcon(m_actionGrayscale, QStringLiteral("adjust_grayscale"));
    // Ctrl+Shift+U 是 Photoshop 的约定
    m_actionGrayscale->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U));
    m_actionGrayscale->setStatusTip(tr("Convert to grayscale by luminance weights (0.299R + 0.587G + 0.114B)"));
    connect(m_actionGrayscale, &QAction::triggered, this, &MainWindow::applyGrayscale);

    m_actionInvert = new QAction(tr("Invert(&I)"), this);
    m_actionInvert->setObjectName(QStringLiteral("actionInvert"));
    setActionIcon(m_actionInvert, QStringLiteral("adjust_invert"));
    m_actionInvert->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    m_actionInvert->setStatusTip(tr("Invert all color channels (like a negative)"));
    connect(m_actionInvert, &QAction::triggered, this, &MainWindow::applyInvert);

    // ── 需要参数对话框的调整（Phase 4）──
    //
    // 快捷键沿用 Photoshop 的约定：色相/饱和度 Ctrl+U、曲线 Ctrl+M。
    // 亮度/对比度在 PS 里没有快捷键，这里也不设 —— 与其硬塞一个
    // 容易和别的功能冲突的组合键，不如保持"无快捷键"。
    m_actionBrightnessContrast = new QAction(tr("Brightness/Contrast(&B)..."), this);
    m_actionBrightnessContrast->setObjectName(QStringLiteral("actionBrightnessContrast"));
    setActionIcon(m_actionBrightnessContrast, QStringLiteral("adjust_brightness"));
    m_actionBrightnessContrast->setStatusTip(tr("Adjust the brightness and contrast of the image"));
    connect(m_actionBrightnessContrast, &QAction::triggered,
            this, &MainWindow::openBrightnessContrast);

    m_actionHueSaturation = new QAction(tr("Hue/Saturation(&H)..."), this);
    m_actionHueSaturation->setObjectName(QStringLiteral("actionHueSaturation"));
    setActionIcon(m_actionHueSaturation, QStringLiteral("adjust_hue"));
    m_actionHueSaturation->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    m_actionHueSaturation->setStatusTip(tr("Adjust the hue, saturation and lightness of colors"));
    connect(m_actionHueSaturation, &QAction::triggered, this, &MainWindow::openHueSaturation);

    m_actionCurves = new QAction(tr("Curves(&C)..."), this);
    m_actionCurves->setObjectName(QStringLiteral("actionCurves"));
    setActionIcon(m_actionCurves, QStringLiteral("adjust_curves"));
    m_actionCurves->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    m_actionCurves->setStatusTip(tr("Finely adjust the per-channel tonal mapping with a control-point curve"));
    connect(m_actionCurves, &QAction::triggered, this, &MainWindow::openCurves);

    m_actionLevels = new QAction(tr("Levels(&L)..."), this);
    m_actionLevels->setObjectName(QStringLiteral("actionLevels"));
    setActionIcon(m_actionLevels, QStringLiteral("adjust_levels"));
    // Ctrl+L 也是 Photoshop 的约定，且当前未被占用（已核对全部快捷键）
    m_actionLevels->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_actionLevels->setStatusTip(tr("Adjust the tonal distribution with the histogram black/white points and midtones"));
    connect(m_actionLevels, &QAction::triggered, this, &MainWindow::openLevels);

    // ─────────────────────────────────────────────────────────────────────
    //  图像尺寸（图像大小 / 画布大小）
    // ─────────────────────────────────────────────────────────────────────

    // ★ 快捷键对齐 Photoshop ★
    //   Ctrl+Alt+I —— Image Size
    //   Ctrl+Alt+C —— Canvas Size
    //   这两个组合在 PS 用户手里是肌肉记忆，换掉会持续制造误操作。
    //   （已核对本工程其余快捷键，无冲突。）
    m_actionImageSize = new QAction(tr("Image Size(&I)..."), this);
    m_actionImageSize->setObjectName(QStringLiteral("actionImageSize"));
    setActionIcon(m_actionImageSize, QStringLiteral("image_size"));
    m_actionImageSize->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_I));
    m_actionImageSize->setStatusTip(tr("Resample the pixel content (the image becomes softer or sharper)"));
    connect(m_actionImageSize, &QAction::triggered, this, &MainWindow::openImageSize);

    m_actionCanvasSize = new QAction(tr("Canvas Size(&C)..."), this);
    m_actionCanvasSize->setObjectName(QStringLiteral("actionCanvasSize"));
    setActionIcon(m_actionCanvasSize, QStringLiteral("canvas_size"));
    m_actionCanvasSize->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    m_actionCanvasSize->setStatusTip(tr("Change the canvas extent: no resampling; crops when smaller, pads with the background color when larger"));
    connect(m_actionCanvasSize, &QAction::triggered, this, &MainWindow::openCanvasSize);

    // ─────────────────────────────────────────────────────────────────────
    //  图像旋转 / 翻转
    // ─────────────────────────────────────────────────────────────────────

    m_actionRotate90CW = new QAction(tr("Rotate 90° Clockwise"), this);
    m_actionRotate90CW->setObjectName(QStringLiteral("actionRotate90CW"));
    setActionIcon(m_actionRotate90CW, QStringLiteral("rotate_cw"));
    m_actionRotate90CW->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(m_actionRotate90CW, &QAction::triggered, this, &MainWindow::rotate90CW);

    m_actionRotate90CCW = new QAction(tr("Rotate 90° Counterclockwise"), this);
    m_actionRotate90CCW->setObjectName(QStringLiteral("actionRotate90CCW"));
    setActionIcon(m_actionRotate90CCW, QStringLiteral("rotate_ccw"));
    m_actionRotate90CCW->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(m_actionRotate90CCW, &QAction::triggered, this, &MainWindow::rotate90CCW);

    m_actionRotate180 = new QAction(tr("Rotate 180°"), this);
    m_actionRotate180->setObjectName(QStringLiteral("actionRotate180"));
    setActionIcon(m_actionRotate180, QStringLiteral("rotate_180"));
    connect(m_actionRotate180, &QAction::triggered, this, &MainWindow::rotate180);

    // ★ 这一块曾经"漏写"过一次 ★
    //   菜单里 addAction(m_actionRotateArbitrary) 在，而 new QAction 不在 ——
    //   结果 imageMenu->addAction(nullptr) 静默什么都不做，菜单项凭空消失。
    //   编译器不报错（nullptr 是合法实参）、离线自检也测不到（自检不建菜单），
    //   只有真正跑起来看菜单才暴露。改动作定义时务必"new + objectName + connect +
    //   addAction"四处齐全。
    m_actionRotateArbitrary = new QAction(tr("Rotate Arbitrary(&A)..."), this);
    m_actionRotateArbitrary->setObjectName(QStringLiteral("actionRotateArbitrary"));
    setActionIcon(m_actionRotateArbitrary, QStringLiteral("rotate_arbitrary"));
    connect(m_actionRotateArbitrary, &QAction::triggered,
            this, &MainWindow::openRotateArbitrary);

    m_actionFlipH = new QAction(tr("Flip Horizontal"), this);
    m_actionFlipH->setObjectName(QStringLiteral("actionFlipH"));
    setActionIcon(m_actionFlipH, QStringLiteral("flip_h"));
    m_actionFlipH->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
    connect(m_actionFlipH, &QAction::triggered, this, &MainWindow::flipHorizontal);

    m_actionFlipV = new QAction(tr("Flip Vertical"), this);
    m_actionFlipV->setObjectName(QStringLiteral("actionFlipV"));
    setActionIcon(m_actionFlipV, QStringLiteral("flip_v"));
    connect(m_actionFlipV, &QAction::triggered, this, &MainWindow::flipVertical);

    // ── 裁剪（裁到当前选区的包围盒）──
    //
    // ★ 与工具箱里的裁剪工具是两条不同的路 ★
    //   菜单项一次点击就完成，操作数取**当前选区的包围盒** —— 用户已经用
    //   选框表达过"我要这一块"，再让他拿裁剪工具重框一遍是重复劳动。
    //   没有选区时置灰（不用内容包围盒兜底，那是「裁切到内容」的语义）。
    //   完整理由见 docs/画布与文件操作设计.md 6.8。
    m_actionCrop = new QAction(tr("Crop"), this);
    m_actionCrop->setObjectName(QStringLiteral("actionCrop"));
    setActionIcon(m_actionCrop, QStringLiteral("tool_crop"));
    m_actionCrop->setStatusTip(tr("Crop the canvas to the bounding box of the current selection"));
    connect(m_actionCrop, &QAction::triggered, this, &MainWindow::cropToSelection);

    // ─────────────────────────────────────────────────────────────────────
    //  图层
    // ─────────────────────────────────────────────────────────────────────

    m_actionLayerNew = new QAction(tr("New Layer(&N)"), this);
    m_actionLayerNew->setObjectName(QStringLiteral("actionLayerNew"));
    setActionIcon(m_actionLayerNew, QStringLiteral("layer_new"));
    m_actionLayerNew->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(m_actionLayerNew, &QAction::triggered, this, &MainWindow::addNewLayer);

    m_actionLayerDuplicate = new QAction(tr("Duplicate Layer(&D)"), this);
    m_actionLayerDuplicate->setObjectName(QStringLiteral("actionLayerDuplicate"));
    setActionIcon(m_actionLayerDuplicate, QStringLiteral("layer_duplicate"));
    m_actionLayerDuplicate->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
    connect(m_actionLayerDuplicate, &QAction::triggered,
            this, &MainWindow::duplicateCurrentLayer);

    m_actionLayerDelete = new QAction(tr("Delete Layer"), this);
    m_actionLayerDelete->setObjectName(QStringLiteral("actionLayerDelete"));
    setActionIcon(m_actionLayerDelete, QStringLiteral("layer_delete"));
    connect(m_actionLayerDelete, &QAction::triggered, this, &MainWindow::deleteCurrentLayer);

    m_actionLayerMoveUp = new QAction(tr("Move Layer Up"), this);
    m_actionLayerMoveUp->setObjectName(QStringLiteral("actionLayerMoveUp"));
    setActionIcon(m_actionLayerMoveUp, QStringLiteral("layer_up"));
    m_actionLayerMoveUp->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
    connect(m_actionLayerMoveUp, &QAction::triggered, this, &MainWindow::moveLayerUp);

    m_actionLayerMoveDown = new QAction(tr("Move Layer Down"), this);
    m_actionLayerMoveDown->setObjectName(QStringLiteral("actionLayerMoveDown"));
    setActionIcon(m_actionLayerMoveDown, QStringLiteral("layer_down"));
    m_actionLayerMoveDown->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    connect(m_actionLayerMoveDown, &QAction::triggered, this, &MainWindow::moveLayerDown);

    m_actionMergeDown = new QAction(tr("Merge Down(&M)"), this);
    m_actionMergeDown->setObjectName(QStringLiteral("actionMergeDown"));
    setActionIcon(m_actionMergeDown, QStringLiteral("layer_merge_down"));
    m_actionMergeDown->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(m_actionMergeDown, &QAction::triggered, this, &MainWindow::mergeLayerDown);

    m_actionFlatten = new QAction(tr("Flatten Image(&F)"), this);
    m_actionFlatten->setObjectName(QStringLiteral("actionFlatten"));
    setActionIcon(m_actionFlatten, QStringLiteral("layer_flatten"));
    connect(m_actionFlatten, &QAction::triggered, this, &MainWindow::flattenImage);

    m_actionLayerMergeVisible = new QAction(tr("Merge Visible Layers(&V)"), this);
    m_actionLayerMergeVisible->setObjectName(QStringLiteral("actionLayerMergeVisible"));
    setActionIcon(m_actionLayerMergeVisible, QStringLiteral("layer_merge_visible"));
    // Ctrl+Shift+E：与"向下合并"的 Ctrl+E 成对，Shift 表示"范围更大"。
    m_actionLayerMergeVisible->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(m_actionLayerMergeVisible, &QAction::triggered,
            this, &MainWindow::mergeVisibleLayers);

    m_actionLayerRename = new QAction(tr("Rename Layer(&R)"), this);
    m_actionLayerRename->setObjectName(QStringLiteral("actionLayerRename"));
    setActionIcon(m_actionLayerRename, QStringLiteral("layer_rename"));
    // ★ 用 F2 而不是回车 ★
    //   回车在图层面板里已经被"确认重命名"占用了；F2 是各平台通用的
    //   "重命名当前项"，而且它不会和列表的键盘导航冲突。
    m_actionLayerRename->setShortcut(QKeySequence(Qt::Key_F2));
    connect(m_actionLayerRename, &QAction::triggered,
            this, &MainWindow::renameActiveLayer);

    m_actionLayerMaskAdd = new QAction(tr("Add Layer Mask from Selection(&M)"), this);
    m_actionLayerMaskAdd->setObjectName(QStringLiteral("actionLayerMaskAdd"));
    setActionIcon(m_actionLayerMaskAdd, QStringLiteral("layer_mask_add"));
    m_actionLayerMaskAdd->setStatusTip(
        tr("Hide everything outside the current selection without deleting pixels"));
    connect(m_actionLayerMaskAdd, &QAction::triggered,
            this, &MainWindow::addLayerMaskFromSelection);

    m_actionLayerMaskRemove = new QAction(tr("Remove Layer Mask(&K)"), this);
    m_actionLayerMaskRemove->setObjectName(QStringLiteral("actionLayerMaskRemove"));
    setActionIcon(m_actionLayerMaskRemove, QStringLiteral("layer_mask_remove"));
    m_actionLayerMaskRemove->setStatusTip(
        tr("Delete the mask and show the whole layer again"));
    connect(m_actionLayerMaskRemove, &QAction::triggered,
            this, &MainWindow::removeLayerMask);

    // ─────────────────────────────────────────────────────────────────────
    //  视图
    // ─────────────────────────────────────────────────────────────────────

    m_actionZoomIn = new QAction(tr("Zoom In(&I)"), this);
    m_actionZoomIn->setObjectName(QStringLiteral("actionZoomIn"));
    setActionIcon(m_actionZoomIn, QStringLiteral("view_zoom_in"));
    // QKeySequence::ZoomIn 会解析成 Ctrl++（同时接受 Ctrl+= 的变体）
    m_actionZoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(m_actionZoomIn, &QAction::triggered, this, &MainWindow::zoomIn);

    m_actionZoomOut = new QAction(tr("Zoom Out(&O)"), this);
    m_actionZoomOut->setObjectName(QStringLiteral("actionZoomOut"));
    setActionIcon(m_actionZoomOut, QStringLiteral("view_zoom_out"));
    m_actionZoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(m_actionZoomOut, &QAction::triggered, this, &MainWindow::zoomOut);

    m_actionZoomFit = new QAction(tr("Fit to Window(&F)"), this);
    m_actionZoomFit->setObjectName(QStringLiteral("actionZoomFit"));
    setActionIcon(m_actionZoomFit, QStringLiteral("view_zoom_fit"));
    m_actionZoomFit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(m_actionZoomFit, &QAction::triggered, this, &MainWindow::zoomToFit);

    m_actionZoomActual = new QAction(tr("Actual Size(&A)"), this);
    m_actionZoomActual->setObjectName(QStringLiteral("actionZoomActual"));
    setActionIcon(m_actionZoomActual, QStringLiteral("view_zoom_100"));
    m_actionZoomActual->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    connect(m_actionZoomActual, &QAction::triggered, this, &MainWindow::zoomToActualSize);

    m_actionShowGrid = new QAction(tr("Show Grid(&G)"), this);
    m_actionShowGrid->setObjectName(QStringLiteral("actionShowGrid"));
    setActionIcon(m_actionShowGrid, QStringLiteral("view_grid"));
    m_actionShowGrid->setCheckable(true);
    m_actionShowGrid->setChecked(false);
    connect(m_actionShowGrid, &QAction::toggled, this, &MainWindow::toggleGrid);

    m_actionShowCheckerboard = new QAction(tr("Show Transparent Background"), this);
    m_actionShowCheckerboard->setObjectName(QStringLiteral("actionShowCheckerboard"));
    setActionIcon(m_actionShowCheckerboard, QStringLiteral("view_checkerboard"));
    m_actionShowCheckerboard->setCheckable(true);
    m_actionShowCheckerboard->setChecked(true);
    connect(m_actionShowCheckerboard, &QAction::toggled,
            this, &MainWindow::toggleCheckerboard);

    // 全屏（F11）。★ 不用可勾选动作 ★
    //   勾选态要靠窗口真实状态回写；用户从系统菜单退出全屏时，
    //   一个没跟上来的勾只会让人以为"菜单里还写着全屏"。
    m_actionFullScreen = new QAction(tr("Full Screen(&U)"), this);
    m_actionFullScreen->setObjectName(QStringLiteral("actionFullScreen"));
    setActionIcon(m_actionFullScreen, QStringLiteral("view_fullscreen"));
    m_actionFullScreen->setShortcut(QKeySequence(Qt::Key_F11));
    m_actionFullScreen->setStatusTip(tr("Toggle full screen (F11)"));
    connect(m_actionFullScreen, &QAction::triggered, this, &MainWindow::toggleFullScreen);

    // ── 网格的间距与吸附（阶段七收尾）──
    //
    //  ★ 快捷键的取舍 ★
    //    参考线「显示」占了 Ctrl+;、「锁定」占了 Ctrl+Alt+; ——
    //    吸附与它俩同族，于是顺势用 **Ctrl+Shift+;**（三连）。
    //    "显示网格"本身没给快捷键（它是纯呈现，用得没吸附频繁）。
    m_actionSnapToGrid = new QAction(tr("Snap to Grid(&S)"), this);
    m_actionSnapToGrid->setObjectName(QStringLiteral("actionSnapToGrid"));
    setActionIcon(m_actionSnapToGrid, QStringLiteral("view_grid_snap"));
    m_actionSnapToGrid->setCheckable(true);
    m_actionSnapToGrid->setChecked(false);
    m_actionSnapToGrid->setShortcut(
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Semicolon));
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
            QAction *act = new QAction(tr("%1 px").arg(kGridPresets[i]), this);
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

    m_actionGridCustom = new QAction(tr("Custom(&U)..."), this);
    m_actionGridCustom->setObjectName(QStringLiteral("actionGridCustom"));
    setActionIcon(m_actionGridCustom, QStringLiteral("view_grid_spacing"));
    connect(m_actionGridCustom, &QAction::triggered, this, &MainWindow::editGridSettings);

    // ── 标尺与参考线（阶段七：视图辅助）──
    //
    //  ★ 快捷键的取舍 ★
    //    Photoshop 里"标尺"是 Ctrl+R，但本工程的 Ctrl+R 已经是
    //    「顺时针旋转 90°」—— 那更常用，而且是**破坏性操作**（会改像素），
    //    不该给一个纯视图开关让位。于是标尺挪到 Ctrl+Alt+R。
    //    参考线的 Ctrl+; / Ctrl+Alt+; 在本工程没有占用，按惯例直接用。
    m_actionShowRuler = new QAction(tr("Show Ruler(&R)"), this);
    m_actionShowRuler->setObjectName(QStringLiteral("actionShowRuler"));
    setActionIcon(m_actionShowRuler, QStringLiteral("view_ruler"));
    m_actionShowRuler->setCheckable(true);
    m_actionShowRuler->setChecked(true);              // 默认显示
    m_actionShowRuler->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R));
    connect(m_actionShowRuler, &QAction::toggled, this, &MainWindow::toggleRuler);

    m_actionShowGuides = new QAction(tr("Show Guides(&G)"), this);
    m_actionShowGuides->setObjectName(QStringLiteral("actionShowGuides"));
    setActionIcon(m_actionShowGuides, QStringLiteral("view_guides"));
    m_actionShowGuides->setCheckable(true);
    m_actionShowGuides->setChecked(true);
    m_actionShowGuides->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Semicolon));
    connect(m_actionShowGuides, &QAction::toggled, this, &MainWindow::toggleGuides);

    m_actionLockGuides = new QAction(tr("Lock Guides(&L)"), this);
    m_actionLockGuides->setObjectName(QStringLiteral("actionLockGuides"));
    setActionIcon(m_actionLockGuides, QStringLiteral("view_guides_lock"));
    m_actionLockGuides->setCheckable(true);
    m_actionLockGuides->setChecked(false);
    m_actionLockGuides->setShortcut(
        QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Semicolon));
    connect(m_actionLockGuides, &QAction::toggled,
            this, &MainWindow::toggleGuidesLocked);

    m_actionClearGuides = new QAction(tr("Clear Guides(&C)"), this);
    m_actionClearGuides->setObjectName(QStringLiteral("actionClearGuides"));
    setActionIcon(m_actionClearGuides, QStringLiteral("view_guides_clear"));
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

    m_actionForegroundColor = new QAction(tr("Set Foreground Color..."), this);
    m_actionForegroundColor->setObjectName(QStringLiteral("actionForegroundColor"));
    setActionIcon(m_actionForegroundColor, QStringLiteral("color_foreground"));
    connect(m_actionForegroundColor, &QAction::triggered,
            this, &MainWindow::chooseForegroundColor);

    m_actionBackgroundColor = new QAction(tr("Set Background Color..."), this);
    m_actionBackgroundColor->setObjectName(QStringLiteral("actionBackgroundColor"));
    setActionIcon(m_actionBackgroundColor, QStringLiteral("color_background"));
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
    m_actionSwapColors = new QAction(tr("Swap Foreground/Background Color"), this);
    m_actionSwapColors->setObjectName(QStringLiteral("actionSwapColors"));
    setActionIcon(m_actionSwapColors, QStringLiteral("color_swap"));
    connect(m_actionSwapColors, &QAction::triggered, this, &MainWindow::swapColors);

    m_actionResetColors = new QAction(tr("Restore Default Colors"), this);
    m_actionResetColors->setObjectName(QStringLiteral("actionResetColors"));
    setActionIcon(m_actionResetColors, QStringLiteral("color_reset"));
    connect(m_actionResetColors, &QAction::triggered, this, &MainWindow::resetColors);

    // ─────────────────────────────────────────────────────────────────────
    //  帮助
    // ─────────────────────────────────────────────────────────────────────

    m_actionAbout = new QAction(tr("About %1(&A)...").arg(zl::appinfo::displayName()), this);
    m_actionAbout->setObjectName(QStringLiteral("actionAbout"));
    setActionIcon(m_actionAbout, QStringLiteral("help_about"));
    m_actionAbout->setMenuRole(QAction::AboutRole);
    connect(m_actionAbout, &QAction::triggered, this, [this]() {
        // ★ 为什么不用 QMessageBox::about ★
        //
        //   about() 是**静态函数**，它把父窗口的 windowIcon 按平台样式里
        //   写死的 PM_MessageBoxIconSize 光栅化 —— Windows 上这个值是 64。
        //   于是 512×512 的 logo 被压成 64×64 摆在左上角，而且没有任何
        //   API 能改这个尺寸（about() 连 QMessageBox 实例都不给你）。
        //
        //   唯一的出路是自己 new 一个 QMessageBox，用 setIconPixmap()
        //   指定任意大的图标 —— QMessageBox 不会去缩放它。
        QMessageBox box(this);
        box.setWindowTitle(tr("About %1").arg(zl::appinfo::displayName()));

        // ★ 按设备像素比渲染，高 DPI 屏上不发糊 ★
        //   dpr 取自**本窗口**而不是 qApp —— 多屏且缩放不同时，
        //   qApp 给的是主屏的值，窗口在副屏上就会取错。
        //   ★ 渲染出来的是 px×dpr 的物理像素图，必须把 dpr 设回去 ★
        //     否则 Qt 会按 1:1 摆到界面上 —— 150% 屏上图标会显示成
        //     192 个逻辑像素（比预期大了 1.5 倍）。设回 dpr 后，
        //     物理像素一一对应，逻辑尺寸仍是 128。
        const qreal dpr = devicePixelRatioF();
        QPixmap logo = zl::appinfo::windowIconPixmap(qRound(128 * dpr));
        if (!logo.isNull()) {
            logo.setDevicePixelRatio(dpr);
            box.setIconPixmap(logo);
        }
        // 图标渲染失败（PNG 解码失败）时不设 —— 与主窗口"降级为没有图标"
        // 一致，绝不在这里崩溃。

        box.setTextFormat(Qt::RichText);
        box.setTextInteractionFlags(Qt::TextBrowserInteraction);
        box.setText(
        tr("<h3>%1</h3>"
           "<p>A lightweight image editor built with Qt.</p>"
           "<p><b>Version</b> %4</p>"
           "<p><b>Tech Stack</b><br>"
           "Qt %2 (built against %3)<br>"
           "Pure Qt implementation, no third-party image libraries</p>"
           "<p><b>Key Features</b><br>"
           "Multiple layers with 22 blend modes · grayscale mask selection (with feathering)<br>"
           "Curves / Levels / Brightness-Contrast · custom bicubic and Lanczos resampling<br>"
           "Dirty-rect and full-layer snapshot dual undo strategy · EXIF auto-rotation</p>"
           "<p><b>Project Home</b><br>"
           "<a href=\"https://github.com/Zoulei-CSU/ZlPhotoEdit\">github.com/Zoulei-CSU/ZlPhotoEdit</a></p>")
                .arg(zl::appinfo::titleSuffix())
                .arg(zl::compat::qtVersionString())
                .arg(QStringLiteral(QT_VERSION_STR))
                .arg(QCoreApplication::applicationVersion()));
        box.setStandardButtons(QMessageBox::Ok);

        // ★ 撑开文字列（图标从 64 变 128 之后图标列多吃 64px）★
        //   ★ 注意：对 QMessageBox 调 setMinimumWidth() 是**无效的** ★
        //     它在 showEvent 里会按自己的 layout sizeHint 重新摆一次，
        //     把外面设的最小宽度覆盖掉（实测设 560 仍是 400 宽）。
        //     能起作用的是让**内部文字标签**变宽 —— layout 的 sizeHint
        //     随之变大，QMessageBox 自己就会跟着变宽。
        //   "qt_msgbox_label" 是 Qt 给文字标签定的 objectName（各版本一致），
        //   找不到就算了（不崩、不报错，只是回到默认宽度）。
        if (auto *lbl = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
            lbl->setMinimumWidth(340);
            // ★ 链接要真的能点开 ★
            //   TextBrowserInteraction 只让链接"可点"，真正唤起默认浏览器
            //   要靠 openExternalLinks。找不到这个标签就算了（见上面的
            //   宽度注释）—— 链接仍在，只是不跳转，不崩不报错。
            lbl->setOpenExternalLinks(true);
        }

        box.exec();
    });

    m_actionAboutQt = new QAction(tr("About Qt(&Q)..."), this);
    m_actionAboutQt->setObjectName(QStringLiteral("actionAboutQt"));
    setActionIcon(m_actionAboutQt, QStringLiteral("help_qt"));
    m_actionAboutQt->setMenuRole(QAction::AboutQtRole);
    connect(m_actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
}

// ============================================================================
//  promptRestartForLanguage —— 语言切换的"重启生效"提示
// ============================================================================

void MainWindow::promptRestartForLanguage(const QString &displayNameText)
{
    // 保存动作在 setCurrentLanguage 里已经落盘（含 sync()），这里只负责
    // 把"重启生效"说清楚。文本本身也走 tr() —— 英文界面下提示也是英文
    // （显示的是切换**前**的当前语言，因为翻译器要重启后才换）。
    QMessageBox::information(
        this,
        tr("Switch UI Language"),
        tr("The UI language has been set to %1. It takes effect the next "
           "time the program starts.").arg(displayNameText));
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
    QMenu *fileMenu = mb->addMenu(tr("File(&F)"));
    fileMenu->setObjectName(QStringLiteral("menuFile"));
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
    QMenu *captureMenu = fileMenu->addMenu(tr("Screen Capture(&C)"));
    captureMenu->setObjectName(QStringLiteral("menuScreenCapture"));
    setActionIcon(captureMenu->menuAction(), QStringLiteral("menu_screen_capture"));
    captureMenu->addAction(m_actionCaptureNow);
    // 三条延迟项归成一组：同一个动作的三个档，与"立即"之间用横线分开
    captureMenu->addSeparator();
    captureMenu->addAction(m_actionCaptureDelay3);
    captureMenu->addAction(m_actionCaptureDelay5);
    captureMenu->addAction(m_actionCaptureDelay10);

    // 最近打开（内容由 updateRecentFileActions 动态填充）
    QMenu *recentMenu = fileMenu->addMenu(tr("Recent Files(&R)"));
    recentMenu->setObjectName(QStringLiteral("menuRecentFiles"));
    // ★ 子菜单也要图标 ★
    //   QMenu::addMenu() 返回的 QMenu 自带一个 menuAction()，
    //   setIcon() 就是给那个动作挂图 —— 它和普通菜单项走同一套绘制，
    //   所以子菜单在有图标的菜单里不会显得"掉队"。
    setActionIcon(recentMenu->menuAction(), QStringLiteral("menu_recent"));
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
    auto *clearRecentAction = recentMenu->addAction(tr("Clear List"));
    connect(clearRecentAction, &QAction::triggered, this, [this]() {
        QSettings settings;
        settings.remove(QStringLiteral("files/recent"));
        updateRecentFileActions();
    });

    fileMenu->addSeparator();
    fileMenu->addAction(m_actionSave);
    fileMenu->addAction(m_actionSaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actionPrint);
    fileMenu->addAction(m_actionExportPdf);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actionClose);
    fileMenu->addAction(m_actionQuit);

    // ── 主题（子菜单）──
    //
    //  ★ 为什么放在「退出」之后的最后一项 ★
    //    上面全是"对文件做的事"（新建/打开/存/打印/关），主题与它们无关；
    //    排在最后，用户找文件操作时不会被它打断。
    //    与"语言"放在菜单栏末尾同一个位置逻辑 —— 都是应用级偏好，
    //    不掺进业务动作里。
    //
    //  ★ 子菜单也要图标 ★
    //    它是这个菜单里唯一一个没有图标的条目，不挂的话在"整栏都有图标"
    //    的菜单里一眼就能看出缺一块。
    QMenu *themeMenu = fileMenu->addMenu(tr("Theme(&T)"));    themeMenu->setObjectName(QStringLiteral("menuTheme"));
    setActionIcon(themeMenu->menuAction(), QStringLiteral("menu_theme"));
    for (QAction *act : m_actionTheme)
        themeMenu->addAction(act);

    // ─────────────────────────────────────────────────────────────────────
    //  编辑
    // ─────────────────────────────────────────────────────────────────────
    QMenu *editMenu = mb->addMenu(tr("Edit(&E)"));
    editMenu->setObjectName(QStringLiteral("menuEdit"));
    editMenu->addAction(m_actionUndo);
    editMenu->addAction(m_actionRedo);
    editMenu->addSeparator();
    editMenu->addAction(m_actionCut);
    editMenu->addAction(m_actionCopy);
    editMenu->addAction(m_actionPaste);
    editMenu->addAction(m_actionPasteInto);
    editMenu->addSeparator();
    editMenu->addAction(m_actionDelete);
    editMenu->addSeparator();
    editMenu->addAction(m_actionUndoLimit);

    // ─────────────────────────────────────────────────────────────────────
    //  选择
    // ─────────────────────────────────────────────────────────────────────
    QMenu *selectMenu = mb->addMenu(tr("Select(&S)"));
    selectMenu->setObjectName(QStringLiteral("menuSelect"));
    selectMenu->addAction(m_actionSelectAll);
    selectMenu->addAction(m_actionDeselect);
    selectMenu->addAction(m_actionInvertSelection);

    // ─────────────────────────────────────────────────────────────────────
    //  调整
    // ─────────────────────────────────────────────────────────────────────
    QMenu *adjustMenu = mb->addMenu(tr("Adjust(&A)"));
    adjustMenu->setObjectName(QStringLiteral("menuAdjust"));
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
    QMenu *imageMenu = mb->addMenu(tr("Image(&I)"));
    imageMenu->setObjectName(QStringLiteral("menuImage"));
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
    QMenu *layerMenu = mb->addMenu(tr("Layer(&L)"));
    layerMenu->setObjectName(QStringLiteral("menuLayer"));
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
    layerMenu->addSeparator();
    layerMenu->addAction(m_actionLayerMaskAdd);
    layerMenu->addAction(m_actionLayerMaskRemove);

    // ─────────────────────────────────────────────────────────────────────
    //  视图
    // ─────────────────────────────────────────────────────────────────────
    QMenu *viewMenu = mb->addMenu(tr("View(&V)"));
    viewMenu->setObjectName(QStringLiteral("menuView"));
    viewMenu->addAction(m_actionZoomIn);
    viewMenu->addAction(m_actionZoomOut);
    viewMenu->addAction(m_actionZoomFit);
    viewMenu->addAction(m_actionZoomActual);
    viewMenu->addSeparator();
    viewMenu->addAction(m_actionShowGrid);
    viewMenu->addAction(m_actionShowCheckerboard);

    // ── 网格的间距与吸附 ──
    viewMenu->addAction(m_actionSnapToGrid);

    QMenu *gridSpacingMenu = viewMenu->addMenu(tr("Grid Spacing(&Z)"));
    gridSpacingMenu->setObjectName(QStringLiteral("menuGridSpacing"));
    setActionIcon(gridSpacingMenu->menuAction(), QStringLiteral("view_grid"));
    for (QAction *act : m_actionGridSpacing)
        gridSpacingMenu->addAction(act);
    gridSpacingMenu->addSeparator();
    gridSpacingMenu->addAction(m_actionGridCustom);

    // ── 视图辅助：标尺与参考线 ──
    viewMenu->addSeparator();
    viewMenu->addAction(m_actionShowRuler);

    // 「标尺单位」子菜单。★ 它自己带图标，5 个单选项不带 ★
    QMenu *unitMenu = viewMenu->addMenu(tr("Ruler Units(&N)"));
    unitMenu->setObjectName(QStringLiteral("menuRulerUnit"));
    setActionIcon(unitMenu->menuAction(), QStringLiteral("view_ruler"));
    for (QAction *act : m_actionRulerUnit)
        unitMenu->addAction(act);

    viewMenu->addSeparator();
    viewMenu->addAction(m_actionShowGuides);
    viewMenu->addAction(m_actionLockGuides);
    viewMenu->addAction(m_actionClearGuides);
    viewMenu->addSeparator();

    // 面板显隐菜单。内容在 setupDocks 里填充
    // （因为那时 QDockWidget 才存在）。
    QMenu *panelMenu = viewMenu->addMenu(tr("Panels(&P)"));
    panelMenu->setObjectName(QStringLiteral("menuPanel"));
    setActionIcon(panelMenu->menuAction(), QStringLiteral("menu_panels"));

    viewMenu->addSeparator();
    viewMenu->addAction(m_actionFullScreen);

    // ─────────────────────────────────────────────────────────────────────
    //  语言
    // ─────────────────────────────────────────────────────────────────────
    //  ★ 菜单结构：第一项"自动（跟随系统）"，其后是每种可用语言 ★
    //
    //    "自动"是默认状态（QSettings 没存过值就等于它）：按系统 locale
    //    推断，中文环境 → 简体中文，其余 → English。用户显式选过之后，
    //    locale 不再参与 —— 经常带着笔记本中英文环境来回跑的用户，
    //    不会某天早上发现界面语言跟着系统漂移了。
    //
    //  ★ 切换后要重启才生效 ★
    //    运行中切换要重翻译整个 UI：每个控件都要响应 LanguageChange 事件、
    //    动态拼的字符串（标题、状态栏、对话框文本）要全部重新生成 ——
    //    那是后续课题。v1 用"保存选择 + 弹提示 + 下次启动生效"，
    //    宁可诚实地说清楚，也不给一个"点了没反应"的死开关。
    //
    //  ★ 菜单总是显示 ★
    //    哪怕当前只有一种可用语言也显示：用户得知道"语言设置"在哪、
    //    默认行为是什么。没有可选语言时它孤零零两项，也好过一个
    //    找不到的菜单。
    QMenu *langMenu = mb->addMenu(tr("Language(&Y)"));
    langMenu->setObjectName(QStringLiteral("menuLanguage"));

    // 与「标尺单位」同款：互斥单选组，**不给单项设 objectName** ——
    // 对账规则是"有名字的菜单动作必须有图标"，为几个语言项各配一张
    // 图标纯属噪音。定位它们靠菜单本身（findChild<QMenu*>("menuLanguage")）。
    auto *langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);

    // 勾选态看"偏好"而不是"生效语言"：自动项在 locale 推断出某种语言时
    // 依然保持勾选 —— 那正是用户选"自动"的含义。
    // 垃圾偏好（用户手改过设置文件）按"自动"显示：与 currentLanguage()
    // 的解析口径一致（那里同样落到自动分支），否则菜单会一项都没勾，
    // 而 currentLanguage() 却说"在用 locale 推断的语言"—— 对不上。
    QString langPref = zl::uilang::languagePreference();
    if (langPref != zl::uilang::autoLanguage()
        && !zl::uilang::availableLanguages().contains(langPref))
        langPref = zl::uilang::autoLanguage();

    // "重启生效"提示是成员函数 promptRestartForLanguage()（声明处说明了
    // 为什么不能是局部 lambda：connect 存闭包，局部量出了作用域就失效）。

    QAction *autoLangAct = new QAction(tr("Auto (Follow System)"), this);
    autoLangAct->setCheckable(true);
    autoLangAct->setChecked(langPref == zl::uilang::autoLanguage());
    connect(autoLangAct, &QAction::triggered, this, [this] {
        zl::uilang::setCurrentLanguage(zl::uilang::autoLanguage());
        promptRestartForLanguage(
            zl::uilang::displayName(zl::uilang::currentLanguage()));
    });
    langGroup->addAction(autoLangAct);
    langMenu->addAction(autoLangAct);

    for (const QString &code : zl::uilang::availableLanguages()) {
        QAction *act = new QAction(zl::uilang::displayName(code), this);
        act->setCheckable(true);
        act->setChecked(langPref == code);
        connect(act, &QAction::triggered, this, [this, code] {
            zl::uilang::setCurrentLanguage(code);
            promptRestartForLanguage(zl::uilang::displayName(code));
        });
        langGroup->addAction(act);
        langMenu->addAction(act);
    }

    // ─────────────────────────────────────────────────────────────────────
    //  帮助
    // ─────────────────────────────────────────────────────────────────────
    QMenu *helpMenu = mb->addMenu(tr("Help(&H)"));
    helpMenu->setObjectName(QStringLiteral("menuHelp"));
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
    m_mainToolBar = addToolBar(tr("Main Toolbar"));
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

    QToolBar *optionsToolBar = addToolBar(tr("Tool Options"));
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
    m_toolsToolBar = new QToolBar(tr("Toolbox"), this);
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
        action->setStatusTip(tr("Switch to the %1 tool").arg(tool->displayName()));

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
    colorIndicator->setToolTip(tr("Foreground color (click to change)"));
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
    m_historyDock = new QDockWidget(tr("History"), this);
    m_historyDock->setObjectName(QStringLiteral("historyDock"));
    m_historyDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_undoView = new QUndoView(this);
    m_undoView->setEmptyLabel(tr("(Already at the initial state)"));
    // 不在这里 setStack —— applyDocumentToPanels 会在文档建立后绑定
    m_historyDock->setWidget(m_undoView);

    addDockWidget(Qt::RightDockWidgetArea, m_historyDock);

    // ─────────────────────────────────────────────────────────────────────
    //  颜色面板
    // ─────────────────────────────────────────────────────────────────────
    m_colorDock = new QDockWidget(tr("Color"), this);
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
        fgButton->setToolTip(tr("Foreground color (click to change)"));
        fgButton->setCursor(Qt::PointingHandCursor);

        auto *bgButton = new QPushButton(page);
        bgButton->setObjectName(QStringLiteral("backgroundSwatch"));
        bgButton->setFixedSize(48, 48);
        bgButton->setToolTip(tr("Background color (click to change)"));
        bgButton->setCursor(Qt::PointingHandCursor);

        swatchRow->addWidget(fgButton);
        swatchRow->addWidget(bgButton);

        // 交换 / 重置按钮（竖排放在色块右侧）
        auto *btnColumn = new QVBoxLayout;
        btnColumn->setSpacing(2);

        auto *swapBtn = new QToolButton(page);
        swapBtn->setText(QStringLiteral("⇄"));
        swapBtn->setToolTip(tr("Swap foreground and background colors (X)"));
        swapBtn->setAutoRaise(true);
        connect(swapBtn, &QToolButton::clicked, this, &MainWindow::swapColors);

        auto *resetBtn = new QToolButton(page);
        resetBtn->setText(QStringLiteral("◧"));
        resetBtn->setToolTip(tr("Restore default colors: black / white (D)"));
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
        hint->setText(tr("Hotkeys\n"
                         "  X    Swap foreground / background color\n"
                         "  D    Restore default colors (black / white)\n\n"
                         "Colors picked with the eyedropper become the foreground color."));
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
    m_layerDock = new QDockWidget(tr("Layers"), this);
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

    // 文件被拖到图层面板上：与拖到窗口空白处走同一条"批量打开"。
    // 面板为了拖拽排序必须 acceptDrops，所以这一步不能省 ——
    // 否则拖到面板上的文件会被它静默吃掉。
    connect(m_layerPanel, &LayerPanel::filesDropped,
            this, &MainWindow::openDocumentPaths);
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
    //  导航器面板（缩略图 + 可视框）
    // ─────────────────────────────────────────────────────────────────────
    //
    // 文档与画布都不在这里绑，由 applyDocumentToPanels() 统一推。
    m_navigatorDock = new QDockWidget(tr("Navigator"), this);
    m_navigatorDock->setObjectName(QStringLiteral("navigatorDock"));
    m_navigatorDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_navigatorPanel = new NavigatorPanel(this);
    m_navigatorDock->setWidget(m_navigatorPanel);

    addDockWidget(Qt::RightDockWidgetArea, m_navigatorDock);

    // ─────────────────────────────────────────────────────────────────────
    //  把面板的显隐动作填进"视图 → 面板"菜单
    // ─────────────────────────────────────────────────────────────────────
    // 注意 objectName 是 menuPanel（单数），与 setupMenus 里创建时一致。
    if (QMenu *panelMenu = menuBar()->findChild<QMenu *>(QStringLiteral("menuPanel"))) {
        panelMenu->addAction(m_navigatorDock->toggleViewAction());
        panelMenu->addAction(m_layerDock->toggleViewAction());
        panelMenu->addAction(m_historyDock->toggleViewAction());
        panelMenu->addAction(m_colorDock->toggleViewAction());
    }

    // 右侧四个面板竖排。并排的话每块只剩四分之一宽度，图层名看不清。
    splitDockWidget(m_navigatorDock, m_layerDock, Qt::Vertical);
    splitDockWidget(m_layerDock, m_historyDock, Qt::Vertical);
    splitDockWidget(m_historyDock, m_colorDock, Qt::Vertical);

    // 面板的初始宽度
    resizeDocks({ m_navigatorDock, m_layerDock, m_historyDock, m_colorDock },
                { 240, 240, 200, 180 }, Qt::Horizontal);
    // 初始高度：导航器只需要装下缩略图，颜色面板只有两个色块
    resizeDocks({ m_navigatorDock, m_layerDock, m_historyDock, m_colorDock },
                { 170, 260, 240, 130 }, Qt::Vertical);
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

    m_statusBar->showMessage(tr("Ready"), 3000);
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
        QLatin1String(kCaptureHotkey),   // Ctrl+Alt+S ← 必须留在第一位
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

    // ★ 会话本身就不允许抓屏时，提示文字要讲那个更根本的原因 ★
    //   这里若照常写"按 %1 截图"，菜单上就会挂着一条**它做不到的承诺**：
    //   用户按下去，弹出的是"Wayland 不允许"—— 两句话互相打脸。
    //   （本函数在 setupScreenCapture 里被调用了一次，所以不能在
    //     setupScreenCapture 里先把 statusTip 设成 Wayland 提示了事：
    //     那一行会被这里覆盖掉。提示文字的来源必须收敛到这一处。）
    if (screenCaptureBlock() == CaptureBlock::Wayland) {
        m_actionCaptureNow->setStatusTip(
            captureFailureText(CaptureBlock::Wayland));
        return;
    }

    m_actionCaptureNow->setStatusTip(
        tr("Capture the current screen into a new tab (%1; works even while the program is minimized)")
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
    // ── ⓪ 当前会话允不允许抓屏？（Wayland 下恒不允许）──
    //
    //  ★ 装配阶段只改 statusTip，**不说话、不弹框、不打日志** ★
    //    这个限制是常驻的，但它不是一次"操作结果"—— 程序刚启动就弹一个
    //    "你这里截不了图"的框，只会让用户以为软件坏了（他还没想截图）。
    //    所以"为什么 / 怎么办"只在**用户真的点了截图**那一刻说（见
    //    captureScreenNow 的失败路径），平时把同样的文字放在
    //    statusTip 里 —— 鼠标在菜单项上停一下就能看到，零打扰。
    //
    //  ★ 不因此禁用菜单 ★
    //    灰掉的菜单项只会让用户更困惑（连"为什么"都点不出来）。
    //    保持可点、点下去给一条能照着做的提示，比灰掉有用。
    if (screenCaptureBlock() == CaptureBlock::Wayland) {
        // 「立即截图」的提示不在这里设 —— 它的提示文字由 applyCaptureHotkey()
        // 单独负责（它稍后会被调用两次：setupActions 建动作时一次、下面登记
        // 成功时再改一次）。在这一处也写一遍的话，最后一次调用会把这里的
        // 文字覆盖掉，而"两处都写"正是这类覆盖最容易出问题的地方。
        const QString tip = captureFailureText(CaptureBlock::Wayland);
        m_actionCaptureDelay3->setStatusTip(tip);
        m_actionCaptureDelay5->setStatusTip(tip);
        m_actionCaptureDelay10->setStatusTip(tip);
    }

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
        tried << tr("%1 (%2)").arg(text, m_hotkey->lastError());
    }

    if (!effective.isEmpty()) {
        // ★ 把**真正生效**的键写回菜单 ★
        //   回落到第二个键时，菜单上若还写着首选键，用户拿到的就是一把错钥匙：
        //   他会一直按 Ctrl+Alt+S，然后认为这个功能是坏的。
        applyCaptureHotkey(effective);

        if (effective.toString(QKeySequence::PortableText)
            != QLatin1String(kCaptureHotkey)) {
            // 回落不是错误，但**必须说出来**，否则用户按首选键毫无反应时
            // 只会怀疑程序。状态栏 + 日志两处都留。
            if (m_statusBar) {
                m_statusBar->showMessage(
                    tr("The preferred hotkey %1 is occupied by another program; using %2 this time (works even while the program is minimized)")
                        .arg(QLatin1String(kCaptureHotkey),
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
            tr("Global hotkey unavailable (%1). It will not work while "
               "the program is minimized, but %2 still works in the "
               "foreground, or use File → Screen Capture → Capture Now")
                .arg(tried.join(QStringLiteral("；")),
                     QLatin1String(kCaptureHotkey)),
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

    // ── 编辑偏好：撤销步数 ──
    //   存档越界时夹回去。setUndoLimit 会把值推给所有已打开的页，
    //   所以这里不需要关心"加载时到底有没有页"。
    setUndoLimit(settings.value(QStringLiteral("edit/undoLimit"), 30).toInt());

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

    // ── 编辑偏好 ──
    settings.setValue(QStringLiteral("edit/undoLimit"), m_undoLimit);

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
