// ============================================================================
//  mainwin_test.cpp —— MainWindow「启动装配」的离线自检
// ----------------------------------------------------------------------------
//  ★ 本文件的由来（两个实测踩到的静默 bug）★
//
//    有两类错误编译器抓不到、已有的每一组自检也都抓不到 —— 它们不在算法里，
//    而在 MainWindow 构造期的**装配顺序**上：
//
//      ① **工具箱一个按钮都没有**
//         buildToolBox() 原先在 setupToolBars() 末尾被调用，而构造函数里
//         setupToolBars() 跑在 setupTools() **之前** —— 那时 m_toolManager
//         还是 nullptr，函数第一行的卫语句直接 return。工具箱永远是空的，
//         而且悄无声息：编译器不管，离线自检不建窗口。
//         症状：用户找不到裁剪/画笔这些工具按钮，只能靠菜单与快捷键。
//
//      ② **画布没有活动工具**
//         connect(activeToolChanged) 原先写在 loadAllOptions() 之后，而
//         loadAllOptions() 会恢复 QSettings 里的"上次使用的工具"并 emit 一次
//         —— 那次 emit 落在连接建立之前，于是 CanvasView::m_activeTool
//         停在 nullptr。
//         症状：打开程序后**画布上任何鼠标操作都没有反应**，因为
//         CanvasView::shouldDispatchToTool() 第一行就因 m_activeTool 为空
//         而返回 false。更麻烦的是"点一下工具箱里那个已经选中的按钮"也
//         修不好：ToolManager::setActiveTool() 发现"已经是这个工具"就早退、
//         不发信号 —— 用户必须先切到别的工具再切回来。
//
//    两个 bug 的共同特征是**整体失效却不报错**，所以必须有断言盯着。
//
//  ★ 判据怎么写的（以及为什么这样写才抓得住）★
//
//    · 不断言"工具箱里有 6 个按钮"这种数字 —— 而是断言
//      **按钮数恒等于 ToolManager 里的工具数**，且每个 id 都能按名字找到。
//      写死数字的话，Phase 6 加工具时这条断言就会变成"改一次代码要改一次
//      测试"，最后被人随手删掉。恒等式不会。
//    · 不断言"画布的活动工具是 move" —— 而是断言
//      **画布的活动工具恒等于 ToolManager 的活动工具、且非空**。
//      这是不变量，与"上次用的是哪个工具"无关。
//    · "恢复上次使用的工具"必须**真正驱动到画布**：每轮往临时 QSettings 里
//      写一个不同的 tools/lastActive 再构造 MainWindow，检查画布、
//      ToolManager、工具箱选中态三者一致。只试一个值只能覆盖一条路径。
//
//  ★ 运行环境 ★
//    需要 QApplication + offscreen 平台（要建真实控件、发真实鼠标事件）。
//    它是**唯一真正构造 MainWindow 的自检**，因此要链接全部 src/，
//    是全量清单里最慢的一个（排在最后）。
//    QSettings 被重定向到临时目录下的 INI 文件 —— 测试绝不写用户的注册表，
//    也不受用户上次退出时留下的"上次使用的工具"影响（否则测试会
//    随用户的使用习惯时红时绿，这类"环境依赖的假失败"最耗人）。
//
//  ★ 输出编码：含中文的动态串一律 qUtf8Printable ★
//    源码字面量是 UTF-8（文件带 BOM），而 qPrintable 走 toLocal8Bit() ——
//    在中文 Windows 上是 GBK 字节。两者混在同一个日志里，用 UTF-8 读就会
//    "一半正常一半乱码"，恰好把出问题那几条的信息糊掉。
//    失败信息读不出来，断言的价值就打折了。
// ============================================================================

#include "compat/GlobalHotkey.h"
#include "compat/ScreenCapture.h"

#include "ui/MainWindow.h"
#include "ui/CanvasView.h"
#include "ui/IconLibrary.h"
// ★ 程序显示名与图标 ★ 窗口标题、"关于"框、以及任务栏图标都从它取 ——
//   本文件要断言"标题里的名字与 AppInfo 一致"，而不是写死字符串，
//   否则改一次中文名就要改一次测试（然后被人随手删掉）。
#include "ui/AppInfo.h"
#include "ui/LayerPanel.h"
#include "ui/NavigatorPanel.h"
#include "core/RecoveryManager.h"
// ★ 状态栏 ★ 用来读"缩放"那一栏显示的数字 —— 多标签下它是
//   "切页要显式补推"的又一处（见 testDocumentTabs 的 ③b 段）。
#include "ui/StatusBarWidget.h"
// ★ 多标签容器 ★ 它是中央控件，"当前是第几页"这件事只有它知道，
//   所以凡是需要"当前画布 / 当前撤销栈"的断言都从它取。
#include "ui/DocumentTabs.h"
#include "ui/RulerWidget.h"
#include "engine/RulerMetrics.h"
#include "commands/CommandStack.h"
#include "tools/ToolManager.h"
#include "tools/AbstractTool.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
// ★ 界面语言 ★ 语言决策/持久化/装载的唯一出处；本组钉它的全部行为。
//   注意本组**不编 i18n.qrc**（翻译只由主程序内嵌），所以断言必须
//   在"没有 .qm"的环境下也成立 —— 这本身就是被测性质的一部分。
#include "core/UiLanguage.h"
// ★ 界面主题 ★ 主题菜单三项的取值、持久化、应用都在这里；
//   本组要钉"切了真的生效"，所以既要用它的 Mode，也要量 palette 与图标。
#include "core/UiTheme.h"
#include "engine/Geometry.h"   // 打开为图层：unionSize / centeredOffset（放置与尺寸的判据）
// ★ 另存为 ICO ★ 512 上限的判据在引擎层，界面层只负责"把它弹给用户"；
//   本组要同时钉住"引擎拒绝"与"界面真的调到了引擎"。
#include "engine/IcoWriter.h"
#include "engine/ImageMeta.h"  // writableFormatFilters / formatSupportsAlpha（格式能力）

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDir>
// 拖文件打开那组：构造一个真实的拖放进入事件交给窗口。
#include <QDragEnterEvent>
// 同一组：macOS 的"用本程序打开文件"事件走 QEvent::FileOpen。
#include <QFileOpenEvent>
#include <QMimeData>
#include <QUrl>
// 历史面板跳转那组：QUndoView 挂在 historyDock 里，点击条目走的是
// selectionModel 的 currentChanged（QUndoView 内部接到 setIndex）。
#include <QDockWidget>
#include <QModelIndex>
#include <QUndoView>
// 屏幕截图那几组要用到：等待"延迟截图"的定时器到点（要真的跑事件循环）、
// 以及把系统消息交给事件循环处理的时间预算。
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPoint>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QSettings>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QtGlobal>
#include <utility>

#include <algorithm>   // 网格像素组：对"相邻线间距"取中位数
#include <cstdio>

using namespace zl;

// 前置声明：按 objectName 从窗口里找动作。
// ★ 为什么需要它 ★
//   它的定义在文件后半段（靠近屏幕截图那组），而「网格间距」那组
//   用到它、位置更靠前。C++ 不允许调用后面才声明的函数，
//   所以这里前置一次 —— 比把整组测试搬到文件末尾更不容易出错。
static QAction *namedAction(MainWindow *w, const char *name);

static int g_failed = 0;
static int g_passed = 0;
static int g_group  = 0;

static void beginGroup(const char *name)
{
    ++g_group;
    std::printf("[%d] %s\n", g_group, name);
    std::fflush(stdout);
}

static void check(bool ok, const char *what)
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n", what);
    }
}

static void checkEqInt(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

static void checkStr(const QString &actual, const QString &expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 \"%s\"，实际 \"%s\"\n",
                    what, qUtf8Printable(expected), qUtf8Printable(actual));
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  临时设置：把 QSettings 引到临时目录，绝不碰用户注册表
// ---------------------------------------------------------------------------

static QTemporaryDir *g_settingsDir = nullptr;

static void redirectSettingsToTempDir()
{
    g_settingsDir = new QTemporaryDir();
    if (!g_settingsDir->isValid()) {
        std::printf("  [失败] 建不出临时目录，测试无法在隔离环境下运行\n");
        ++g_failed;
        return;
    }

    QCoreApplication::setOrganizationName(QStringLiteral("ZlPhotoEditTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MainWindowAssemblyTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       g_settingsDir->path());

    std::printf("  设置文件位置：%s\n", qUtf8Printable(g_settingsDir->path()));
}

// ---------------------------------------------------------------------------
//  构造辅助
// ---------------------------------------------------------------------------

// 构造一个"上次用的是 lastActive 这个工具"的完整主窗口。
//
// ★ 这里必须先 clear() 再写 lastActive ★
//   上一轮窗口析构时会把它的活动工具写回 tools/lastActive，
//   不清掉的话"这一轮到底测的是哪个值"就取决于上一轮的构造，测试会
//   随着执行顺序飘 —— 那种失败最难查。
static MainWindow *newWindow(const QString &lastActive)
{
    {
        QSettings s;
        s.clear();
        s.setValue(QStringLiteral("tools/lastActive"), lastActive);
        s.sync();
    }

    auto *w = new MainWindow;
    // ★ 离屏自检里**一律关掉截图失败的模态框** ★
    //   抓不到屏幕时交互路径会弹框（那是产品行为），但离屏环境没有人点
    //   "确定"—— 模态框会把整个测试进程永久挂住，表现是"跑到某一组就卡死"，
    //   而看起来完全不像框的问题。与 importImageAsLayer 的 GrowPolicy 同理：
    //   把"自动化不弹框"做成结构性保证，而不是靠每处记得判断。
    w->setScreenCaptureDialogsEnabled(false);
    w->resize(1000, 700);
    w->show();
    QApplication::processEvents();   // 让 show 触发的布局/重绘跑完
    return w;
}

static CanvasView  *canvasOf(MainWindow *w) { return w->findChild<CanvasView *>(); }
static ToolManager *toolsOf(MainWindow *w)  { return w->findChild<ToolManager *>(); }

// ★ 中央控件（多标签容器）★
static DocumentTabs *tabsOf(MainWindow *w) { return w->findChild<DocumentTabs *>(); }

// ★ "当前那一页的画布" —— 注意它与 canvasOf() 的区别 ★
//
//  canvasOf() 是 findChild<CanvasView*>，多标签下它返回的是**子对象顺序里
//  第一个**画布，未必是当前显示的那一块（后台页的画布也是子对象）。
//  单页时两者恰好相同，所以旧断言在加上多标签之后仍然绿 ——
//  但"当前画布"这类断言必须走这里，否则它测的是随机的一页。
static CanvasView *currentCanvasOf(MainWindow *w)
{
    DocumentTabs *t = tabsOf(w);
    DocumentTab *tab = t ? t->currentTab() : nullptr;
    return tab ? tab->canvas : nullptr;
}

// 状态栏上「缩放」那一栏显示的百分比（读不到时返回 -1）。
//
// ★ 为什么从 StatusBarWidget 里找那个 QComboBox，而不是全窗口扫一遍 ★
//   全窗口 findChildren<QComboBox*>() 会连工具选项条上的下拉框一起拿到，
//   而"哪个里面装着百分号"只是个巧合式判据 —— 工具选项条上再加一个
//   百分比参数（不透明度、容差百分比…）就会让这条断言测到别的控件上。
static qreal statusZoomPercent(MainWindow *w)
{
    auto *sb = w->findChild<StatusBarWidget *>();
    QComboBox *combo = sb ? sb->findChild<QComboBox *>() : nullptr;
    if (!combo)
        return -1.0;

    QString t = combo->currentText().trimmed();
    t.remove(QLatin1Char('%'));
    bool ok = false;
    const qreal v = t.toDouble(&ok);
    return ok ? v : -1.0;
}

static AbstractTool *activeToolOf(MainWindow *w)
{
    ToolManager *tm = toolsOf(w);
    return tm ? tm->activeTool() : nullptr;
}

static QString activeToolId(MainWindow *w)
{
    AbstractTool *t = activeToolOf(w);
    return t ? t->id() : QStringLiteral("<null>");
}

static bool canvasHasActiveTool(MainWindow *w)
{
    return activeToolOf(w) != nullptr;
}

// 工具箱里"看起来是工具按钮"的那些（排除分隔符与颜色指示器）。
static QStringList toolButtonIds(MainWindow *w)
{
    QStringList ids;
    auto *tb = w->findChild<QToolBar *>(QStringLiteral("toolsToolBar"));
    if (!tb)
        return ids;

    for (QAction *a : tb->actions()) {
        if (!a || a->isSeparator())
            continue;
        const QString name = a->objectName();
        // 命名规则见 buildToolBox()：toolAction_<工具 id>
        if (name.startsWith(QLatin1String("toolAction_")))
            ids << name.mid(QStringLiteral("toolAction_").size());
    }
    return ids;
}

// 「图像」菜单 —— aboutToShow 里刷新「裁剪」的可用性，
// 所以必须真的弹一次菜单才能读到正确状态（规格见
// docs/画布与文件操作设计.md 6.8）。
// ★ 按 objectName 找，不按菜单文本 ★
//   阶段 2 英文化后菜单文本变成英文（"Image"），按中文文本查找会静默
//   返回 nullptr —— 症状是"找得到菜单"这条断言失败，而真正的原因是
//   查找方式过时。objectName 与界面语言无关，两种语言下都成立。
static QMenu *imageMenuOf(MainWindow *w)
{
    return w->findChild<QMenu *>(QStringLiteral("menuImage"));
}

// 「文件」菜单 —— 用来断言「打开为图层」确实被挂进去了。
// 理由见 testOpenAsLayerAssembly() 的说明：只断言"动作存在"是不够的。
// （同上：按 objectName 找，中英文界面下都成立。）
static QMenu *fileMenuOf(MainWindow *w)
{
    return w->findChild<QMenu *>(QStringLiteral("menuFile"));
}

static void popupMenu(QMenu *m)
{
    if (!m)
        return;
    m->popup(QPoint(0, 0));      // 弹出才会发 aboutToShow
    QApplication::processEvents();
}

// 用真实鼠标事件在画布上拖出一个矩形选区（走 CanvasView 的事件分发，
// 与用户手拖完全同一条路径）。
static void dragSelection(CanvasView *cv, const QRectF &imageRect)
{
    const QPointF p0 = cv->imageToWidget(imageRect.topLeft());
    const QPointF p1 = cv->imageToWidget(imageRect.bottomRight());

    auto send = [cv](QEvent::Type t, const QPointF &pos,
                     Qt::MouseButton btn, Qt::MouseButtons btns) {
        QMouseEvent e(t, pos, btn, btns, Qt::NoModifier);
        QApplication::sendEvent(cv, &e);
    };

    send(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 6; ++i) {
        const qreal f = qreal(i) / 6.0;
        send(QEvent::MouseMove, p0 + (p1 - p0) * f, Qt::NoButton, Qt::LeftButton);
    }
    send(QEvent::MouseButtonRelease, p1, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
}

// ===========================================================================
//  [1] 工具箱按钮 ⇄ ToolManager 的工具列表
// ===========================================================================

static void testToolBoxMatchesTools()
{
    beginGroup("工具箱按钮与工具列表一一对应");

    MainWindow *w = newWindow(QStringLiteral("move"));

    auto *tb = w->findChild<QToolBar *>(QStringLiteral("toolsToolBar"));
    check(tb != nullptr, "找得到左侧工具箱（toolsToolBar）");

    ToolManager *tm = toolsOf(w);
    check(tm != nullptr, "找得到 ToolManager");

    const QStringList ids = toolButtonIds(w);
    check(!ids.isEmpty(), "工具箱里至少有一个工具按钮");
    checkEqInt(ids.size(), tm ? tm->tools().size() : -1,
               "工具箱按钮数 == ToolManager 的工具数");

    // 每个工具都要能按 objectName 找到按钮 —— 这一条比"数量相等"更强：
    // 数量相等但两个 id 写错、彼此抵消的情况也能被抓出来。
    for (AbstractTool *t : tm->tools()) {
        if (!t)
            continue;
        const QString name = QStringLiteral("toolAction_") + t->id();
        check(w->findChild<QAction *>(name) != nullptr,
              qUtf8Printable(QStringLiteral("工具箱里有 %1 的按钮").arg(t->id())));
    }

    // 颜色指示器：它不是工具按钮，但必须还在（建工具箱时一并创建的）
    check(w->findChild<QToolButton *>(QStringLiteral("toolBoxColorIndicator")) != nullptr,
          "工具箱底部的颜色指示器存在");

    delete w;
}

// ===========================================================================
//  [2] 不变量：画布的活动工具 == ToolManager 的活动工具（且非空）
// ===========================================================================

static void testCanvasToolInvariant()
{
    beginGroup("画布的活动工具恒等于 ToolManager 的活动工具");

    MainWindow *w = newWindow(QStringLiteral("move"));
    CanvasView *cv = canvasOf(w);

    check(cv != nullptr, "找得到画布");
    check(canvasHasActiveTool(w), "ToolManager 有活动工具");
    check(cv && cv->activeTool() != nullptr,
          "画布有活动工具（为 nullptr 时画布上任何鼠标操作都不会被派发）");
    check(cv && cv->activeTool() == activeToolOf(w),
          "画布的活动工具 == ToolManager 的活动工具");

    // 点工具箱按钮切工具 —— 画布必须跟着换（这是用户的真实操作路径）
    if (QAction *brushBtn = w->findChild<QAction *>(QStringLiteral("toolAction_brush"))) {
        brushBtn->trigger();
        QApplication::processEvents();
        checkStr(activeToolId(w), QStringLiteral("brush"),
                 "点画笔按钮后 ToolManager 的活动工具是 brush");
        check(cv && cv->activeTool() && cv->activeTool()->id() == QLatin1String("brush"),
              "点画笔按钮后画布拿到的是画笔");
    } else {
        check(false, "找得到画笔按钮（toolAction_brush）");
    }

    // 再点另一个：两个方向的切换都要同步
    if (QAction *cropBtn = w->findChild<QAction *>(QStringLiteral("toolAction_crop"))) {
        cropBtn->trigger();
        QApplication::processEvents();
        checkStr(activeToolId(w), QStringLiteral("crop"),
                 "点裁剪按钮后 ToolManager 的活动工具是 crop");
        check(cv && cv->activeTool() && cv->activeTool()->id() == QLatin1String("crop"),
              "点裁剪按钮后画布拿到的是裁剪");
    } else {
        check(false, "找得到裁剪按钮（toolAction_crop）");
    }

    delete w;
}

// ===========================================================================
//  [3] 恢复"上次使用的工具"必须一路推到画布与按钮
// ===========================================================================

static void testRestoreLastActiveTool()
{
    beginGroup("恢复上次使用的工具要驱动到画布与工具箱按钮");

    // ★ 三个不同的值，覆盖三条不同的路径 ★
    //   move   —— 恰好是第一个注册的工具（ToolManager 的默认值，
    //             最容易因为"setActiveTool 早退"而漏掉同步）
    //   crop   —— 最后注册的工具
    //   brush  —— 中间的工具
    //   只试其中一个的话，"恢复逻辑只对某个位置生效"这种 bug 会漏网。
    const char *ids[] = { "move", "brush", "crop" };

    for (const char *id : ids) {
        const QString want = QLatin1String(id);
        MainWindow *w = newWindow(want);
        CanvasView *cv = canvasOf(w);

        const QString message = QStringLiteral("lastActive=%1 → ").arg(want);

        checkStr(activeToolId(w), want,
                 qUtf8Printable(message + QStringLiteral("ToolManager 的活动工具")));
        check(cv && cv->activeTool() != nullptr,
              qUtf8Printable(message + QStringLiteral("画布拿到活动工具（非 nullptr）")));
        check(cv && cv->activeTool() && cv->activeTool()->id() == want,
              qUtf8Printable(message + QStringLiteral("画布拿到的是同一个工具")));

        // 工具箱里对应的按钮要是选中态，其余不是
        auto *btn = w->findChild<QAction *>(QStringLiteral("toolAction_") + want);
        check(btn && btn->isChecked(),
              qUtf8Printable(message + QStringLiteral("对应按钮处于选中态")));

        int checkedCount = 0;
        for (AbstractTool *t : toolsOf(w)->tools()) {
            if (!t)
                continue;
            auto *b = w->findChild<QAction *>(QStringLiteral("toolAction_") + t->id());
            if (b && b->isChecked())
                ++checkedCount;
        }
        checkEqInt(checkedCount, 1, "有且只有一个工具按钮被选中");

        delete w;
    }
}

// ===========================================================================
//  [4] 换文档之后工具同步不能被打破
// ===========================================================================

static void testToolInvariantSurvivesNewDocument()
{
    beginGroup("新建文档之后工具同步仍然成立");

    MainWindow *w = newWindow(QStringLiteral("brush"));
    CanvasView *cv = canvasOf(w);

    if (QAction *newAct = w->findChild<QAction *>(QStringLiteral("actionNew"))) {
        newAct->trigger();
        QApplication::processEvents();

        check(cv && cv->document() != nullptr, "新建之后画布上有文档");
        check(cv && cv->activeTool() != nullptr,
              "新建之后画布仍然有活动工具");
        check(cv && cv->activeTool() == activeToolOf(w),
              "新建之后画布的活动工具仍与 ToolManager 一致");
        checkStr(activeToolId(w), QStringLiteral("brush"),
                 "新建文档不会改变当前工具");
    } else {
        check(false, "找得到「新建」动作（actionNew）");
    }

    delete w;
}

// ===========================================================================
//  [5] 「图像 → 裁剪」的可用性规则
// ===========================================================================

static void testCropMenuEnabledState()
{
    beginGroup("图像 → 裁剪 的置灰规则");

    MainWindow *w = newWindow(QStringLiteral("rectselect"));
    CanvasView *cv = canvasOf(w);

    auto *cropAct = w->findChild<QAction *>(QStringLiteral("actionCrop"));
    check(cropAct != nullptr, "菜单里有「裁剪」动作（actionCrop）");

    QMenu *imageMenu = imageMenuOf(w);
    check(imageMenu != nullptr, "找得到「图像」菜单");

    // ── 没有选区 → 置灰 ──
    w->findChild<QAction *>(QStringLiteral("actionDeselect"))->trigger();
    QApplication::processEvents();

    popupMenu(imageMenu);
    check(cropAct && !cropAct->isEnabled(),
          "没有选区时「裁剪」置灰（规格：裁到选区包围盒，见设计文档 6.8）");
    if (imageMenu)
        imageMenu->hide();

    // ── 拖出选区 → 可用 ──
    const QSize cs = cv->document()->canvasSize();
    dragSelection(cv, QRectF(cs.width()  * 0.2, cs.height() * 0.2,
                             cs.width()  * 0.5, cs.height() * 0.5));

    check(cv->document()->selection().isActive(),
          "拖出的矩形让选区进入活动状态");

    popupMenu(imageMenu);
    check(cropAct && cropAct->isEnabled(),
          "有选区时「裁剪」可用");
    if (imageMenu)
        imageMenu->hide();

    // ── 取消选择 → 回到置灰 ──
    w->findChild<QAction *>(QStringLiteral("actionDeselect"))->trigger();
    QApplication::processEvents();

    popupMenu(imageMenu);
    check(cropAct && !cropAct->isEnabled(), "取消选择后「裁剪」重新置灰");
    if (imageMenu)
        imageMenu->hide();

    delete w;
}

// ===========================================================================
//  [6] 执行「图像 → 裁剪」：真的把画布收成选区包围盒
// ===========================================================================

static void testCropToSelection()
{
    beginGroup("执行「图像 → 裁剪」把画布收成选区包围盒");

    MainWindow *w = newWindow(QStringLiteral("rectselect"));
    CanvasView *cv = canvasOf(w);

    const QSize before = cv->document()->canvasSize();

    // 选一块 40% × 40%，放在 (10%, 20%)
    const QRectF sel(before.width()  * 0.10, before.height() * 0.20,
                     before.width()  * 0.40, before.height() * 0.40);
    dragSelection(cv, sel);
    check(cv->document()->selection().isActive(), "选区已建立");

    const QRect bounds = cv->document()->selection().bounds();

    w->findChild<QAction *>(QStringLiteral("actionCrop"))->trigger();
    QApplication::processEvents();

    const QSize after = cv->document()->canvasSize();
    checkEqInt(after.width(),  bounds.width(),  "裁剪后画布宽度 == 选区包围盒宽度");
    checkEqInt(after.height(), bounds.height(), "裁剪后画布高度 == 选区包围盒高度");

    // ★ 尺寸变了不算完 —— 各图层的位图也必须跟着收 ★
    //   项目的隐含不变量是"图层位图尺寸恒等于画布尺寸"，
    //   漏掉平移/裁剪这一步会让画面错位，而尺寸断言看不出来。
    Document *doc = cv->document();
    checkEqInt(doc->activeLayer()->image().width(),  after.width(),
               "活动图层位图宽度 == 裁剪后的画布宽度");
    checkEqInt(doc->activeLayer()->image().height(), after.height(),
               "活动图层位图高度 == 裁剪后的画布高度");

    delete w;
}

// ---------------------------------------------------------------------------
//  [7] 图标：工具箱按钮与菜单项都必须真的有图标
// ---------------------------------------------------------------------------
//  ★ 这一组盯的是"静默变成空白"的那一类失效 ★
//    图标名写错一个字母 → icons::icon() 返回空 QIcon → 按钮上是空白；
//    而 buildToolBox 还有"字母占位图"兜底，所以连"到底有没有配图标"
//    都看不出来（有个丑字母总比空白强，但用户看到的是"移/画/橡"这种占位图）。
//    这里用 cacheKey 做**同一性**比较：
//      工具箱动作上的图标，必须就是 iconForTool(id) 那一张，
//      而不是兜底的字母占位图。
//    （QIcon 隐式共享，cacheKey 相同 ⇔ 指向同一份内容。）
static void collectActions(QMenu *menu, QList<QAction *> *out)
{
    if (!menu)
        return;
    for (QAction *a : menu->actions()) {
        if (!a || a->isSeparator())
            continue;
        if (a->menu()) {                 // 子菜单本身不挂图标，往下递归
            collectActions(a->menu(), out);
            continue;
        }
        out->append(a);
    }
}

static void testIcons()
{
    beginGroup("图标：工具箱与菜单");

    MainWindow *w = newWindow(QStringLiteral("move"));

    auto *tb = w->findChild<QToolBar *>(QStringLiteral("toolsToolBar"));
    check(tb != nullptr, "找得到工具箱");

    // ── 工具箱：每个工具都用"专用图标"，不是字母占位图 ──
    QStringList noDedicated, mismatched;
    if (tb) {
        for (QAction *a : tb->actions()) {
            if (!a || a->isSeparator())
                continue;
            const QString name = a->objectName();
            if (!name.startsWith(QLatin1String("toolAction_")))
                continue;                // 颜色指示器等不是工具按钮
            const QString id = name.mid(QStringLiteral("toolAction_").size());

            const QIcon dedicated = icons::iconForTool(id);
            if (dedicated.isNull()) {
                noDedicated << id;       // 没配图标 → 界面上是字母占位图
                continue;
            }
            if (a->icon().isNull() || a->icon().cacheKey() != dedicated.cacheKey())
                mismatched << id;
        }
    }
    check(noDedicated.isEmpty(), "每个工具都有专用 SVG 图标（不是字母占位图）");
    if (!noDedicated.isEmpty())
        std::printf("       缺图标：%s\n", qUtf8Printable(noDedicated.join(", ")));
    check(mismatched.isEmpty(), "工具箱按钮上的图标就是该工具的专用图标");
    if (!mismatched.isEmpty())
        std::printf("       对不上：%s\n", qUtf8Printable(mismatched.join(", ")));

    // ── 菜单：每个有名字的动作都要有图标 ──
    //    只查"有 objectName"的：自动化与对账脚本都靠它定位，
    //    动态生成的历史文件项之类没有名字，也本来就不该有图标。
    QList<QAction *> menuActions;
    for (QAction *top : w->menuBar()->actions()) {
        if (top->menu())
            collectActions(top->menu(), &menuActions);
    }
    check(!menuActions.isEmpty(), "菜单里收集到了动作");

    QStringList iconless;
    int named = 0;
    for (QAction *a : menuActions) {
        if (a->objectName().isEmpty())
            continue;
        ++named;
        if (a->icon().isNull())
            iconless << a->objectName();
    }
    check(named >= 40, "有名字的菜单动作足够多（菜单确实建起来了）");
    check(iconless.isEmpty(), "每个有名字的菜单动作都有图标");
    if (!iconless.isEmpty())
        std::printf("       缺图标：%s\n", qUtf8Printable(iconless.join(", ")));
    std::printf("       菜单动作 %d 个（有名字的 %d 个）\n",
                int(menuActions.size()), named);

    // 反向：图标库对不存在的名字必须返回空 —— 这是"响亮失败"那条路。
    check(icons::icon(QStringLiteral("no_such_icon_zzz")).isNull(),
          "不存在的图标名返回空 QIcon");

    delete w;
}

// ===========================================================================
//  [7a-1b] 图标跟随主题
// ===========================================================================
//
//  ★ 这一组为什么必须存在 ★
//    图标是在**渲染时刻**按 palette 上色的（深色主题下自动变浅）。
//    它带来两个只有真跑才看得见的失效：
//      ① 换主题后不重设 → 图标永远停在旧颜色（用户看到"切了深色，图标还是黑的"）；
//      ② 重设了但没清缓存 → 取回来的还是旧色缓存项，等于没重设。
//    两条都不会报错、不会崩，界面上只是"看不清"。任何静态检查都发现不了。
//
//  ★ 探针是像素，不是 cacheKey ★
//    "图标换了个 QIcon 对象"不等于"颜色变了"。直接读一个不透明像素的 RGB。
static QRgb sampleOpaqueInk(const QIcon &ic)
{
    const QImage img = ic.pixmap(QSize(24, 24)).toImage()
                           .convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) == 255)
                return img.pixel(x, y);
    return 0;
}

static void testIconsFollowTheme()
{
    beginGroup("图标跟随主题（上色 + 换主题重设）");

    MainWindow *w = newWindow(QStringLiteral("move"));
    QAction *a = w->findChild<QAction *>(QStringLiteral("actionNew"));
    check(a != nullptr, "找得到菜单动作");
    if (!a) {
        delete w;
        return;
    }
    // 必须有图标名（否则换主题根本不知道该重设成什么）
    check(a->property("iconName").toString() == QStringLiteral("file_new"),
          "动作上记着自己用的图标名");

    const QPalette original = QApplication::palette();

    QPalette light = original;
    light.setColor(QPalette::WindowText, QColor(0x33, 0x39, 0x3F));
    QApplication::setPalette(light);
    QApplication::processEvents();
    const QRgb onLight = sampleOpaqueInk(a->icon());

    QPalette dark = original;
    dark.setColor(QPalette::WindowText, QColor(0xE6, 0xE6, 0xE6));
    QApplication::setPalette(dark);
    QApplication::processEvents();
    const QRgb onDark = sampleOpaqueInk(a->icon());

    check(qRed(onLight) < 0x80, "浅色主题下图标是深色");
    check(qRed(onDark) > 0xC0, "切到深色主题后图标自动变浅");
    if (!(qRed(onLight) < 0x80) || !(qRed(onDark) > 0xC0))
        std::printf("       实测 浅=%08x 深=%08x\n", onLight, onDark);

    // 切回去也要对（不是单向的）
    QApplication::setPalette(light);
    QApplication::processEvents();
    check(qRed(sampleOpaqueInk(a->icon())) < 0x80, "切回浅色主题图标又变回深色");

    // ★ 颜色没变时不该重设 ★
    //   PaletteChange 在拖动窗口、切焦点时也会来，无脑重设 74 个动作是白干。
    const qint64 before = a->icon().cacheKey();
    QPalette other = light;
    other.setColor(QPalette::Window, QColor(0x11, 0x22, 0x33));
    QApplication::setPalette(other);
    QApplication::processEvents();
    check(a->icon().cacheKey() == before, "只改无关颜色时不重设图标");

    QApplication::setPalette(original);
    QApplication::processEvents();
    delete w;
}

// ===========================================================================
//  [7a-2] 应用名称与图标
// ===========================================================================
//
//  ★ 这一组盯的是"两类静默失效" ★
//
//    ① **显示名改了、某处没跟着改**
//       程序名以前是散落的字面量。加中文名时如果只改了标题、忘了"关于"框，
//       没有任何报错 —— 用户会在菜单里看到"关于 逐鹿 图片编辑器"、
//       点开却是"ZlPhotoEdit"。所以这里不写死期望值，而是断言
//       **所有显示位置与 AppInfo 的一致性**。
//
//    ② **图标静默变成空白**
//       qrc 里少一行、SVG 解析失败（比如带 DOCTYPE）、路径少一个斜杠，
//       全都表现为"没图标"，编译器与运行期都不报错。
//       唯一抓得住的办法是**真的把图上渲染出来、量它的像素**。
//       只断言 !icon.isNull() 还不够 —— QIcon 只要有 pixmap 就不算 null，
//       而那张 pixmap 可能是全透明的。
//
//    另有一条"身份不能混"的断言：QCoreApplication::applicationName()
//    是 **QSettings 的存储路径**，永远必须是英文的工程名。
//    万一有人图省事把它改成中文，用户的全部设置就"消失"了（其实是换了路径）。

/// 数一张位图里非透明像素的个数。用来判"图是真的画出来了"。
static int opaquePixelCount(const QPixmap &pm)
{
    if (pm.isNull())
        return 0;

    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(row[x]) > 8)      // >8：容忍 SVG 抗锯齿边缘的极淡像素
                ++n;
        }
    }
    return n;
}

static void testAppIdentityAndIcon()
{
    beginGroup("应用身份：显示名与图标");

    // ── ① 名字：中文名是给人看的，英文名是存储路径 ──
    check(!appinfo::displayNameCn().isEmpty(), "中文显示名非空");
    check(appinfo::displayNameCn().contains(QStringLiteral("逐鹿")),
          "★ 中文名就是用户定的那个（含「逐鹿」）");
    check(appinfo::displayNameEn() == QStringLiteral("ZlPhotoEdit"),
          "英文显示名 = 工程名 ZlPhotoEdit");

    // ★★ 存储身份必须是英文 ★★
    //   它决定 QSettings 落在注册表的哪个键下。改成中文 ≠ 改名，
    //   而是"用户设置全部丢失"。
    //
    //   ★ 为什么这里不能断言 QCoreApplication::applicationName() ★
    //     main() 为了解决"测试绝不写用户注册表"，在 redirectSettingsToTempDir()
    //     里把 applicationName 改成了 "MainWindowAssemblyTest"。所以那一刻
    //     起，QCoreApplication 上挂的就是测试自己的名字，与真实程序无关。
    //     硬要比对只会得到一条永远红的断言（而且会诱使人"把测试的名字改成
    //     程序名" —— 那才是真的把隔离破坏掉）。
    //
    //   正确的判据是**源头**：AppInfo 里的英文名常量。它同时也是
    //   main.cpp 的 kApplicationName 的镜像 —— 两者的同步由下面这条
    //   "displayNameEn() 是纯 ASCII 的 ZlPhotoEdit" 加上
    //   testIconsOfExeIcon 里的源码对账一起守住。
    checkStr(appinfo::displayNameEn(), QStringLiteral("ZlPhotoEdit"),
             "★★ QSettings 用的应用名（AppInfo 的英文名）仍是英文工程名");

    // ★ 真实程序的身份（未被测试改写时）——只在没被重定向时才成立 ★
    //   用 organizationName 判断"是不是已经被重定向过了"。
    if (QCoreApplication::organizationName() == QStringLiteral("ZlStudio")) {
        checkStr(QCoreApplication::applicationName(), QStringLiteral("ZlPhotoEdit"),
                 "★★ QSettings 的应用名 = 英文工程名（真实运行时的身份）");
        checkStr(QCoreApplication::organizationName(), QStringLiteral("ZlStudio"),
                 "★★ 组织名仍是 ZlStudio");
        check(appinfo::displayNameEn() == QCoreApplication::applicationName(),
              "★ AppInfo 的英文名与 QSettings 的应用名一致（不会各说各话）");
    } else {
        std::printf("       （QSettings 已被隔离重定向，跳过对 QCoreApplication 的直接比对）\n");
    }

    // 后缀：两个名字不同才拼括号，且必须两个都在
    //   ★ 必须先强制界面语言，不能信"自动" ★
    //     titleSuffix() 跟着 currentLanguage() 走，而"自动"按系统 locale
    //     推断 —— 开发机的 Git Bash 里 LANG=en_US.UTF-8（实测 Qt 真的会读
    //     这个环境变量：带上是 en_US、清掉是 zh_CN），CI 机器更不可控。
    //     不强制的话，这条断言会在中文系统上绿、英文系统上红，是典型的
    //     "飘移断言"（源码英文化前它被"locale 猜不中就回落中文源码"兜底
    //     掩盖着，kSourceLanguage 翻成 en 之后才现形）。
    //     两种界面语言各验一遍，验完清键恢复"自动"。
    {
        QSettings s;
        s.remove(QStringLiteral("ui/language"));
        s.sync();
    }
    uilang::setCurrentLanguage(QStringLiteral("zh_CN"));
    const QString suffixCn = appinfo::titleSuffix();
    check(suffixCn.contains(appinfo::displayNameCn()), "中文界面：标题后缀含中文名");
    check(suffixCn.contains(appinfo::displayNameEn()), "中文界面：后缀同时带英文名（括号里）");

    uilang::setCurrentLanguage(QStringLiteral("en"));
    checkStr(appinfo::titleSuffix(), appinfo::displayNameEn(),
             "英文界面：标题只放英文名（不对英文用户拼中文）");

    {
        QSettings s;
        s.remove(QStringLiteral("ui/language"));
        s.sync();
    }

    // 标题拼法：不绑定具体语言，只要求"文件名 / 缩放 / 后缀"的拼接自洽
    const QString suffix = appinfo::titleSuffix();
    check(!suffix.contains(QStringLiteral("(%1)").arg(appinfo::displayNameEn())
                               + QStringLiteral(" (%1)").arg(appinfo::displayNameEn())),
          "★ 后缀没有把同一个名字拼两遍");

    // 标题拼法
    checkStr(appinfo::windowTitleFor(QString(), QString()), suffix,
             "无文件名时标题就是后缀本身");
    checkStr(appinfo::windowTitleFor(QStringLiteral("a.png"), QStringLiteral("50%")),
             QStringLiteral("a.png [50%] - %1").arg(suffix),
             "★ 有文件名 + 缩放比时的拼法");
    checkStr(appinfo::windowTitleFor(QStringLiteral("a.png"), QString()),
             QStringLiteral("a.png - %1").arg(suffix),
             "没有缩放比时不留空的方括号");

    // ── ② 图标：窗口图标 ──
    //   ★ 这里必须量像素 ★
    //     只断言 !isNull() 会漏掉"有 QIcon 但 pixmap 全透明"这一类，
    //     而那恰好是 PNG 解码失败但资源路径存在时的表现。
    const QIcon appIcon = appinfo::windowIcon();
    check(!appIcon.isNull(), "★ 应用程序图标不是空 QIcon（logo.png 解码成功）");

    bool allNull = true;
    for (int px : {16, 32, 48, 128}) {
        if (!appIcon.pixmap(px, px).isNull())
            allNull = false;
    }
    check(!allNull, "★ 图标的各档位图都能取出来");

    // 逐档量像素：每一档都要"画出了东西"
    QStringList emptySizes;
    for (int px : {16, 20, 24, 32, 48, 64, 128, 256}) {
        const int n = opaquePixelCount(appinfo::windowIconPixmap(px));
        if (n == 0)
            emptySizes << QString::number(px);
    }
    check(emptySizes.isEmpty(),
          "★★ 每一档图标都真的画出了像素（不是一整张透明图）");
    if (!emptySizes.isEmpty())
        std::printf("       全透明的档位：%s\n", qUtf8Printable(emptySizes.join(", ")));

    // ── 图标：几何合理性（防止"画出来了，但是一团糊"）──
    //
    //  ★ 为什么不能断言"覆盖率必须小于某个值" ★
    //    这个 logo 是一张**圆角方底 + 插画**的图 —— 方底几乎铺满整个
    //    画面，16px 下覆盖率天然接近 1（旧版相机 logo 实测 0.98）。硬写一个
    //    "覆盖率 < 0.9" 之类的阈值，是在用另一张图的设计去约束这张图，
    //    改天换 logo 就会无理由变红。
    //
    //    真正该钉的是**"不是简单的一整块纯色"** —— 也就是"画上去了细节"。
    //    做法：数一下有多少种不同的颜色。纯色块只有 1 种；
    //    这张图有橙鹿、蓝天、绿树、黄太阳、棕画架 —— 至少好几种。
    {
        const QPixmap pm32 = appinfo::windowIconPixmap(32);
        QSet<QRgb> colors;
        if (!pm32.isNull()) {
            const QImage img = pm32.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height(); ++y) {
                const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(row[x]) > 8)
                        colors.insert(row[x]);
                }
            }
        }
        check(colors.size() >= 8,
              "★★ 32px 图标有足够多的颜色（不是一整块纯色 —— 说明细节真的画上了）");
        std::printf("       32px 不同颜色数 %d\n", int(colors.size()));
    }

    // 同一档重复渲染必须稳定（同源缩放是确定性的；若不稳定说明
    // 渲染路径里有不该有的状态 —— 比如缓存串了名字）
    {
        const QPixmap a = appinfo::windowIconPixmap(48);
        const QPixmap b = appinfo::windowIconPixmap(48);
        check(a.toImage() == b.toImage(), "同一档重复渲染逐像素一致（渲染无隐藏状态）");
    }

    // ── ③ 图标：exe 资源 ──
    //   运行时读不到 PE 资源段里那个图标（Windows 不通过 API 暴露给我们），
    //   但能确认"源文件在、且构建时确实接进来了"。
    //   源文件缺失 = 构建配置写错了（.rc 指向了一个不存在的文件）。
    const QString icoPath = appinfo::exeIconSourcePath();
    check(QFileInfo::exists(icoPath),
          "★ exe 图标源文件 resources/logo.ico 存在");
    if (!QFileInfo::exists(icoPath))
        std::printf("       找不到：%s\n", qUtf8Printable(icoPath));
    check(appinfo::hasExeIcon(),
          "★ 当前构建已接入 Windows 资源图标（.rc 编进去了，不是白底窗户）");

    // .rc 本身也要在 —— 它才是"接进去"这件事的实体
    {
        QDir d(QFileInfo(icoPath).absolutePath());     // <repo>/resources
        check(d.exists(QStringLiteral("app.rc")),
              "★ resources/app.rc 存在（exe 图标的载体）");

        // ★★ 源码级对账：英文名与 main.cpp 的 kApplicationName 必须一致 ★★
        //
        //  为什么在这里做（而不是断言 QCoreApplication::applicationName()）
        //    见上面 ① 段的说明：测试把 QCoreApplication 的身份改掉了，
        //    运行时读不到真实值。而这两个常量一旦不同步，后果是
        //    "关于框里写 ZlPhotoEdit、注册表路径却是别的" —— 不致命，
        //    但会让排查问题的人抓狂。
        //    编译器不管这件事（两个各自合法的字符串），所以只能对账源码。
        //
        //  ★ 解析要认准"定义那一行" ★
        //    "kApplicationName" 这个词在 main.cpp 的**注释里也出现**（就在定义
        //    上方几行，讲中文名不能写进这里）。简单粗暴地"找第一个 kApplicationName
        //    再往右找第一个引号"，会一路走到上一条定义
        //    `kOrganizationName = "ZlStudio"` 的引号上 —— 实测正是这么错的，
        //    于是断言报 "期望 ZlPhotoEdit，实际 ZlStudio"。
        //    所以用**正则匹配定义行**：必须同时含 `kApplicationName`、`=`、双引号。
        {
            const QString mainCpp =
                QDir(QFileInfo(icoPath).absolutePath()).absoluteFilePath(
                    QStringLiteral("../src/main.cpp"));
            QFile f(mainCpp);
            bool ok = false;
            QString actual;
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString text = QString::fromUtf8(f.readAll());

                // 逐行找：跳过注释行，只认"代码行里的 kApplicationName = "...""
                const QStringList lines = text.split(QLatin1Char('\n'));
                for (const QString &ln : lines) {
                    const QString trimmed = ln.trimmed();
                    // 注释行整行跳过（kApplicationName 在注释里出现过两次）
                    if (trimmed.startsWith(QStringLiteral("//"))
                        || trimmed.startsWith(QLatin1Char('*')))
                        continue;
                    const int at = ln.indexOf(QStringLiteral("kApplicationName"));
                    if (at < 0)
                        continue;
                    const int q1 = ln.indexOf(QLatin1Char('"'), at);
                    const int q2 = (q1 >= 0) ? ln.indexOf(QLatin1Char('"'), q1 + 1) : -1;
                    if (q2 > q1) {
                        actual = ln.mid(q1 + 1, q2 - q1 - 1);
                        ok = true;
                        break;
                    }
                }
            }
            check(ok, "★ 能读到 src/main.cpp 并解析出 kApplicationName 的定义值");
            if (ok) {
                checkStr(actual, appinfo::displayNameEn(),
                         "★★ main.cpp 的 kApplicationName == AppInfo 的英文名"
                         "（不同步会让「关于」框与注册表路径各说各话）");
            } else {
                std::printf("       解析失败，路径：%s\n", qUtf8Printable(mainCpp));
            }
        }
    }

    delete newWindow(QStringLiteral("move"));   // 顺带把窗口建出来确保无副作用
    // ★ 上面这一行的建窗只为确认"读取 AppInfo 不影响建窗"；
    //   真正的窗口标题一致性断言在 testDocumentTabs 的 ① 段。
}

// ===========================================================================
//  [7b] 图层面板：装配与接线
// ===========================================================================
//
//  这一组盯的是**跨模块的方向一致性** —— 图层面板、图层菜单、Document
//  三方对"索引 0 在哪一头""向下合并往哪边合"必须持同一套约定。
//  实测踩过：Document::mergeLayerDown 的合并方向是反的（把**上面**那层
//  合成到当前层上），而 MainWindow 的守卫恰好也是"最底部的图层不能合并"，
//  两处错误各自看起来都合理，于是谁都没报错 —— 只有"合并完选中了
//  旁边那个图层"这一个症状。所以这里不看代码，只看**结果落在哪一行**。

static LayerPanel *layerPanelOf(MainWindow *w)
{
    return w->findChild<LayerPanel *>(QStringLiteral("layerPanel"));
}

/// 按工具提示的前缀找面板上的按钮。
/// 用具名而不是"第几个"：按钮的创建顺序是内部细节，而工具提示是给用户看的，
/// 改顺序不该让测试红，改功能才该。
static QToolButton *buttonWithTip(LayerPanel *panel, const QString &prefix)
{
    if (!panel)
        return nullptr;
    const QList<QToolButton *> btns = panel->buttons();
    for (QToolButton *b : btns) {
        if (b && b->toolTip().startsWith(prefix))
            return b;
    }
    return nullptr;
}

static void testLayerPanelAssembly()
{
    beginGroup("图层面板：装配与接线");

    MainWindow *w = newWindow(QStringLiteral("brush"));
    CanvasView *cv = canvasOf(w);
    Document *doc = cv ? cv->document() : nullptr;
    check(doc != nullptr, "拿得到当前文档");

    LayerPanel *panel = layerPanelOf(w);
    check(panel != nullptr,
          "主窗口里挂着真正的 LayerPanel（不再是 Phase 1 的裸 QListWidget）");
    if (!panel || !doc) {
        delete w;
        return;
    }

    QListWidget *list = panel->listWidget();
    check(list != nullptr, "面板里有列表控件");

    // ── 面板与文档同步 ──
    doc->addLayer(QStringLiteral("第二层"));
    doc->addLayer(QStringLiteral("第三层"));
    QApplication::processEvents();

    checkEqInt(list->count(), doc->layerCount(), "面板行数恒等于图层数");
    checkStr(list->item(0)->text(), QStringLiteral("第三层"),
              "第 0 行是最上面那个图层（行序是倒的）");
    checkEqInt(list->item(0)->data(Qt::UserRole).toInt(), doc->layerCount() - 1,
               "第 0 行对应的文档索引是最顶层那个");

    // ── 按钮行：功能齐全 + 每个都有图标 ──
    const QList<QToolButton *> btns = panel->buttons();
    check(btns.size() >= 8, "按钮行至少有 8 个按钮");

    bool allIcons = true;
    QStringList missing;
    for (QToolButton *b : btns) {
        if (!b || b->icon().isNull()) {
            allIcons = false;
            missing << (b ? b->toolTip() : QStringLiteral("<null>"));
        }
    }
    if (!allIcons)
        std::printf("       缺图标的按钮：%s\n",
                    qUtf8Printable(missing.join(QStringLiteral(" / "))));
    check(allIcons, "每个按钮都有图标（不是字母兜底）");

    check(buttonWithTip(panel, QStringLiteral("New Layer")) != nullptr,
          "有「新建图层」按钮");
    check(buttonWithTip(panel, QStringLiteral("Duplicate Layer")) != nullptr,
          "有「复制图层」按钮");
    check(buttonWithTip(panel, QStringLiteral("Delete")) != nullptr,
          "有「删除图层」按钮");
    check(buttonWithTip(panel, QStringLiteral("Move Layer Up")) != nullptr,
          "有「上移图层」按钮");
    check(buttonWithTip(panel, QStringLiteral("Move Layer Down")) != nullptr,
          "有「下移图层」按钮");
    check(buttonWithTip(panel, QStringLiteral("Merge Down")) != nullptr,
          "有「向下合并」按钮");
    check(buttonWithTip(panel, QStringLiteral("Merge Visible Layers")) != nullptr,
          "有「合并可见图层」按钮");
    check(buttonWithTip(panel, QStringLiteral("Flatten Image")) != nullptr,
          "有「拼合图像」按钮");

    // ── 属性控件挂在面板上 ──
    check(panel->blendCombo() != nullptr, "面板上有混合模式下拉框");
    check(panel->opacitySlider() != nullptr, "面板上有不透明度滑块");

    // ── 图层菜单补齐了两个新动作 ──
    auto *mergeVisible = w->findChild<QAction *>(QStringLiteral("actionLayerMergeVisible"));
    auto *renameLayer  = w->findChild<QAction *>(QStringLiteral("actionLayerRename"));
    check(mergeVisible != nullptr, "菜单里有「合并可见图层」（actionLayerMergeVisible）");
    check(renameLayer != nullptr, "菜单里有「重命名图层」（actionLayerRename）");
    if (mergeVisible) {
        check(!mergeVisible->icon().isNull(), "「合并可见图层」有图标");
        check(!mergeVisible->shortcut().isEmpty(), "「合并可见图层」有快捷键");
    }
    if (renameLayer) {
        check(!renameLayer->icon().isNull(), "「重命名图层」有图标");
        check(renameLayer->shortcut() == QKeySequence(Qt::Key_F2),
              "「重命名图层」的快捷键是 F2");
    }

    // ══════════════════════════════════════════════════════════════════════
    //  ★ 向下合并的方向 ★
    // ══════════════════════════════════════════════════════════════════════
    //
    //  三层，活动层 = 索引 1（中间那层）。
    //  "向下合并"应该把它合进索引 0，结果留在索引 0。
    //  索引 0 是**最底**那一头 —— 这就是本组最容易搞反的地方。
    doc->setActiveLayerIndex(1);
    doc->layers().mutableAll()[0].setName(QStringLiteral("下面那层"));
    doc->layers().mutableAll()[1].setName(QStringLiteral("要被合掉的"));
    doc->layers().mutableAll()[2].setName(QStringLiteral("最上面那层"));
    emit doc->structureChanged();
    QApplication::processEvents();

    const int countBefore = doc->layerCount();

    auto *mergeDown = w->findChild<QAction *>(QStringLiteral("actionMergeDown"));
    check(mergeDown != nullptr, "菜单里有「向下合并」（actionMergeDown）");
    if (mergeDown) {
        mergeDown->trigger();
        QApplication::processEvents();

        checkEqInt(doc->layerCount(), countBefore - 1, "向下合并后图层数减一");
        checkStr(doc->layers().layer(0)->name(), QStringLiteral("下面那层"),
                 "★ 合并结果落在**下面那层**的位置上，并继承它的名字");
        checkEqInt(doc->activeLayerIndex(), 0,
                   "★ 合并后的活动图层是索引 0（结果所在的位置，不是旁边那个）");
        if (doc->layerCount() >= 2) {
            checkStr(doc->layers().layer(1)->name(), QStringLiteral("最上面那层"),
                     "原本在顶上的图层没有被动过（它不该被卷进这次合并）");
        }

        // ── 撤销要能整条退回去 ──
        auto *undo = w->findChild<QAction *>(QStringLiteral("actionUndo"));
        check(undo != nullptr, "菜单里有「撤销」（actionUndo）");
        if (undo) {
            undo->trigger();
            QApplication::processEvents();
            checkEqInt(doc->layerCount(), countBefore, "撤销一次恢复图层数");
            checkStr(doc->layers().layer(1)->name(), QStringLiteral("要被合掉的"),
                     "撤销把被合掉的那一层原样放回来了");
            checkStr(doc->layers().layer(0)->name(), QStringLiteral("下面那层"),
                     "撤销恢复了下层的名字");
        }
    }

    // ── 最底那层不能向下合并（下面是空的） ──
    doc->setActiveLayerIndex(0);
    const int countBottom = doc->layerCount();
    if (mergeDown) {
        mergeDown->trigger();
        QApplication::processEvents();
        checkEqInt(doc->layerCount(), countBottom,
                   "★ 索引 0（最底那层）向下合并会被拒绝，图层数不变");
    }

    delete w;
}

static void testMergeVisibleKeepsHiddenLayers()
{
    beginGroup("合并可见图层：隐藏图层不被吃掉，且合并结果落在原槽位");

    MainWindow *w = newWindow(QStringLiteral("brush"));
    CanvasView *cv = canvasOf(w);
    Document *doc = cv ? cv->document() : nullptr;
    if (!doc) {
        check(false, "拿得到当前文档");
        delete w;
        return;
    }

    doc->addLayer(QStringLiteral("可见中"));
    doc->addLayer(QStringLiteral("隐藏顶"));
    doc->layers().mutableAll()[0].setName(QStringLiteral("可见底"));
    doc->layers().mutableAll()[2].setVisible(false);   // ★ 隐藏层在**最上面**
    emit doc->structureChanged();
    QApplication::processEvents();
    doc->setActiveLayerIndex(0);

    auto *act = w->findChild<QAction *>(QStringLiteral("actionLayerMergeVisible"));
    if (!act) {
        check(false, "找得到「合并可见图层」动作");
        delete w;
        return;
    }
    act->trigger();
    QApplication::processEvents();

    checkEqInt(doc->layerCount(), 2,
               "两个可见层合成一个 + 隐藏层保留 = 2 层");
    checkStr(doc->layers().layer(0)->name(), QStringLiteral("Merged Layer"),
             "★ 合并结果落在原来「最上面那个可见层」的槽位上 —— "
             "也就是那个隐藏层的**下面**，而不是被 append 到栈顶");
    if (doc->layerCount() >= 2) {
        checkStr(doc->layers().layer(1)->name(), QStringLiteral("隐藏顶"),
                 "隐藏图层还在（Photoshop 的行为：合并可见图层不删除隐藏层）");
        check(!doc->layers().layer(1)->isVisible(), "保留的隐藏层依然是隐藏的");
    }
    checkEqInt(doc->activeLayerIndex(), 0, "活动图层落在合并结果上");

    // 撤销：隐藏层要连同它的隐藏状态一起回来
    w->findChild<QAction *>(QStringLiteral("actionUndo"))->trigger();
    QApplication::processEvents();
    checkEqInt(doc->layerCount(), 3, "撤销恢复成三层");
    if (doc->layerCount() >= 3) {
        checkStr(doc->layers().layer(2)->name(), QStringLiteral("隐藏顶"),
                 "撤销把隐藏层放回原来的位置（栈顶）");
        check(!doc->layers().layer(2)->isVisible(),
              "撤销连「隐藏」这个状态一起恢复了");
    }

    delete w;
}

static void testRenameActiveLayerAction()
{
    beginGroup("重命名图层（F2）作用在活动图层那一行");

    MainWindow *w = newWindow(QStringLiteral("brush"));
    CanvasView *cv = canvasOf(w);
    Document *doc = cv ? cv->document() : nullptr;
    LayerPanel *panel = layerPanelOf(w);
    if (!doc || !panel || !panel->listWidget()) {
        check(false, "拿得到文档与图层面板");
        delete w;
        return;
    }

    doc->addLayer(QStringLiteral("第二层"));
    doc->addLayer(QStringLiteral("第三层"));
    QApplication::processEvents();

    // 活动图层 = 索引 0（最底）→ 它在**最后一行**
    doc->setActiveLayerIndex(0);
    QApplication::processEvents();

    auto *act = w->findChild<QAction *>(QStringLiteral("actionLayerRename"));
    if (!act) {
        check(false, "找得到「重命名图层」动作");
        delete w;
        return;
    }
    act->trigger();
    QApplication::processEvents();

    QListWidget *list = panel->listWidget();
    checkEqInt(list->currentRow(), list->count() - 1,
               "★ F2 进入编辑的是活动图层那一行（最底那层在最后一行，"
               "不是第 0 行）");

    delete w;
}

// ---------------------------------------------------------------------------
//  「文件 → 打开为图层」
// ---------------------------------------------------------------------------
//  ★ 为什么这里只断言"装配"，不断言"点了会怎样" ★
//    openAsLayer() 第一件事就是弹模态的 QFileDialog，测试里点下去会挂在
//    对话框上。所以这里守的是**装配**：动作建出来了、有对象名、真的挂在
//    「文件」菜单里、有文档时可用、并且有真图标。
//
//    它的**决策逻辑**（画布扩到多大、图片放在哪里）已经被抽成
//    engine/Geometry 的 unionSize() / centeredOffset()，由 geometry_test
//    直接断言 —— 这正是"把决策抽成纯函数"的价值所在。留在 MainWindow 里
//    就只能靠人眼看截图了。
static void testOpenAsLayerAssembly()
{
    beginGroup("文件 → 打开为图层：装配");

    MainWindow *w = newWindow(QStringLiteral("brush"));

    auto *act = w->findChild<QAction *>(QStringLiteral("actionOpenAsLayer"));
    check(act != nullptr, "★ 建出了 actionOpenAsLayer");
    if (!act) {
        delete w;
        return;
    }

    check(!act->icon().isNull(),
          "★ 有真图标（不是字母兜底 —— 图标加载失败是静默的，必须断言）");
    check(act->isEnabled(), "有文档时可用");

    // ★ 必须单独断言"它在菜单里" ★
    //   addAction(nullptr) 是**合法**的，且不报错 —— 症状是菜单项凭空消失、
    //   0 错 0 警。所以"动作对象存在"完全证明不了"菜单里有它"。
    QMenu *fileMenu = fileMenuOf(w);
    check(fileMenu != nullptr, "拿得到「文件」菜单");

    auto *openAct = w->findChild<QAction *>(QStringLiteral("actionOpen"));
    check(openAct != nullptr, "拿得到「打开」动作");

    if (fileMenu) {
        check(fileMenu->actions().contains(act),
              "★ 它确实挂在「文件」菜单里（不是个没人引用的孤儿动作）");

        const int iOpen    = openAct ? fileMenu->actions().indexOf(openAct) : -1;
        const int iAsLayer = fileMenu->actions().indexOf(act);
        check(iOpen >= 0 && iAsLayer == iOpen + 1,
              "★ 排在「打开」的紧后面（两者成组，用户才好对比区别）");
    }

    // 与「打开」必须是两个独立动作：一个换文档、一个只加图层，
    // 破坏力差得远，合并成一个会让人分不清自己按下去会发生什么。
    check(openAct != nullptr && openAct != act,
          "与「打开」是两个独立动作");

    delete w;
}

// ---------------------------------------------------------------------------
//  「粘贴为新图层」：抽出 addImageAsNewLayer() 之后的回归
// ---------------------------------------------------------------------------
//  粘贴与「打开为图层」现在共用 addImageAsNewLayer()。重构之后必须确认
//  粘贴那条路**行为一点没变**，尤其是"新图层填满画布"这条不变量 ——
//  它成立与否决定 Document::addLayer 会不会踩到那条**静默改画布尺寸**的
//  分支（详见 MainWindowCommands.cpp 里 addImageAsNewLayer 的说明）。
static void testPasteAsNewLayerRegression()
{
    beginGroup("粘贴为新图层：一条撤销 + 新图层填满画布");

    MainWindow *w = newWindow(QStringLiteral("brush"));
    CanvasView *cv = canvasOf(w);
    Document *doc = cv ? cv->document() : nullptr;
    check(doc != nullptr, "拿得到当前文档");
    if (!doc) {
        delete w;
        return;
    }

    const QSize canvasBefore = doc->canvasSize();
    const int   layersBefore = doc->layerCount();

    // 只往剪贴板放一张 QImage，不放本程序的自定义格式 ——
    // 这样走的正是 pasteFromClipboard() 里"降级读标准图像格式"那条分支
    // （sourceBounds 为空、来源不明 → 按居中放置），
    // 而"居中放置"恰好也是「打开为图层」的放置规则，两边共用同一套。
    QImage src(40, 24, QImage::Format_ARGB32);
    src.fill(QColor(255, 0, 0));
    QApplication::clipboard()->setImage(src);
    QApplication::processEvents();

    auto *pasteAct = w->findChild<QAction *>(QStringLiteral("actionPaste"));
    check(pasteAct != nullptr, "有「粘贴为新图层」动作");
    if (!pasteAct) {
        delete w;
        return;
    }

    pasteAct->trigger();
    QApplication::processEvents();

    checkEqInt(doc->layerCount(), layersBefore + 1, "粘贴后图层数 +1");

    // ── ★ 核心不变量：画布尺寸一个像素都不能变 ★ ──
    //
    //  addImageAsNewLayer() 特意把新图层开成**画布大小**再把图像画进去，
    //  就是为了不触发 Document::addLayer 里那条"图层比画布大 →
    //  静默 setCanvasSizeInternal()"的路径。那条路径不压任何命令，
    //  撤销时画布尺寸回不去 —— 属于"静默失效"，必须用断言钉住。
    check(doc->canvasSize() == canvasBefore,
          "★ 画布尺寸不变（没有踩到 addLayer 的静默扩画布分支）");

    const Layer *top = doc->layers().layer(doc->layerCount() - 1);
    check(top != nullptr, "拿得到最上面那层");
    if (top) {
        check(top->image().size() == canvasBefore,
              "★ 新图层的位图尺寸恒等于画布（图层统一尺寸模型）");
        checkEqInt(doc->activeLayerIndex(), doc->layerCount() - 1,
                   "新图层成为活动图层（后续框选 / 移动要作用在它上面）");
        checkStr(top->name(), QStringLiteral("Pasted Layer"), "图层名是「Pasted Layer」");

        // 居中放置，而且只占了中间那一小块 ——
        // 后一条挡住"整层被图像铺满"这类错误（例如 drawImage 位置算成 0,0）
        const QPoint center((canvasBefore.width()  - 40) / 2,
                            (canvasBefore.height() - 24) / 2);
        check(qRed(top->image().pixel(center.x() + 5, center.y() + 5)) > 200,
              "★ 粘贴内容落在画布中央（居中放置）");
        check(qAlpha(top->image().pixel(0, 0)) == 0,
              "★ 画布左上角仍是透明的（不是整层被铺满）");
    }

    // ── ★ 撤销粒度：一次粘贴 = 一条撤销记录 ★ ──
    //
    //  重构把建层代码挪了地方，最容易的失误就是把"一条命令"变成"两条"，
    //  于是用户按一次 Ctrl+Z 只退掉一半。用可观察的行为断言，
    //  而不是去数栈里的条数 —— 前者才是用户真正感受到的东西。
    auto *undoAct = w->findChild<QAction *>(QStringLiteral("actionUndo"));
    check(undoAct != nullptr, "有撤销动作");
    if (undoAct) {
        undoAct->trigger();
        QApplication::processEvents();
        checkEqInt(doc->layerCount(), layersBefore,
                   "★ 撤销**一次**就把图层数复原（一次粘贴 = 一条撤销）");
    }

    // ── 第二段：粘贴一张**比画布大**的图 ──
    //
    //  ★ 这一段才是真正在守"静默扩画布"那条路径 ★
    //    上面用的是 40×24 的小图。就算有人把新图层开成图片尺寸，
    //    Document::addLayer 也只会把它重排到左上角（因为比画布小），
    //    画布尺寸照样不变 —— 也就是说那条断言在小图上**根本测不出来**。
    //    换成比画布大的图，一旦新图层不是画布尺寸，addLayer 内部就会走
    //    setCanvasSizeInternal() 把画布悄悄撑大，而那条路径**不压任何命令**，
    //    撤销时画布尺寸回不去。这正是本工程最忌讳的"静默失效"。
    if (pasteAct && doc->canvasSize().isValid()) {
        const QSize canvasNow = doc->canvasSize();
        const int   layersNow = doc->layerCount();

        QImage big(canvasNow.width()  + 80,
                   canvasNow.height() + 60,
                   QImage::Format_ARGB32);
        big.fill(QColor(0, 0, 255));
        QApplication::clipboard()->setImage(big);
        QApplication::processEvents();

        pasteAct->trigger();
        QApplication::processEvents();

        checkEqInt(doc->layerCount(), layersNow + 1, "大图同样只是加一层");
        check(doc->canvasSize() == canvasNow,
              "★ 粘贴比画布大的图，画布尺寸依然一个像素都没变"
              "（没有踩到 addLayer 那条不压命令的静默扩画布路径）");

        const Layer *bigTop = doc->layers().layer(doc->layerCount() - 1);
        check(bigTop != nullptr && bigTop->image().size() == canvasNow,
              "★ 大图的新图层同样是画布尺寸（超出部分被裁掉，画布不被撑大）");
    }

    delete w;
}

// ---------------------------------------------------------------------------
//  「打开为图层」：三种尺寸策略 + 撤销粒度 + 失败时不动文档
// ---------------------------------------------------------------------------
//  testOpenAsLayerAssembly() 只证明了"菜单里有这个动作、它有真图标"。
//  它证明不了按下去会发生什么 —— 而「打开为图层」最难、最该被钉住的
//  恰恰是**尺寸决策**：
//
//    · 放得下          → 画布一个像素都不许变（只是加了层）
//    · 放不下 + Grow   → 画布扩到**逐维取大**（不是"整体取导入图"）
//    · 放不下 + Crop   → 画布**一个像素都不许变**，超出部分裁掉
//    · 有图层被锁      → 直接拒绝（画布尺寸变更会重写每一个图层）
//    · 读不出的文件    → 返回 false，且文档完全没被动过
//
//  ★ 为什么这些能在这里测 ★
//    QFileDialog 与"要不要扩画布"那个三选一框都留在 openAsLayer() 里；
//    importImageAsLayer() 只负责"给定路径 + 给定策略，把事情做完"。
//    于是这条路径可以脱离 UI 被调用 —— 也正因为如此，**"要不要扩画布"
//    在自动化场景下是一个显式参数，而不是一个没人能点的弹框**。
//    只为截图而存在的代码不受自检保护，所以当初才把它做成公开入口。
//
//  ★ "只宽不高"是刻意的 ★
//    "放不下"那张图做成宽超、高不超。若宽高都超，"逐维取大"与
//    "直接取导入图的尺寸"结果相同，断言就区分不出这两种实现 ——
//    而后者会把用户原来的画布高度也一起改掉。
//
//  ★ 注意 QImage 与 Document 是变尺寸的，所以每段都重新取 canvasSize ★
static void testImportImageAsLayerPolicies()
{
    beginGroup("打开为图层：三种尺寸策略 + 撤销粒度 + 失败不动文档");

    MainWindow *w = newWindow(QStringLiteral("brush"));
    CanvasView *cv = canvasOf(w);
    Document *doc = cv ? cv->document() : nullptr;
    check(doc != nullptr, "拿得到当前文档");
    if (!doc || !g_settingsDir) {
        delete w;
        return;
    }

    auto *undoAct = w->findChild<QAction *>(QStringLiteral("actionUndo"));
    check(undoAct != nullptr, "有撤销动作");

    const QString dir = g_settingsDir->path() + QStringLiteral("/oasl");
    QDir().mkpath(dir);

    const QSize c0 = doc->canvasSize();
    const int   n0 = doc->layerCount();

    // ── ① 放得下的图（60%）──────────────────────────────────────────────
    //
    //  ★ 这里用 GrowPolicy::Ask（= 产品默认值）★
    //    Ask 只在**放不下**时才弹框。放得下却弹出来 = 规格写错了，
    //    而离屏环境里没有第二个人点它 —— 症状是自检**永久挂死**，
    //    不是悄悄跳过。所以这一段同时也在钉"Ask 不会乱问"。
    const QSize s1(qMax(16, c0.width()  * 6 / 10),
                   qMax(16, c0.height() * 6 / 10));
    QImage i1(s1, QImage::Format_ARGB32_Premultiplied);
    i1.fill(QColor(255, 214, 102));
    const QString p1 = dir + QStringLiteral("/small.png");
    check(i1.save(p1, "PNG"), "写出临时 PNG（放得下）");

    check(w->importImageAsLayer(p1, MainWindow::GrowPolicy::Ask),
          "① 放得下：导入成功（Ask 且放得下 = 不弹框）");
    checkEqInt(doc->layerCount(), n0 + 1, "① 图层 +1");

    // ★ 核心不变量一：画布尺寸不许变 ★
    //   "只是加一层"最容易被写坏成"顺手把画布按导入图放大"。
    check(doc->canvasSize() == c0,
          "★ ① 画布尺寸一个像素都没变（加层不该动画布）");

    const Layer *top1 = doc->layers().layer(doc->layerCount() - 1);
    check(top1 != nullptr, "① 拿得到新图层");
    if (top1) {
        // ★ 核心不变量二：新图层位图恒等于画布 ★
        //   它成立与否决定 Document::addLayer 会不会踩到那条
        //   **静默改画布尺寸、且不压任何命令**的分支。
        check(top1->image().size() == c0,
              "★ ① 新图层位图恒等于画布尺寸（守住 addLayer 的静默扩画布）");

        const QPoint want = centeredOffset(c0, s1);
        check(want.x() > 0 && want.y() > 0, "① 画布确实比导入图大（居中偏移为正）");
        check(qAlpha(top1->image().pixel(want.x(), want.y())) == 255,
              "★ ① 导入内容落在 (canvas-img)/2 处（整幅居中，不是贴左上角）");
        check(qAlpha(top1->image().pixel(want.x() - 1, want.y())) == 0,
              "★ ① 偏移处左邻一格仍是透明的（差一个像素也抓得住）");
        checkStr(top1->name(), QFileInfo(p1).fileName(), "① 图层名就是文件名");
    }

    // 撤销一次应当整段回退（一次导入 = 一条撤销）
    if (undoAct) {
        undoAct->trigger();
        QApplication::processEvents();
        checkEqInt(doc->layerCount(), n0, "★ ① 撤销一次即回退（一次导入 = 一条撤销）");
        check(doc->canvasSize() == c0, "① 撤销后画布仍等于原尺寸");
    }

    // ── ② 放不下 + Crop：画布一个像素都不许变 ─────────────────────────
    const QSize s2(c0.width() + 80, qMax(16, c0.height() * 8 / 10));
    QImage i2(s2, QImage::Format_ARGB32_Premultiplied);
    i2.fill(QColor(120, 200, 255));
    const QString p2 = dir + QStringLiteral("/wide.png");
    check(i2.save(p2, "PNG"), "写出临时 PNG（比画布宽但更矮）");
    check(s2.width() > c0.width() && s2.height() < c0.height(),
          "② 前提：这张图只宽不高（逐维取大才可区分）");

    check(w->importImageAsLayer(p2, MainWindow::GrowPolicy::Crop),
          "② Crop：导入成功");
    checkEqInt(doc->layerCount(), n0 + 1, "② Crop：图层 +1（图被裁不等于丢掉那一层）");
    check(doc->canvasSize() == c0,
          "★ ② Crop：画布一个像素都没变（这是用户选 Crop 的全部意义）");

    if (undoAct) {
        undoAct->trigger();
        QApplication::processEvents();
        checkEqInt(doc->layerCount(), n0, "② 撤销一次即回退");
    }

    // ── ③ 放不下 + Grow：画布扩到逐维取大 ────────────────────────────
    check(w->importImageAsLayer(p2, MainWindow::GrowPolicy::Grow),
          "③ Grow：导入成功");

    const QSize wantUnion = unionSize(c0, s2);
    check(doc->canvasSize() == wantUnion,
          "★ ③ Grow：画布扩到 unionSize（逐维取大）");
    checkEqInt(doc->canvasSize().height(), c0.height(),
               "★ ③ 只宽不高 → 高度必须原封不动（区分逐维取大 / 整体取导入图）");
    checkEqInt(doc->layerCount(), n0 + 1,
               "③ Grow：图层 +1（扩画布 + 加图层合成一条撤销）");

    if (undoAct) {
        undoAct->trigger();
        QApplication::processEvents();
        // ★ 一条撤销必须把两件事一起退掉 ★
        //   扩画布与加图层是两条不同的命令，包在一个宏里。宏失效的症状是
        //   "按一次 Ctrl+Z 只退掉了图层、画布还大着"，用户会以为撤销坏了。
        check(doc->canvasSize() == c0,
              "★ ③ 撤销一次：画布尺寸跟着一起回退（扩画布与加图层是原子的）");
        checkEqInt(doc->layerCount(), n0,
                   "★ ③ 撤销一次：图层也退回去了（没有只退一半）");
    }

    // ── ④ 有锁定图层时，拒绝扩大画布 ─────────────────────────────────
    //
    //  画布尺寸变更会逐图层重排到新尺寸，所以锁定图层必须挡住它 ——
    //  与 openCanvasSize 的检查同款。"加一层"本身不需要这个检查。
    {
        Layer *base = doc->layers().layer(0);
        if (base)
            base->setLocked(true);

        const int nLock = doc->layerCount();
        check(!w->importImageAsLayer(p2, MainWindow::GrowPolicy::Grow),
              "★ ④ 有锁定图层时 Grow 被拒绝（画布变更会重写每个图层）");
        check(doc->canvasSize() == c0, "★ ④ 拒绝之后画布没被改");
        checkEqInt(doc->layerCount(), nLock,
                   "★ ④ 拒绝之后图层数没变（没做出「半件事」）");

        if (base)
            base->setLocked(false);
    }

    // ── ⑤ 读不出的文件：返回 false，文档一点不动 ─────────────────────
    //
    //  ★ 这一条同时钉住"自动化下不弹模态框"★
    //    交互时把"读不出"变成一句弹窗是对的；自动化时没有第二个人点
    //    "确定"，弹窗等于把进程挂死。所以失败汇报走 reportFailure()
    //    统一分流。这里若改成弹框，本测试会卡死而不是报错。
    {
        const int nBad = doc->layerCount();
        check(!w->importImageAsLayer(dir + QStringLiteral("/does_not_exist.png"),
                                     MainWindow::GrowPolicy::Grow),
              "★ ⑤ 读不出的文件返回 false（且**不弹框** —— 弹框会挂死脚本）");
        check(doc->canvasSize() == c0 && doc->layerCount() == nBad,
              "★ ⑤ 失败时文档完全没被动过（画布与图层数都没变）");
    }

    delete w;
}

// ---------------------------------------------------------------------------
//  多标签（阶段八）
// ---------------------------------------------------------------------------
//
//  ★ 这组盯的全是"删掉一行照样编译、照样看起来能用"的东西 ★
//
//    ① 中央控件是不是真的换成了标签容器
//       `setCentralWidget(m_canvas)` 改回单画布也能编译、也能跑，
//       只是标签栏永远不出现。
//
//    ② 撤销栈是不是真的**每页一个**
//       全工程共用一个栈时，在 B 页按 Ctrl+Z 会撤销掉 A 页的操作 ——
//       而 A 页的画面不会跟着变，用户看到的是"撤销了但什么都没发生"。
//       断言方式是"三个页的栈两两不等"，而不是"栈不是空指针"。
//
//    ③ 切页之后撤销菜单的可用状态有没有跟着走
//       ★ 这是本组最容易漏掉的一条 ★
//       canUndoChanged 只在**变化**时发。从"改过的 A 页"切到"没动过的
//       B 页"，B 的栈从头到尾没发过任何信号 —— 菜单项会停在 A 页的
//       状态上：明明不能撤销，却是亮的。所以切页必须**显式推**一次
//       （syncUndoRedoActions）。
//
//    ④ 打开一个已经打开过的文件，是"切过去"还是"多开一份"
//       多开一份会让"在两个标签里各改一遍、保存时后者覆盖前者"成真 ——
//       而用户在保存前完全不会意识到自己有两份。
//
//    ⑤ 一页都不剩时的空状态
//       "新建"必须仍然是可用的，否则用户再也没有出路
//       （菜单全灰 + 关不掉窗口 = 只能杀进程）。
//
//  ★ 有一件事这组测不了：关闭**有未保存修改**的页会弹模态确认框 ★
//    离屏环境下没人点它，触发就是永久挂死（自检挂死比失败更糟）。
//    这条只能靠人眼验证；这里能钉住的是它的**判据**：标签标题上的 *
//    （与模态框用的是同一个 needsSave 判据，见 MainWindowCommands.cpp）。
// ---------------------------------------------------------------------------

static void testDocumentTabs()
{
    beginGroup("多标签：中央控件 / 每页独立撤销栈 / 切页同步 / 关闭 / 空状态");

    MainWindow *w = newWindow(QStringLiteral("brush"));

    // ── ① 中央控件必须是标签容器，而不是裸画布 ──
    DocumentTabs *tabs = tabsOf(w);
    check(tabs != nullptr, "★ 找得到 DocumentTabs");
    // ★ 中央控件从"裸标签容器"变成了「拐角 + 标尺 + 标签」的网格宿主 ★
    //   （阶段七加了标尺）。所以不能再断言 centralWidget() == tabs ——
    //   改成"它确实挂在中央控件里"，语义才是我们要的：标签容器真的在显示，
    //   不是个没人用的孤儿控件。
    check(w->centralWidget() != nullptr && tabs != nullptr
              && tabs->parentWidget() == w->centralWidget(),
          "★ 标签容器挂在中央控件里（中央控件 = 拐角 + 标尺 + 标签的网格宿主）");
    check(tabs && tabs->tabsClosable(), "★ 每页带 ×（关不掉页的多标签等于没做）");
    checkEqInt(tabs ? tabs->count() : -1, 1, "启动时正好一页");

    DocumentTab *tab0 = tabs ? tabs->tabAt(0) : nullptr;
    check(tab0 && tab0->doc && tab0->stack && tab0->canvas, "第一页的三件套齐全");
    if (!tabs || !tab0 || !tab0->doc || !g_settingsDir) {
        delete w;
        return;
    }

    Document     *doc0    = tab0->doc;
    CommandStack *stack0  = tab0->stack;
    CanvasView   *canvas0 = tab0->canvas;

    check(canvas0 == currentCanvasOf(w),
          "★ 「当前画布」就是第一页的画布");

    auto *undoAct = w->findChild<QAction *>(QStringLiteral("actionUndo"));
    auto *newAct  = w->findChild<QAction *>(QStringLiteral("actionNew"));
    auto *closeAct = w->findChild<QAction *>(QStringLiteral("actionClose"));
    auto *saveAct = w->findChild<QAction *>(QStringLiteral("actionSave"));
    check(undoAct && newAct && closeAct && saveAct, "四个动作都拿得到");
    if (!undoAct || !newAct || !closeAct || !saveAct) {
        delete w;
        return;
    }

    check(!undoAct->isEnabled(), "★ 刚启动时撤销是灰的（没有可撤销的操作）");

    // ── 新建两页 ──
    newAct->trigger();
    QApplication::processEvents();
    newAct->trigger();
    QApplication::processEvents();

    checkEqInt(tabs->count(), 3, "★ 新建两次得到三页");
    check(tabs->tabAt(0) && tabs->tabAt(0)->doc == doc0,
          "★ 第一页的文档还是原来那一份（新建**不会**关掉旧文档）");
    check(tabs->tabAt(0) && tabs->tabAt(0)->stack == stack0,
          "★ 第一页的撤销栈也没被换掉（撤销历史不会因为新建而丢）");
    checkEqInt(tabs->currentIndex(), 2, "★ 新建之后当前页是新的那一页");
    check(currentCanvasOf(w) != canvas0,
          "★ 切页换了画布（不是共用一块）");

    // ── ② 三个栈互不相同 ──
    const QList<CommandStack *> stacks = w->findChildren<CommandStack *>();
    checkEqInt(stacks.size(), 3, "★ 窗口里正好有三个撤销栈（一页一个）");
    check(stacks.size() == 3
              && stacks.at(0) != stacks.at(1) && stacks.at(1) != stacks.at(2)
              && stacks.at(0) != stacks.at(2),
          "★★ 三个栈两两不等（共用栈 = 撤销会撤到别的文档上）");

    // ── ③ 撤销可用状态必须跟着切页走 ──
    const QString dir = g_settingsDir->path() + QStringLiteral("/tabs");
    QDir().mkpath(dir);
    QImage small(QSize(40, 30), QImage::Format_ARGB32_Premultiplied);
    small.fill(QColor(255, 214, 102));
    const QString png = dir + QStringLiteral("/a.png");
    check(small.save(png, "PNG"), "写出临时 PNG");

    // 在第 0 页上做一次导入（一条撤销记录，文档变脏）
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    checkEqInt(tabs->currentIndex(), 0, "★ 点标签切回了第 0 页");
    check(currentCanvasOf(w) == canvas0, "★ 切回来时当前画布又变回第 0 页那块");

    const int n0 = doc0->layerCount();
    check(w->importImageAsLayer(png, MainWindow::GrowPolicy::Crop),
          "第 0 页导入成功");
    checkEqInt(doc0->layerCount(), n0 + 1, "第 0 页图层 +1");
    check(undoAct->isEnabled(), "★ 第 0 页现在可以撤销了（撤销菜单亮了）");
    check(tabs->tabText(0).startsWith(QLatin1Char('*')),
          "★ 第 0 页的标签带上了 *（有未保存的修改）");

    // 切到从未改过的第 1 页 —— 撤销必须变灰
    tabs->setCurrentIndex(1);
    QApplication::processEvents();
    check(!undoAct->isEnabled(),
          "★★ 切到没动过的第 1 页后撤销立刻变灰"
          "（canUndoChanged 不会发信号，必须靠切页时显式推）");

    // 再切回第 0 页 —— 撤销必须又亮了，而且历史还在
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    check(undoAct->isEnabled(),
          "★★ 切回第 0 页后撤销又亮了（历史按页保存，没有丢）");
    checkEqInt(doc0->layerCount(), n0 + 1,
               "★ 第 0 页的图层数没有被切页影响");

    // ── ④ 重复打开同一个文件：切过去，不新开一页 ──
    const int beforeDup = tabs->count();
    check(w->openDocumentPaths(QStringList{png}),
          "第一次打开这个文件（此前只是导入过它的内容，没打开过文件本身）");
    QApplication::processEvents();
    checkEqInt(tabs->count(), beforeDup + 1, "第一次确实新开了一页");

    Document *dupDoc = tabs->tabAt(beforeDup) ? tabs->tabAt(beforeDup)->doc : nullptr;
    check(dupDoc != nullptr && dupDoc->filePath() == png,
          "新页的文档记着这个文件路径（标签标题才会显示文件名）");

    tabs->setCurrentIndex(0);           // 先离开它，才能验"第二次是切过去"
    QApplication::processEvents();
    checkEqInt(tabs->currentIndex(), 0, "先切回第 0 页");

    check(w->openDocumentPaths(QStringList{png}),
          "第二次打开同一个文件：报告成功");
    QApplication::processEvents();
    checkEqInt(tabs->count(), beforeDup + 1,
               "★★ 没有多开一页（多开一份 → 两个标签各改一遍会互相覆盖）");
    check(dupDoc != nullptr && tabs->indexOfDocument(dupDoc) >= 0,
          "★ 原来那一页还在");
    check(dupDoc != nullptr && tabs->currentIndex() == tabs->indexOfDocument(dupDoc),
          "★ 而是切到了已有那一页");

    // ── ④b 状态栏的缩放比必须跟着切页走 ──
    //
    //  ★ 这一条是"看截图"看出来的，不是断言先发现的 ★
    //    CanvasView::viewChanged 只在缩放**变化**时才发，而切页时新画布
    //    的缩放本来就是它自己的值 —— applyDocumentToPanels 里那句
    //    setDocument() 会因此早退，一个信号都不发。不显式推一次，
    //    状态栏上那个数字就会停在上一个页的数值上。
    //    实测：在 1200×800 的页上写着「99.9%」，切到 3840×2160 的页
    //    仍然是「99.9%」，而真实值是 31.6%。
    //    窗口标题是对的（它直接读 m_canvas->zoom()）—— 同一件事两处显示、
    //    只有一处走信号，不一致就是从那里来的。
    //
    //  ★ 为什么必须挑两页尺寸不同的 ★
    //    这一组里的 1200×800 页彼此缩放相同，拿它们对比时"跟不跟着走"
    //    完全看不出来。40×30 的那一页是唯一尺寸差得够远的。
    const int tinyTabIndex = tabs->indexOfDocument(dupDoc);
    check(tinyTabIndex >= 0, "找得到那张小图所在的页");

    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    const qreal shownBig  = statusZoomPercent(w);
    const qreal realBig   = canvas0 ? canvas0->zoom() * 100.0 : -1.0;
    check(canvas0 && qAbs(shownBig - realBig) < 0.6,
          "★ 第 0 页：状态栏显示的缩放比 == 这一页画布的真实缩放比");

    tabs->setCurrentIndex(tinyTabIndex);
    QApplication::processEvents();
    CanvasView *canvasTiny = currentCanvasOf(w);
    const qreal shownTiny = statusZoomPercent(w);
    const qreal realTiny  = canvasTiny ? canvasTiny->zoom() * 100.0 : -1.0;
    check(canvasTiny && canvasTiny != canvas0,
          "小图那一页有它自己的一块画布");
    // ★ 先证明这一组"验得出来"：两页的**真实**缩放比必须差得够远 ★
    //   否则下面那条"状态栏跟着换"可能只是两页恰好相同而撞对的。
    //   ★ 这里比的是画布的真实缩放，不是状态栏显示的数字 ★
    //     比显示值的话，状态栏一旦卡住，这条"前提断言"会跟着一起红 ——
    //     一次植入验证会报出两条失败，看起来像两个问题，其实只有一个。
    check(canvas0 && canvasTiny && qAbs(realBig - realTiny) > 1.0,
          "★★ 两页的真实缩放比相差足够大（后面那条断言才有意义）");
    check(canvasTiny && qAbs(shownTiny - realTiny) < 0.6,
          "★★ 切到小图那一页后状态栏的缩放比跟着换"
          "（不显式补推就会停在第 0 页的数值上 —— 而它不报错，只是数字不对）");

    // ── ⑤ 关闭标签页 ──
    //   只关**干净**的页 —— 关脏页会弹模态确认框，离屏下没人点，
    //   自检会永久挂死（比失败更糟）。见本组开头的说明。
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    Document *doc2 = tabs->tabAt(2) ? tabs->tabAt(2)->doc : nullptr;
    QPointer<Document> doc2Alive = doc2;
    check(doc2 != nullptr && doc2 != doc0, "第 2 页装的是另一个文档（刚建的空文档）");

    const int beforeClose = tabs->count();
    closeAct->trigger();
    QApplication::processEvents();

    checkEqInt(tabs->count(), beforeClose - 1, "★ 关掉一页（页数 -1）");
    check(doc2Alive.isNull(), "★ 被关掉那一页的文档确实被销毁了");
    check(tabs->tabAt(0) && tabs->tabAt(0)->doc == doc0,
          "★ 关掉别的页完全没有碰到第 0 页的文档");
    check(tabs->tabAt(0) && tabs->tabAt(0)->stack == stack0,
          "★ 第 0 页的撤销栈也没被换掉");
    check(currentCanvasOf(w) != nullptr, "★ 关掉一页之后当前画布有效");

    // ── ⑤b 把第 0 页恢复成"没有未保存修改" ──
    //   ★ 为什么必须做这一步 ★
    //     后面的"全部关掉"会一页一页走未保存确认。第 0 页此刻是脏的
    //     （第 ③ 段导入过），而确认框是模态的 —— 离屏环境下没人点它，
    //     自检会永久挂死。撤销回原点让它变回 clean，路才走得通。
    //     顺带这也是"栈的 clean 状态自动跟踪"的一次验证。
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    check(undoAct->isEnabled(), "第 0 页此刻仍然可以撤销");
    undoAct->trigger();
    QApplication::processEvents();
    checkEqInt(doc0->layerCount(), n0, "★ 撤销之后第 0 页的图层数回到初始值");
    check(!undoAct->isEnabled(),
          "★ 撤销到原点后变成「不能再撤销」（栈自动回到 clean）");
    check(!tabs->tabText(0).startsWith(QLatin1Char('*')),
          "★ 标签上的 * 也跟着消失了（与未保存确认用的是同一个判据）");

    // ── ⑥ 关到一页不剩：进入空状态 ──
    while (tabs->count() > 0) {
        closeAct->trigger();
        QApplication::processEvents();
    }

    checkEqInt(tabs->count(), 0, "全部关完");
    check(currentCanvasOf(w) == nullptr, "★ 没有页时「当前画布」为空（不是野指针）");
    check(!saveAct->isEnabled(), "★ 没有文档时「保存」是灰的");
    check(!undoAct->isEnabled(), "★ 没有文档时「撤销」是灰的");

    // ★★ 但「新建」必须仍然可用 ★★
    //   全灰 + 关不掉 = 用户只能杀进程。这是空状态唯一不能出错的地方。
    check(newAct->isEnabled(), "★★ 没有文档时「新建」仍然可用（否则用户没有出路）");
    checkStr(w->windowTitle(), appinfo::titleSuffix(),
             "★ 没有文档时窗口标题 = 程序名（不带文件名与缩放比）");

    newAct->trigger();
    QApplication::processEvents();
    checkEqInt(tabs->count(), 1, "★ 空状态下「新建」真的能建出一页（不是个亮着没用的菜单项）");
    check(currentCanvasOf(w) != nullptr, "★ 新的一页有画布");
    check(saveAct->isEnabled(), "★ 新建之后「保存」又可用（动作状态被显式推回来了）");
    check(tabs->tabTitle(0) == QStringLiteral("Untitled-1"),
          "★ 新页的标题是未命名-1");

    // ── ⑦ 关窗口：逐页销毁全部撤销栈 ──
    //
    //  ★ 这条路径必须单独测 ★
    //    它走的是 clearAllTabs()，与上面"一页一页点 ×"**不是同一段代码**。
    //    而"销毁撤销栈"恰恰是最容易留下悬垂指针的地方 ——
    //    历史面板（QUndoView）内部模型存的是裸 QUndoStack*，
    //    没在销毁前解绑就是一次 use-after-free。实测症状就是段错误，
    //    而且它只在"被销毁的栈恰好被某个部件指着"时才出现，
    //    换一条路径跑就绿了。
    //
    //  ★ 这里不会弹模态框 ★
    //    所有页都是干净的：第 0 页已被撤销回原点，这一页是刚建的空文档。
    check(w->close(), "★ 关窗口成功（没有未保存内容，不该被拦下）");
    QApplication::processEvents();
    checkEqInt(tabs->count(), 0,
               "★★ 关窗口逐页销毁了撤销栈而没有踩空（历史面板已提前解绑）");
    check(currentCanvasOf(w) == nullptr, "★ 关窗口后「当前画布」为空（不是野指针）");

    delete w;
}

// ---------------------------------------------------------------------------
//  命令行参数 = 要打开的文件（多标签之后才有意义）
// ---------------------------------------------------------------------------
//
//  单文档时代第二个参数只能被丢掉，所以这段行为从"多标签"才开始存在。
//  它同时钉住两件事：参数里的文件真的被打开成多页；参数全都无效时
//  回落到一个空白文档（而不是"一个标签都没有、菜单全灰"）。

// ===========================================================================
//  阶段七：视图辅助 —— 标尺与参考线
// ===========================================================================
//
//  为什么这一组必须存在：
//
//    标尺与参考线都是**纯呈现**：装错了不会崩、不会报错，只会"看起来正常
//    但数字/位置对不上"。三条最危险的静默失效都在这里 ——
//      ① 切页之后标尺仍按上一页的缩放画（刻度与图像对不上位）；
//      ② 参考线同时混进了导出（视图辅助变成了图像内容）；
//      ③ "松手判定"反了 —— 松回尺子上反而落线。
//    ①②③ 各配了断言；其中"参考线确实被画出来"用的是**量像素**，
//    "参考线不进导出"用的是**合成结果逐像素不变**。
static void testRulersAndGuides()
{
    beginGroup("视图辅助：标尺装配 / 单位 / 参考线（拖出·拖动·锁定·不影响导出）");

    MainWindow *w = newWindow(QStringLiteral("brush"));

    // ── ① 装配：两把尺子 ──
    const QList<RulerWidget *> rulers = w->findChildren<RulerWidget *>();
    checkEqInt(rulers.size(), 2, "★ 正好两把标尺（一条在上边、一条在左边）");

    RulerWidget *hr = nullptr;
    RulerWidget *vr = nullptr;
    for (RulerWidget *r : rulers) {
        if (r->orientation() == Qt::Horizontal) hr = r;
        else                                    vr = r;
    }
    check(hr != nullptr && vr != nullptr, "一把水平、一把垂直（方向没写反）");
    check(hr && hr->height() == RulerWidget::kThickness,
          "★ 水平标尺的厚度 == kThickness");
    check(vr && vr->width() == RulerWidget::kThickness,
          "★ 垂直标尺的厚度 == kThickness");

    QWidget   *host = w->centralWidget();
    CanvasView *cv  = currentCanvasOf(w);
    check(host && hr && vr && hr->parentWidget() == host && vr->parentWidget() == host,
          "★ 两把尺子都挂在中央控件（网格宿主）上，不是浮在画布上");
    check(cv && hr && hr->view() == cv, "★ 启动时水平标尺指向当前画布");
    check(cv && vr && vr->view() == cv, "★ 启动时垂直标尺指向当前画布");

    if (!hr || !vr || !cv || !host) {
        delete w;
        return;
    }

    // ── ② 切页之后标尺必须重指 ──
    //     ★★ 本组最重要的一条 ★★
    //       不重指也能"正常显示"（刻度照画），只是刻度与图像对不上 ——
    //       属于"看起来对、其实全错"，人眼几乎发现不了。
    auto *newAct = w->findChild<QAction *>(QStringLiteral("actionNew"));
    check(newAct != nullptr, "拿得到「新建」动作");
    if (newAct) {
        newAct->trigger();
        QApplication::processEvents();
    }
    CanvasView *cv2 = currentCanvasOf(w);
    check(cv2 && cv2 != cv, "新开一页换了画布");
    check(cv2 && hr->view() == cv2, "★★ 切页后水平标尺重指到新画布");
    check(cv2 && vr->view() == cv2, "★★ 切页后垂直标尺也重指了");
    if (!cv2) {
        delete w;
        return;
    }

    // ── ③ 单位：必须走**真实的菜单动作**，不能直接调那个槽 ──
    //
    //   ★ 为什么"直接调槽"是假绿 ★
    //     setRulerUnit() 是个普通私有成员函数，直接调它只证明"槽自己是对的"。
    //     真正会坏的是**菜单第 i 项 ⇄ 枚举 unitAt(i)** 这根线
    //     （加一个单位、或改一次顺序就会错位），而它只有"把第 i 项点一下"
    //     才照得到 —— 顺带把 action→槽 那根 connect 也一起验了。
    auto *unitMenu = w->findChild<QMenu *>(QStringLiteral("menuRulerUnit"));
    check(unitMenu != nullptr, "拿得到「标尺单位」子菜单（objectName 对得上）");

    if (unitMenu) {
        const QList<QAction *> unitActs = unitMenu->actions();
        checkEqInt(unitActs.size(), int(rulermetrics::kUnitCount),
                   "★ 子菜单里正好 5 个单位项");

        check(hr->unit() == rulermetrics::Unit::Pixel, "默认单位是像素");

        // ★ 逐项点一遍：第 i 项必须正好切到 unitAt(i) ★
        //   一条断言同时钉住三件事 —— 顺序协议、两把尺子一起切、菜单→槽是通的。
        const int n = qMin(unitActs.size(), int(rulermetrics::kUnitCount));
        for (int i = 0; i < n; ++i) {
            unitActs.at(i)->trigger();
            QApplication::processEvents();

            const rulermetrics::Unit want = rulermetrics::unitAt(i);
            char what[160];
            std::snprintf(what, sizeof(what),
                          "★ 点第 %d 项 → 水平尺切到 unitAt(%d)", i, i);
            check(hr->unit() == want, what);
            std::snprintf(what, sizeof(what),
                          "★ 点第 %d 项 → 垂直尺跟着一起切（不会只切一把）", i);
            check(vr->unit() == want, what);
        }

        // 切回像素 —— 别把这个状态留给后面的用例
        const int pxIdx = rulermetrics::unitIndex(rulermetrics::Unit::Pixel);
        unitActs.at(pxIdx)->trigger();
        QApplication::processEvents();
        check(hr->unit() == rulermetrics::Unit::Pixel, "切回像素");
    }

    // ── ④ 从标尺拖出参考线（真实鼠标事件链）──
    //
    //   ★ 为什么走事件而不是直接调 addGuide() ★
    //     本组的坑全在"松手判定"上（松在尺子上 = 放弃 / 松到画布 = 落线），
    //     那是事件层面的逻辑，直接调 API 就绕过去了。
    auto sendMouse = [](QWidget *target, QEvent::Type type, const QPointF &pos,
                        Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent ev(type, pos, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &ev);
    };

    checkEqInt(cv2->guides().size(), 0, "新页上一根参考线都没有（参考线是每页一份的视图状态）");

    sendMouse(hr, QEvent::MouseButtonPress,   QPointF(120, 5),  Qt::LeftButton, Qt::LeftButton);
    sendMouse(hr, QEvent::MouseMove,          QPointF(120, 60), Qt::NoButton,   Qt::LeftButton);
    check(cv2->hasPendingGuide(), "★ 拖动过程中画布上有「待落参考线」");
    sendMouse(hr, QEvent::MouseButtonRelease, QPointF(120, 60), Qt::LeftButton, Qt::NoButton);
    check(!cv2->hasPendingGuide(), "松手之后「待落参考线」被清掉");
    checkEqInt(cv2->guides().size(), 1, "★★ 松在画布那边 → 落下一根参考线");
    check(cv2->guides().size() == 1
              && cv2->guides().at(0).orientation == Qt::Horizontal,
          "★★ 从**顶部**尺子拖出来的是**水平**线（方向 == 尺子的方向，位置是 y）");

    // ★ 位置语义：参考线落在"松手那一点"换算出的图像坐标上 ★
    //   写反了（例如误用尺子上的那一点）会得到一条"位置完全不对但看着正常"的线。
    {
        //  originInRuler：画布左上角在**尺子坐标系**里的位置。
        //  ★ 走全局坐标中转，别写 cv2->mapTo(hr, …) ★
        //    尺子与画布是**兄弟**（都在中央网格宿主下），而 mapTo(parent,…)
        //    要求 parent 是自己的**祖先** —— 传兄弟进去父链走到 nullptr 就崩。
        const QPoint originInRuler = hr->mapFromGlobal(cv2->mapToGlobal(QPoint(0, 0)));
        const int expectedY = qRound(
            cv2->widgetToImage(QPointF(0.0, 60.0 - originInRuler.y())).y());
        check(cv2->guides().at(0).position == expectedY,
              "★★ 参考线位置 = 松手点换算出的图像 y（不是尺子自身的那一点）");
    }

    // 松回尺子自己身上 = 放弃
    sendMouse(hr, QEvent::MouseButtonPress,   QPointF(200, 5), Qt::LeftButton, Qt::LeftButton);
    sendMouse(hr, QEvent::MouseMove,          QPointF(200, 60), Qt::NoButton,  Qt::LeftButton);
    sendMouse(hr, QEvent::MouseButtonRelease, QPointF(200, 8), Qt::LeftButton, Qt::NoButton);
    checkEqInt(cv2->guides().size(), 1, "★★ 松回尺子上 → 不落线（数量不变）");

    // 垂直尺拖出来的是竖直线
    sendMouse(vr, QEvent::MouseButtonPress,   QPointF(5, 120), Qt::LeftButton, Qt::LeftButton);
    sendMouse(vr, QEvent::MouseMove,          QPointF(60, 120), Qt::NoButton,  Qt::LeftButton);
    sendMouse(vr, QEvent::MouseButtonRelease, QPointF(60, 120), Qt::LeftButton, Qt::NoButton);
    checkEqInt(cv2->guides().size(), 2, "★ 再从左边尺子拖出一根");
    check(cv2->guides().size() == 2
              && cv2->guides().at(1).orientation == Qt::Vertical,
          "★★ 从**左边**尺子拖出来的是**竖直线**（位置是 x）");

    // ── ⑤ 命中与锁定 ──
    //   只留一根，避免"两条线都在容差内"这种歧义。
    cv2->clearGuides();
    const int  gy0 = int(cv2->visibleImageRect().center().y());
    cv2->addGuide(Qt::Horizontal, gy0);
    const qreal gwY = cv2->imageToWidget(QPointF(0.0, double(gy0))).y();

    checkEqInt(cv2->guideAtWidget(QPointF(50.0, gwY), 5.0), 0,
               "★ 压在参考线上能命中（所以可以拖它）");
    check(cv2->guideAtWidget(QPointF(50.0, gwY + 60.0), 5.0) < 0,
          "离得远就命中不到（不会抢走正常落笔）");

    cv2->setGuidesLocked(true);
    check(cv2->guideAtWidget(QPointF(50.0, gwY), 5.0) < 0,
          "★★ 锁定之后命中不到 → 拖不动（但线仍然画出来）");
    cv2->setGuidesLocked(false);
    checkEqInt(cv2->guideAtWidget(QPointF(50.0, gwY), 5.0), 0, "解锁之后又能命中");

    // ── ⑥ ★ 量像素：参考线真的被画出来了 ★ ──
    //
    //   "数学全绿而画面不对"是自绘控件的经典失败模式（见铁律第十三章），
    //   所以每条自绘管线都要有一条**量它画出来的像素**的断言。
    cv2->clearGuides();
    cv2->setCheckerboardVisible(false);
    cv2->setGridVisible(false);

    const QImage imgNo = cv2->grab().toImage();

    const QRectF vis = cv2->visibleImageRect();
    cv2->addGuide(Qt::Horizontal, int(vis.center().y()));
    const QImage imgYes = cv2->grab().toImage();

    check(imgNo.size() == imgYes.size(), "两次 grab 的尺寸一致");

    // 差异统计抽成一个 lambda：**实线与虚线各量一次，判据完全一样**。
    //   （写两遍的话，早晚只有一遍会被改对 —— 与"判据只有一份"同律。）
    //   返回 { 有多少行变了, 变了的像素里有没有满足 pred 的颜色 }。
    auto diffStats = [](const QImage &a, const QImage &b, auto pred) {
        int  rows = 0;
        bool hit  = false;
        for (int y = 0; y < a.height() && y < b.height(); ++y) {
            bool rowDiffers = false;
            for (int x = 0; x < a.width() && x < b.width(); ++x) {
                if (a.pixel(x, y) == b.pixel(x, y))
                    continue;
                rowDiffers = true;
                if (pred(b.pixelColor(x, y)))
                    hit = true;
            }
            if (rowDiffers)
                ++rows;
        }
        return std::make_pair(rows, hit);
    };

    // 已落的线：**青色的实线**
    {
        const auto st = diffStats(imgNo, imgYes, [](const QColor &c) {
            return c.blue() > 150 && c.blue() > c.red() + 40;
        });
        check(st.first >= 1 && st.first <= 3,
              "★★ 参考线确实被画出来了（差异集中在 1~3 行，不是整片变色）");
        check(st.second, "★★ 而且画的是参考线的青色（不是别的东西恰好变了）");
    }

    // 「待落的那根」：**另一种颜色（橙）且是虚线** ★★ 必须单独量 ★★
    //   它是"还没松手、随时可能取消"的唯一视觉提示。不量它的话，
    //   "完全不画待落线"这种 bug 一条断言都碰不到（状态对了、画没画没人管）。
    //   虚线要额外验"是虚的"：沿那一行必须有多次明暗翻转，
    //   否则"实心橙线"也能满足"有橙色"。
    {
        cv2->clearGuides();
        const QImage imgBefore = cv2->grab().toImage();
        const int guideImgY = int(vis.center().y()) + 20;
        cv2->setPendingGuide(Qt::Horizontal, guideImgY);
        const QImage imgPending = cv2->grab().toImage();

        const auto st = diffStats(imgBefore, imgPending, [](const QColor &c) {
            return c.red() > 170 && (c.red() - c.blue()) > 70;
        });
        check(st.first >= 1 && st.first <= 3,
              "★★ 「待落的参考线」也被画出来了（同样只占 1~3 行）");
        check(st.second, "★★ 而且画的是它的橙色（与已落线的青色区分得开）");

        // 虚线的签名：那一行上"橙 ↔ 非橙"来回翻转多次。
        //   实心橙线只会翻转 2 次（一段）。
        //
        //   ★ 两条都必须注意 ★
        //   ⑴ 采样行要用**控件坐标**：参考线存的是图像坐标，
        //      直接拿它去索引 grab() 出来的图会量到无关的一行
        //      （这条一开始就写错过一次，报的是假失败）。
        //   ⑵ 不能只盯"算出来的那一行"：线落在亚像素位置时会被抗锯齿
        //      摊到相邻两行，哪一行更浓取决于四舍五入 ——
        //      钉死一行会得到一条"看运气"的断言。
        //      取"期望行 ±3"里翻转次数最多的那一行。
        const int expectedRow = qRound(
            cv2->imageToWidget(QPointF(0.0, double(guideImgY))).y());
        int bestFlips  = 0;
        int bestRow    = expectedRow;
        for (int dy = -3; dy <= 3; ++dy) {
            const int row = expectedRow + dy;
            if (row < 0 || row >= imgPending.height())
                continue;

            int  flips   = 0;
            bool prev    = false;
            bool started = false;
            for (int x = 0; x < imgPending.width(); ++x) {
                const QColor c = imgPending.pixelColor(x, row);
                const bool orange = c.red() > 150 && (c.red() - c.blue()) > 60;
                if (started && orange != prev)
                    ++flips;
                prev    = orange;
                started = true;
            }
            if (flips > bestFlips) {
                bestFlips = flips;
                bestRow   = row;
            }
        }
        check(bestFlips >= 6,
              "★★ 待落线是**虚线**（沿行多次明暗翻转），不是一条实心色带");
        cv2->clearPendingGuide();
    }

    // ── ⑦ ★ 参考线绝不进入导出 ★ ──
    cv2->addGuide(Qt::Vertical, 11);
    cv2->setPendingGuide(Qt::Horizontal, 13);
    const QImage composedWithGuides = cv2->document()->composite();
    cv2->clearGuides();
    const QImage composedNoGuides = cv2->document()->composite();
    check(composedWithGuides == composedNoGuides,
          "★★ 有参考线（含「待落线」）时合成结果逐像素不变（视图辅助不是图像内容）");

    // ── ⑧ 菜单动作：装配 + 图标 + 真的生效 ──
    auto *actShowRuler =
        w->findChild<QAction *>(QStringLiteral("actionShowRuler"));
    auto *actShowGuides =
        w->findChild<QAction *>(QStringLiteral("actionShowGuides"));
    auto *actLockGuides =
        w->findChild<QAction *>(QStringLiteral("actionLockGuides"));
    auto *actClearGuides =
        w->findChild<QAction *>(QStringLiteral("actionClearGuides"));
    check(actShowRuler && actShowGuides && actLockGuides && actClearGuides,
          "四个动作都拿得到（objectName 对得上）");

    if (actShowRuler && actShowGuides && actLockGuides && actClearGuides) {
        check(!actShowRuler->icon().isNull() && !actShowGuides->icon().isNull()
                  && !actLockGuides->icon().isNull()
                  && !actClearGuides->icon().isNull(),
              "★ 四个动作都有图标（守「有名动作必须有图标」那条规则）");

        // 显示/隐藏标尺：两把一起（拐角也跟着）
        actShowRuler->setChecked(false);
        QApplication::processEvents();
        check(!hr->isVisible() && !vr->isVisible(), "★ 取消勾选后两把尺子都藏起来了");
        actShowRuler->setChecked(true);
        QApplication::processEvents();
        check(hr->isVisible() && vr->isVisible(), "再勾上又都显示");

        // 显示/隐藏参考线：只影响"画不画"，不删线
        cv2->addGuide(Qt::Horizontal, 12);
        actShowGuides->setChecked(false);
        check(!cv2->guidesVisible(), "★ 取消「显示参考线」后画布收到通知");
        checkEqInt(cv2->guides().size(), 1, "隐藏 ≠ 删除（线还在，只是不画）");
        actShowGuides->setChecked(true);
        check(cv2->guidesVisible(), "再勾上又能看见");

        actLockGuides->setChecked(true);
        check(cv2->guidesLocked(), "★ 勾「锁定参考线」→ 画布进入锁定态");
        actLockGuides->setChecked(false);
        check(!cv2->guidesLocked(), "取消锁定");

        actClearGuides->trigger();
        checkEqInt(cv2->guides().size(), 0, "★ 「清除参考线」真的清掉了当前页的线");
    }

    delete w;

    // ── ⑨ 存档里的越界单位下标必须被夹回来 ──
    //
    //   ★ 为什么单列一组、还要另开一个窗口 ★
    //     这是**只在配置损坏时**才走到的分支，没法靠点菜单触发（菜单只有 5 项）。
    //     存档可能是旧版本写的、或被人手改过 —— 越界下标会让 5 个单选项
    //     **一个都不选中**（菜单上那一片全是空的），而 setRulerUnit() 又会
    //     静默拒绝。用户看到的是"单位菜单坏了"，且全程零报错。
    //     用"写坏 QSettings 再构造窗口"来照它，是唯一能复现这条路径的办法。
    {
        QSettings s;
        s.clear();
        s.setValue(QStringLiteral("view/rulerUnit"), 9999);
        s.sync();

        MainWindow bad;
        bad.resize(800, 600);
        bad.show();
        QApplication::processEvents();

        RulerWidget *badH = nullptr;
        for (RulerWidget *r : bad.findChildren<RulerWidget *>())
            if (r->orientation() == Qt::Horizontal)
                badH = r;
        check(badH && badH->unit() == rulermetrics::Unit::Pixel,
              "★★ 越界单位下标被夹回像素（存档损坏不会让标尺单位变成乱的）");

        auto *badMenu = bad.findChild<QMenu *>(QStringLiteral("menuRulerUnit"));
        int checkedCount = 0;
        if (badMenu)
            for (QAction *a : badMenu->actions())
                if (a->isChecked())
                    ++checkedCount;
        checkEqInt(checkedCount, 1,
                   "★★ 而且 5 个单选项里有且只有一个被选中（菜单不会全空）");
    }
}

// ===========================================================================
//  网格的间距与吸附
// ===========================================================================
//  这一组盯的是"菜单 / 单一来源 / 画布"三者的同步。吸附的**数学**已经由
//  grid_test 钉死了，工具的**接线**由 movetool / selection 钉死 ——
//  这里只验装配层最容易静默失效的两件事：
//    ① 菜单项真的挂在「视图」菜单里（addAction(nullptr) 合法且静默）；
//    ② 菜单改了之后，**当前画布**真的跟着改（属性不往下推 = 点了没反应）。
static void testGridSpacingAndSnap()
{
    beginGroup("网格：间距菜单 / 对齐开关 / 与画布同步");

    MainWindow *w = newWindow(QStringLiteral("move"));
    CanvasView *cv = currentCanvasOf(w);
    check(cv != nullptr, "拿得到当前画布");
    if (!cv) {
        delete w;
        return;
    }

    // ── ① 子菜单与动作都在 ──
    QMenu *spacingMenu = w->findChild<QMenu *>(QStringLiteral("menuGridSpacing"));
    check(spacingMenu != nullptr, "★ 建出了「网格间距」子菜单");

    // 它必须真的挂在「视图」菜单里，而不是个没人引用的孤儿
    //   ★ 按 objectName 找，不按菜单文本 ★
    //     源码英文化后菜单文本变成了 "View(&V)"，按中文前缀
    //     startsWith("视图") 查找会静默找不到（viewMenu 停在
    //     nullptr，后面两条"挂在视图里"的断言跟着全红）。
    //     所有顶层菜单现在都有 objectName（MainWindowActions.cpp
    //     逐条 setObjectName），这是两种界面语言下都成立的找法。
    QMenu *viewMenu = w->findChild<QMenu *>(QStringLiteral("menuView"));
    check(viewMenu != nullptr, "拿得到「视图」菜单");
    if (viewMenu && spacingMenu)
        check(viewMenu->actions().contains(spacingMenu->menuAction()),
              "★ 子菜单确实挂在「视图」里");
    check(spacingMenu && !spacingMenu->menuAction()->icon().isNull(),
          "子菜单自己有图标");

    QAction *snapAct = namedAction(w, "actionSnapToGrid");
    check(snapAct != nullptr, "★ 「对齐到网格」动作存在");
    check(snapAct && snapAct->isCheckable(), "它是可勾选的");
    check(snapAct && viewMenu && viewMenu->actions().contains(snapAct),
          "★ 它挂在「视图」菜单里（不是孤儿动作）");
    check(snapAct && !snapAct->icon().isNull(), "它有图标");
    check(snapAct && !snapAct->shortcut().isEmpty(),
          "它有快捷键（Ctrl+Shift+;）");

    QAction *customAct = namedAction(w, "actionGridCustom");
    check(customAct != nullptr, "「自定义…」动作存在");
    check(customAct && spacingMenu
              && spacingMenu->actions().contains(customAct),
          "★ 「自定义…」在「网格间距」子菜单里");
    check(customAct && !customAct->icon().isNull(), "它有图标");

    // ── ② 6 个预设：逐项 trigger()，验"第 i 项 → 间距值" ──
    //
    //  ★ 为什么必须逐项真的 trigger() ★
    //    直接调 setGridSpacing() 只能证明那个槽对，证明不了
    //    "菜单下标 ⇄ 间距值"这根线接对了。而这根线正是最容易写错的地方
    //    （菜单循环里用错数组下标、或者 setChecked 与 setData 不同步）。
    const int expected[] = { 8, 16, 32, 64, 128, 256 };
    if (spacingMenu) {
        // 子菜单里：6 个预设 + 1 个分隔符 + 1 个「自定义…」
        QList<QAction *> presets;
        for (QAction *a : spacingMenu->actions())
            if (!a->isSeparator() && a != customAct)
                presets << a;

        checkEqInt(presets.size(), 6, "子菜单里正好 6 个预设项");

        for (int i = 0; i < presets.size() && i < 6; ++i) {
            presets.at(i)->trigger();
            QApplication::processEvents();

            checkEqInt(cv->gridSize(), expected[i],
                       "★ 第 i 个预设 → 画布间距 = 表里的第 i 个值");
            checkEqInt(presets.at(i)->data().toInt(), expected[i],
                       "★ data() 与值同源（自定义设置要靠它同步勾选态）");
        }
    }

    // ── ③ 对齐开关必须推到画布 ──
    //     不推的话菜单勾了、画布不知道 —— 拖动完全不吸附，而且不报错。
    if (snapAct) {
        check(!cv->snapToGrid(), "默认不开吸附");
        snapAct->setChecked(true);
        QApplication::processEvents();
        check(cv->snapToGrid(), "★★ 勾上「对齐到网格」→ 画布真的开启了");

        snapAct->setChecked(false);
        QApplication::processEvents();
        check(!cv->snapToGrid(), "取消勾选 → 画布也关了");
    }

    // ── ④ 切页之后三个值都要补推 ──
    //     ★ 与"标准/参考线"同一条规律：画布每页一块，属性不补推就不跟着走。
    //       症状是"我在旧页调好了，新页又变回默认"。
    if (spacingMenu) {
        // 当前页设成 32（**非默认值**，这样"新页没补推"才暴露得出来 ——
        // 若停在默认 64 上，新页即使不补推也恰好是 64，断言会假绿）。
        const int idx32 = 2;   // 与 kGridPresets 的 {8,16,32,64,128,256} 对应
        QList<QAction *> presets;
        for (QAction *a : spacingMenu->actions())
            if (!a->isSeparator() && a != customAct)
                presets << a;
        if (idx32 < presets.size()) {
            presets.at(idx32)->trigger();
            QApplication::processEvents();
            checkEqInt(cv->gridSize(), 32, "先把当前页间距设成 32");
        }

        auto *newAct = namedAction(w, "actionNew");
        if (newAct) {
            newAct->trigger();                    // 新开一页
            QApplication::processEvents();
            CanvasView *cv2 = currentCanvasOf(w);
            check(cv2 && cv2 != cv, "新开了一页，换了画布");
            checkEqInt(cv2 ? cv2->gridSize() : -1, 32,
                       "★★ 新页的间距必须是**上一步设的 32**，不是默认 64");
            if (snapAct) {
                snapAct->setChecked(true);
                QApplication::processEvents();
            }

            // 再开一页，验证吸附也跟着走
            newAct->trigger();
            QApplication::processEvents();
            CanvasView *cv3 = currentCanvasOf(w);
            check(cv3 && cv3->snapToGrid(),
                  "★★ 新页的吸附状态也补推了");
        }
    }

    // ── ⑤ ★ 量像素：网格线真的被画出来了，而且间距就是设的那个值 ★ ──
    //
    //  "数学全绿而画面不对"是自绘管线的经典失败模式（铁律第十三章）。
    //  网格的**全部用户价值**就在画面上 —— drawGrid 里帧坐标算错、
    //  或者条件写反导致一条线都不画，上面所有断言（间距值、开关、
    //  补推）照样全绿。所以必须量它画出来的像素。
    //
    //  ★ 为什么不能只断言"有像素变了"★
    //    那样"网格画成一条竖线"或"线间距算成 2 倍"都能通过。
    //    这里额外量**相邻两条线的屏幕距离 ≈ 间距 × 缩放**，
    //    于是"间距真的等于设的那个值"也被钉在像素上。
    {
        CanvasView *cg = currentCanvasOf(w);
        check(cg != nullptr, "拿到量像素用的画布");
        if (cg) {
            cg->setCheckerboardVisible(false);   // 棋盘格会掩盖网格线
            cg->setGridVisible(false);
            cg->setGridSize(64);
            QApplication::processEvents();
            const QImage imgNo = cg->grab().toImage();

            cg->setGridVisible(true);
            QApplication::processEvents();
            const QImage imgYes = cg->grab().toImage();

            check(imgNo.size() == imgYes.size(), "两次 grab 的尺寸一致");

            const QRectF vis = cg->visibleImageRect();
            std::printf("      画布参数：控件 %dx%d  缩放 %.4f  间距 %d  屏幕间距 %.2f\n",
                        cg->width(), cg->height(), cg->zoom(), cg->gridSize(),
                        cg->gridSize() * cg->zoom());
            std::printf("      画布参数：可见图像矩形 (%.1f,%.1f,%.1f,%.1f)  画布 %d×%d\n",
                        vis.x(), vis.y(), vis.width(), vis.height(),
                        cg->document() ? cg->document()->canvasSize().width() : -1,
                        cg->document() ? cg->document()->canvasSize().height() : -1);

            // ★ 找"竖线"不能只看"这一列有没有像素变过" ★
            //   网格的**横线**横跨整个画布 —— 于是每一个列都会因为
            //   "横线经过它"而变化，直接收集会得到"几乎每一列都变了"，
            //   把竖线淹没掉（实测：680 列全中，合并后只剩 1 个中心）。
            //   竖线的签名是"这一列从上到下几乎全是变了的像素"，
            //   所以判据改成：**该列的变化行数 > 总行数的一半**。
            QList<int> cols;
            for (int x = 0; x < imgYes.width() && x < imgNo.width(); ++x) {
                int diffRows = 0;
                for (int y = 0; y < imgYes.height() && y < imgNo.height(); ++y) {
                    if (imgYes.pixel(x, y) != imgNo.pixel(x, y))
                        ++diffRows;
                }
                if (diffRows * 2 > imgYes.height())
                    cols << x;
            }

            std::printf("      竖线所在列 %d 个：", int(cols.size()));
            for (int i = 0; i < cols.size() && i < 30; ++i)
                std::printf("%d ", cols.at(i));
            std::printf("\n");

            check(cols.size() >= 3,
                  "★★ 网格确实被画出来了（纵向有不止一条线）");

            if (cols.size() >= 3) {
                // ★ 先把相邻的列**并成一条线**，再取各条线的中心 ★
                //   线落在亚像素位置时会被抗锯齿摊到相邻两列 —— 不合并的话
                //   gaps 里会混进一堆 1，中位数直接失真（实测就是这么挂的）。
                QList<int> centers;
                int runStart = cols.first();
                int prev     = cols.first();
                for (int i = 1; i < cols.size(); ++i) {
                    if (cols.at(i) == prev + 1) {
                        prev = cols.at(i);
                        continue;
                    }
                    centers << (runStart + prev) / 2;
                    runStart = prev = cols.at(i);
                }
                centers << (runStart + prev) / 2;

                std::printf("      网格线列中心 %d 个：", int(centers.size()));
                for (int i = 0; i < centers.size() && i < 12; ++i)
                    std::printf("%d ", centers.at(i));
                std::printf("\n");

                check(centers.size() >= 3,
                      "★★ 合并亚像素摊宽后仍有 ≥3 条独立的竖线");

                if (centers.size() >= 3) {
                    QList<int> gaps;
                    for (int i = 1; i < centers.size(); ++i)
                        gaps << (centers.at(i) - centers.at(i - 1));
                    std::sort(gaps.begin(), gaps.end());
                    const int medGap = gaps.at(gaps.size() / 2);

                    const qreal expect = 64.0 * cg->zoom();
                    std::printf("      相邻线距中位数 = %d px，期望 64×%.4f = %.1f px\n",
                                medGap, cg->zoom(), expect);
                    check(qAbs(qreal(medGap) - expect) <= 2.0,
                          "★★ 相邻网格线的屏幕距离 ≈ 间距×缩放（线间距真的是 64 图像像素）");
                }
            }
        }
    }

    delete w;
}

// ===========================================================================
//  另存为 ICO 图标
// ===========================================================================
//  这条功能的**唯一**用户可见入口是「文件 → 另存为…」时选 .ico ——
//  也就是说它藏在一个模态对话框后面。模态框在无人环境里会把进程永久
//  挂住，所以"干活的那半"被拆成了 MainWindow::exportIcon(path)：
//  吃路径、不弹窗、直接写出文件。本组驱动它。
//
//  像素级编码判据（字节结构 / 往返 / 掩码 / 预乘）在 ico_test 里 ——
//  那边不建窗口，跑得又快又干净。这里只管三件**只有装配层才知道**的事：
//    ① 光有编码器不算功能：文档 → 合成 → 导出 这条链真的通；
//    ② 超过 512 必须被拦下，而且**不许落下半成品文件**；
//    ③ Ctrl+S 存到一个 .ico 上时走的是同一条路（而不是回落到 Qt 插件，
//       那个会静默把大图缩到 256）。
// ---------------------------------------------------------------------------

// 造一张四象限的测试图：不透明红 / 半透明绿 / 全透明 / 不透明蓝。
//   四种 alpha 各一个，是为了让"透明通道有没有被丢掉"这件事无处可藏。
static QImage makeIcoTestImage(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const bool right  = x >= w / 2;
            const bool bottom = y >= h / 2;
            if (!right && !bottom)
                row[x] = qRgba(255, 0, 0, 255);
            else if (right && !bottom)
                row[x] = qRgba(0, 255, 0, 128);
            else if (!right && bottom)
                row[x] = qRgba(0, 0, 0, 0);
            else
                row[x] = qRgba(0, 0, 255, 255);
        }
    }
    return img;
}

// 两个颜色算不算"同一个像素"。
//   ★ 全透明像素的 RGB 没有意义（很多编码器会清零），只比 alpha ★
static bool icoPixelNear(const QColor &a, const QColor &b)
{
    if (a.alpha() != b.alpha())
        return false;
    if (a.alpha() == 0)
        return true;
    return qAbs(a.red()   - b.red())   <= 1
        && qAbs(a.green() - b.green()) <= 1
        && qAbs(a.blue()  - b.blue())  <= 1;
}

static void testIcoExport()
{
    beginGroup("另存为 ICO：上限拦截 + 真的能读回来");

    QTemporaryDir dir;
    check(dir.isValid(), "能创建临时目录");
    if (!dir.isValid())
        return;

    // ── ① 格式列表里确实有 ICO ──
    //   没有过滤器 = 用户根本选不到，功能等于不存在。
    //   这一条与"编码器写不写对"完全无关，只有装配层测得到。
    {
        bool hasIcoFilter = false;
        for (const QString &f : writableFormatFilters()) {
            if (f.contains(QStringLiteral("*.ico")))
                hasIcoFilter = true;
        }
        check(hasIcoFilter,
              "★★ 「另存为」的格式列表里确实有 ICO（否则这条路对用户不存在）");
        check(formatSupportsAlpha(QByteArray("ico")),
              "★ ICO 被当作**支持透明**的格式（否则另存为会错弹「透明会被填白」）");
    }

    // ── ② 正常尺寸：64×64 走通"文档 → 合成 → ICO → 读回来" ──
    {
        const QString srcPath = dir.filePath(QStringLiteral("src64.png"));
        check(makeIcoTestImage(64, 64).save(srcPath, "PNG"),
              "造出 64×64 的测试图");

        MainWindow *w = newWindow(QStringLiteral("brush"));
        checkEqInt(w->openDocumentPaths(QStringList{ srcPath }), 1,
                   "64×64 的图开成了一页");

        const QString icoPath = dir.filePath(QStringLiteral("out64.ico"));
        QString err;
        check(w->exportIcon(icoPath, &err), "★ 64×64 导出 ICO 成功");
        check(err.isEmpty(), "成功时不带错误信息");
        check(QFile::exists(icoPath), "文件真的落在磁盘上");

        // ★ 用独立实现（Qt 自带的 ico 读取器）读回来 ★
        QImageReader reader(icoPath, "ico");
        const QImage back = reader.read();
        check(!back.isNull(), "★★ 用 Qt 的 ico 读取器能读回来");
        if (!back.isNull()) {
            checkEqInt(back.width(), 64, "读回来的宽是 64");
            checkEqInt(back.height(), 64, "读回来的高是 64");

            // 与**合成结果**比，而不是与源文件比 ——
            //   判据是"存下来的就是用户看到的画面"。
            QImage expect = currentCanvasOf(w)->document()->composite();
            if (expect.format() != QImage::Format_ARGB32)
                expect = expect.convertToFormat(QImage::Format_ARGB32);
            checkEqInt(expect.width(), 64, "合成结果的宽是 64（前提）");

            int bad = 0;
            const int xs[] = { 1, 20, 40, 62 };
            const int ys[] = { 1, 20, 40, 62 };
            for (int yi = 0; yi < 4; ++yi) {
                for (int xi = 0; xi < 4; ++xi) {
                    if (!icoPixelNear(expect.pixelColor(xs[xi], ys[yi]),
                                      back.pixelColor(xs[xi], ys[yi])))
                        ++bad;
                }
            }
            checkEqInt(bad, 0, "★★ 四象限抽查逐点一致（含半透明与全透明）");
        }

        // 文件头：确认写出的是"图标"而不是别的什么
        {
            QFile f(icoPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray head = f.read(22);
                checkEqInt(head.size(), 22, "文件至少 22 字节（头 + 一条目录项）");
                checkEqInt(quint8(head.at(0)), 0, "idReserved == 0");
                checkEqInt(quint8(head.at(2)), 1, "idType == 1（图标）");
                checkEqInt(quint8(head.at(4)), 1, "idCount == 1");
                checkEqInt(quint8(head.at(12)), 32, "wBitCount == 32（带 alpha）");
            } else {
                check(false, "能打开导出的 ICO 文件");
            }
        }

        delete w;
    }

    // ── ③ ★★ 超过 512：必须拒绝，而且不落文件 ★★ ──
    //   用户原话："如果图片尺寸大于512x512，提醒用户尺寸太大无法保存ico格式"。
    {
        const QString bigPath = dir.filePath(QStringLiteral("src600.png"));
        QImage big(600, 400, QImage::Format_ARGB32);
        big.fill(QColor(200, 200, 200, 255));
        check(big.save(bigPath, "PNG"), "造出 600×400 的源图（宽超上限）");

        MainWindow *w = newWindow(QStringLiteral("brush"));
        checkEqInt(w->openDocumentPaths(QStringList{ bigPath }), 1,
                   "600×400 开成了一页");
        checkEqInt(currentCanvasOf(w)->document()->canvasSize().width(), 600,
                   "画布宽度确实是 600（前提）");

        const QString outPath = dir.filePath(QStringLiteral("too_big.ico"));
        QString err;
        check(!w->exportIcon(outPath, &err), "★★ 600×400 导出 ICO **被拒绝**");
        check(!err.isEmpty(), "★★ 拒绝时给出了原因（界面层拿它弹窗）");
        check(err.contains(QStringLiteral("600")),
              "★★ 原因里带**用户图的真实尺寸**（不是一句【保存失败】）");
        check(err.contains(QStringLiteral("512")),
              "★★ 原因里带**上限值**");
        check(!QFile::exists(outPath),
              "★★ 被拒绝时**不落下任何文件**（半成品比没有更糟）");

        // 只有一边超，同样要拦
        const QString tallPath = dir.filePath(QStringLiteral("src900.png"));
        QImage tall(100, 900, QImage::Format_ARGB32);
        tall.fill(Qt::white);
        check(tall.save(tallPath, "PNG"), "造出 100×900 的源图（高超上限）");
        checkEqInt(w->openDocumentPaths(QStringList{ tallPath }), 1, "100×900 开成了一页");
        QString errTall;
        check(!w->exportIcon(dir.filePath(QStringLiteral("tall.ico")), &errTall),
              "★★ 100×900（只有高超）同样拒绝");
        check(errTall.contains(QStringLiteral("900")),
              "★ 这次的提示里带的是高度");

        delete w;
    }

    // ── ④ 边界：512×512 恰好在上限内，必须放行 ──
    //   "大于 512"才拒绝 —— 恰好 512 是允许的。差一像素的判据很容易写反
    //   （写成 >=），这条断言就是防它。
    {
        const QString src512 = dir.filePath(QStringLiteral("src512.png"));
        check(makeIcoTestImage(512, 512).save(src512, "PNG"), "造出 512×512 的源图");

        MainWindow *w = newWindow(QStringLiteral("brush"));
        checkEqInt(w->openDocumentPaths(QStringList{ src512 }), 1, "512×512 开成了一页");

        const QString ico512 = dir.filePath(QStringLiteral("edge512.ico"));
        QString err;
        check(w->exportIcon(ico512, &err),
              "★★ 512×512 恰好在上限内 —— 导出成功（判据不能写成 >=）");
        if (!err.isEmpty())
            std::printf("     （意外的错误信息：%s）\n", qUtf8Printable(err));

        const QImage back = QImageReader(ico512, "ico").read();
        check(!back.isNull(), "512×512 能读回来");
        checkEqInt(back.width(), 512, "往返宽度一致");
        checkEqInt(back.height(), 512, "往返高度一致");

        delete w;
    }

    // ── ⑤ ★★ Ctrl+S 存到已有 .ico 上，走的必须是**同一条**路 ★★ ──
    //
    //  ★ 这条断言的价值在于它**能区分两套实现** ★
    //    · 我们的编码器：>256 写成 PNG 条目，尺寸原样保留；
    //    · Qt 自带的 qico 插件：超过 256 会**静默缩到 256**。
    //    所以"打开一张 300×300 的 ico → 保存 → 读回来仍是 300×300"
    //    只有我们这条实现能成立。若哪天有人在 writeImage 里漏掉 ico 分流，
    //    这里会立刻红 —— 而"文件存出来了"这种弱判据是抓不住的。
    {
        const QString src300 = dir.filePath(QStringLiteral("src300.png"));
        check(makeIcoTestImage(300, 300).save(src300, "PNG"), "造出 300×300 的源图");

        MainWindow *w = newWindow(QStringLiteral("brush"));
        checkEqInt(w->openDocumentPaths(QStringList{ src300 }), 1, "300×300 开成了一页");

        const QString ico300 = dir.filePath(QStringLiteral("round300.ico"));
        check(w->exportIcon(ico300, nullptr), "先导出一份 300×300 的 ICO");

        // 把它开成一页 —— 这一页的「归属文件」就是 .ico，
        // 于是 saveDocument()（Ctrl+S 那条路）会以 ico 格式写回同一个路径。
        checkEqInt(w->openDocumentPaths(QStringList{ ico300 }), 1,
                   "★ 把自己导出的 ico 再打开一次（该页归属文件 = .ico）");
        checkEqInt(currentCanvasOf(w)->document()->canvasSize().width(), 300,
                   "★ 能读回 300 宽（PNG 条目这条链是通的）");

        // ★ 走**菜单动作**触发，而不是直接调 saveDocument ★
        //   saveDocument 是 private；而它对应的正是用户按 Ctrl+S 的那条路。
        //   从动作触发既绕开了访问级别，又顺带钉住"这个动作真的接上了"。
        //
        // ★★ 先把文件删掉 ★★
        //   不删的话，"保存失败"与"保存成功但内容没变"分不开 ——
        //   文件还在那儿，断言照样绿（典型假绿）。
        //   删掉之后，**文件重新出现**本身就是"写入真的发生了"的证据。
        check(QFile::remove(ico300), "先删掉 ico，让「文件重新出现」成为判据");
        check(!QFile::exists(ico300), "删干净了");

        auto *actSave = w->findChild<QAction *>(QStringLiteral("actionSave"));
        check(actSave != nullptr, "找到「保存」菜单动作（actionSave）");
        if (actSave)
            actSave->trigger();

        check(QFile::exists(ico300),
              "★★ 触发「保存」后文件被重新写出来了（写入确实发生）");

        const QImage back = QImageReader(ico300, "ico").read();
        check(!back.isNull(), "★★ 保存后仍是合法的 ICO");
        if (!back.isNull()) {
            checkEqInt(back.width(), 300,
                       "★★ 仍是 300 宽 —— 没有被静默缩到 256（说明走的是自写编码器）");
            checkEqInt(back.height(), 300, "★★ 高同理");
        }

        QFile f(ico300);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray head = f.read(30);
            checkEqInt(head.size(), 30, "文件够长（头 + 目录项 + PNG 魔数）");
            checkEqInt(quint8(head.at(6)), 0,
                       "★★ 目录项宽度字节是 0（PNG 条目的特征；BMP 条目装不下 300）");
            // ★ 魔数用字节数组，不要写字面量 "\x89PNG" ★
            //   /utf-8 下源码里塞一个 0x89 原始字节属于无效 UTF-8，
            //   编译器会为此报编码告警 —— 而我们要求 0 警。
            static const uchar kPngMagic[4] = { 137, 80, 78, 71 };
            bool png = head.size() >= 26;
            for (int i = 0; png && i < 4; ++i)
                png = (quint8(head.at(22 + i)) == kPngMagic[i]);
            check(png, "★★ 图像数据仍以 PNG 魔数开头（我们这条分支）");
        } else {
            check(false, "能重新打开保存后的 ICO");
        }

        delete w;
    }
}

static void testCommandLineFiles()
{
    beginGroup("命令行参数：多文件开多页（并确认无参数时是一页空白）");

    if (!g_settingsDir) {
        check(false, "临时目录不可用");
        return;
    }

    const QString dir = g_settingsDir->path() + QStringLiteral("/cli");
    QDir().mkpath(dir);
    const QString p1 = dir + QStringLiteral("/one.png");
    const QString p2 = dir + QStringLiteral("/two.png");

    QImage a(QSize(24, 18), QImage::Format_ARGB32_Premultiplied);
    a.fill(QColor(255, 100, 100));
    QImage b(QSize(30, 22), QImage::Format_ARGB32_Premultiplied);
    b.fill(QColor(100, 100, 255));
    check(a.save(p1, "PNG") && b.save(p2, "PNG"), "写出两张临时 PNG");

    // ── 两个有效路径 → 两页，且不额外送一个空白页 ──
    {
        MainWindow w(nullptr, QStringList{p1, p2});
        DocumentTabs *tabs = tabsOf(&w);
        check(tabs != nullptr, "带参数构造的窗口也有标签容器");
        checkEqInt(tabs ? tabs->count() : -1, 2,
                   "★ 两个文件 = 两页（不会白送一个空白标签）");

        if (tabs && tabs->count() == 2) {
            check(tabs->tabAt(0)->doc->filePath() == p1,
                  "第一页开的是第一个文件");
            check(tabs->tabAt(1)->doc->filePath() == p2,
                  "第二页开的是第二个文件");
            checkEqInt(tabs->currentIndex(), 1,
                       "★ 当前页是最后打开的那一个（用户刚要求打开它）");
            check(tabs->tabAt(0)->stack != tabs->tabAt(1)->stack,
                  "两页各有自己的撤销栈");
        }
    }

    // ── 参数全都无效 → 回落到一个空白文档 ──
    //
    //  ★ 这里只能验"没有参数"的那一支 ★
    //    真给一个不存在的路径会弹"打开失败"的模态框，而离屏环境下
    //    没人点它 —— 自检会永久挂死。所以"参数全都无效时回落"
    //    这条路径的行为由构造函数的逻辑保证（openDocumentPaths 返回 0
    //    → 走 newDocument()），这里用"空列表"覆到同一个判据的相邻分支上。
    {
        MainWindow w(nullptr, QStringList());
        DocumentTabs *tabs = tabsOf(&w);
        checkEqInt(tabs ? tabs->count() : -1, 1,
                   "★ 没有参数时正好一页空白文档");
        check(tabs && tabs->tabAt(0) && tabs->tabAt(0)->doc->filePath().isEmpty(),
              "空白文档没有文件路径（标签标题会是「未命名-1」）");
        check(tabs && tabs->tabTitle(0) == QStringLiteral("Untitled-1"),
              "★ 标题确实是未命名-1");
    }
}

// ---------------------------------------------------------------------------
//  屏幕截图那几组共用的助手
// ---------------------------------------------------------------------------

/// 造一张能逐像素核对的"截图替身"：四象限四种颜色 + 右下角半透明。
///
/// ★ 尺寸刻意用非正方形、非 2 的幂 ★ —— 宽高写反、被取整到偶数之类的
/// 错误才暴露得出来（37×23 这种数字不会"碰巧"对上）。
/// ★ 透明像素直接写**预乘值** ★（50% 灰 = 64,64,64,128）。
/// 写 qRgba(128,128,128,128) 那是"未预乘的量"，在搬运/转换时会被改掉，
/// 断言就悄悄变成在测别的东西了。
static QImage makeCaptureTestImage(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(qRgba(0, 0, 0, 0));

    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const bool left = (x < w / 2);
            const bool top  = (y < h / 2);
            line[x] = top ? (left ? qRgba(200, 30, 30, 255) : qRgba(30, 200, 30, 255))
                          : (left ? qRgba(30, 30, 200, 255) : qRgba(64, 64, 64, 128));
        }
    }
    return img;
}

/// 逐像素差异个数（-1 = 尺寸都不同）。0 容差 —— 与全项目的像素断言同一口径。
static int diffPixelCount(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return -1;

    const QImage x = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    int diff = 0;
    for (int row = 0; row < x.height(); ++row) {
        const QRgb *px = reinterpret_cast<const QRgb *>(x.constScanLine(row));
        const QRgb *py = reinterpret_cast<const QRgb *>(y.constScanLine(row));
        for (int col = 0; col < x.width(); ++col) {
            if (px[col] != py[col])
                ++diff;
        }
    }
    return diff;
}

/// 跑事件循环直到条件成立（或超时），返回条件最终是否成立。
///
/// 这一组有两处**必须真的跑事件循环**才能验的东西：
///   · 延迟截图的定时器（它靠事件循环到点）
///   · 系统投来的 WM_HOTKEY（PostThreadMessage 是异步入队，要 PeekMessage 才取得出来）
/// ★ 用模板而不是 std::function ★：省一个 include，也不引入分配。
template <typename Predicate>
static bool pumpUntil(Predicate done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

// ===========================================================================
//  [18] 屏幕截图：菜单装配
// ===========================================================================
//
//  与「打开为图层」那组同一个理由：`QMenu::addAction(nullptr)` 是**合法且
//  静默**的，所以"动作对象存在"完全证明不了"菜单里有它"。
//  这一组钉的是"菜单树里的那几项"，而不是"某个对象存在"。

// 找子菜单里的某一项（用 objectName —— 自动化与对账脚本都靠它定位）
static QAction *namedAction(MainWindow *w, const char *name)
{
    return w->findChild<QAction *>(QLatin1String(name));
}

static void testScreenCaptureMenu()
{
    beginGroup("屏幕截图：菜单装配（子菜单 + 四项）");

    MainWindow *w = newWindow(QStringLiteral("move"));
    QMenu *fileMenu = fileMenuOf(w);
    check(fileMenu != nullptr, "拿得到「文件」菜单");

    // ── ① 子菜单确实在「文件」里 ──
    QMenu *capMenu = w->findChild<QMenu *>(QStringLiteral("menuScreenCapture"));
    check(capMenu != nullptr, "★ 建出了「屏幕截图」子菜单");

    if (fileMenu && capMenu) {
        check(fileMenu->actions().contains(capMenu->menuAction()),
              "★ 它确实挂在「文件」菜单里（不是个没人引用的孤儿菜单）");
    }
    check(capMenu && !capMenu->menuAction()->icon().isNull(),
          "子菜单自己有图标（否则在「一栏都有图标」的菜单里明显掉队）");

    // 位置：紧跟在「打开为图层」之后
    //   —— 它们同属"把外面的东西拿进来"那一组，放一起用户才好对照。
    if (fileMenu && capMenu) {
        QAction *asLayer = namedAction(w, "actionOpenAsLayer");
        const int iAsLayer = asLayer ? fileMenu->actions().indexOf(asLayer) : -1;
        const int iCap     = fileMenu->actions().indexOf(capMenu->menuAction());
        check(iAsLayer >= 0 && iCap == iAsLayer + 1,
              "★ 排在「打开为图层」的紧后面");
    }

    // ── ② 四项：都在子菜单里、都有名字、都有图标 ──
    const char *names[] = { "actionCaptureNow", "actionCaptureDelay3",
                            "actionCaptureDelay5", "actionCaptureDelay10" };

    QStringList missing, unlisted, iconless;
    for (const char *n : names) {
        QAction *a = namedAction(w, n);
        const QString name = QLatin1String(n);

        if (!a) {
            missing << name;
            continue;
        }
        if (capMenu && !capMenu->actions().contains(a))
            unlisted << name;
        if (a->icon().isNull())
            iconless << name;
    }

    check(missing.isEmpty(), "★ 四个动作都建出来了");
    if (!missing.isEmpty())
        std::printf("       没找到：%s\n", qUtf8Printable(missing.join(QStringLiteral(", "))));

    check(unlisted.isEmpty(), "★ 四个动作都真的在子菜单里");
    if (!unlisted.isEmpty())
        std::printf("       不在菜单里：%s\n", qUtf8Printable(unlisted.join(QStringLiteral(", "))));

    check(iconless.isEmpty(), "★ 四个动作都有图标");
    if (!iconless.isEmpty())
        std::printf("       缺图标：%s\n", qUtf8Printable(iconless.join(QStringLiteral(", "))));

    // ── ②.5 会话限制的判断不能"恒真" ──
    //   Wayland 会话里抓屏必然失败，上层据此改成"用系统截图键 + Ctrl+V
    //   粘贴"的提示。但如果这个判断写错了方向（比如无脑返回 Wayland），
    //   **所有桌面**都会看到"当前环境不支持截图" —— 那比原来的 bug 更糟：
    //   用户会以为软件坏了，其实一切正常。
    //   自检跑在 offscreen 平台上，正确答案必须是 None。
    check(zl::screenCaptureBlock() == zl::CaptureBlock::None,
          "★ 非 Wayland 会话不应被判为受限（否则所有桌面都会误报不支持）");

    // ── ③ 快捷键只在「立即截图」上，且必须来自候选表 ──
    //   ★ 这条不变量值得钉住 ★
    //     三个延迟项各带自己的秒数，给它们挂快捷键只会做出三个
    //     "用户永远记不住、还占着系统组合键"的键；而"立即"必须有，
    //     因为它是唯一需要"程序在后台也能按"的那一个。
    //
    //   ★ 为什么断言"属于候选表"，而不是"== kCaptureHotkey" ★
    //     首选键被别的程序占着时，程序会回落到候选表里的下一个键，并
    //     把菜单改成那一个 —— 这恰恰是对的（菜单上写的必须就是能按的）。
    //     写死"== 首选键"的话，这条断言会在**某些机器上**变红，而那里
    //     功能其实是好的：只是那台机器上恰好有别的软件占着这个键。
    //     真正与环境无关的不变量是这两条：
    //       ① 候选表第一项就是 kCaptureHotkey（"产品定的那一个"没被偷偷改掉）；
    //       ② 菜单上显示的键一定是候选表里的某一个（不会显示一个没人登记过的键）。
    QAction *now = namedAction(w, "actionCaptureNow");
    check(now && !now->shortcut().isEmpty(), "★ 「立即截图」带着快捷键");

    const QStringList hotkeyCandidates = MainWindow::captureHotkeyCandidates();
    check(!hotkeyCandidates.isEmpty()
              && hotkeyCandidates.first() == QLatin1String(MainWindow::kCaptureHotkey),
          "★ 候选表第一项就是 kCaptureHotkey（产品定的那一个）");

    const QString shownShortcut =
        now ? now->shortcut().toString(QKeySequence::PortableText) : QString();
    check(hotkeyCandidates.contains(shownShortcut),
          "★ 菜单上显示的快捷键是候选表里的某一个（菜单写的 = 能按的）");
    if (!hotkeyCandidates.contains(shownShortcut)) {
        std::printf("       菜单上写的是「%s」，候选表是 %s\n",
                    qUtf8Printable(shownShortcut),
                    qUtf8Printable(hotkeyCandidates.join(QStringLiteral(" / "))));
    }

    QStringList withShortcut;
    for (const char *n : { "actionCaptureDelay3", "actionCaptureDelay5",
                           "actionCaptureDelay10" }) {
        QAction *a = namedAction(w, n);
        if (a && !a->shortcut().isEmpty())
            withShortcut << a->objectName();
    }
    check(withShortcut.isEmpty(), "★ 三个延迟项都不带快捷键");
    if (!withShortcut.isEmpty())
        std::printf("       不该有快捷键：%s\n", qUtf8Printable(withShortcut.join(QStringLiteral(", "))));

    // ── ④ 全局热键对象：要么登记成功，要么给出原因 ──
    //   ★ 这条断言的形式是刻意的 ★
    //     "Ctrl+Alt+S 是否登记得上"取决于**这台机器上同时跑着什么程序**
    //     （QQ / 微信 / 各种截图工具都会抢这个键），拿它当成功判据会做出
    //     一个随环境时红时绿的自检 —— 那类假失败最耗人。
    //     真正与环境无关的不变量是：**绝不允许静默失败**。
    //     机制本身的成功路径在下面 [19] 组用"几乎不可能被占用"的键验。
    GlobalHotkey *hk = w->globalHotkey();
    check(hk != nullptr, "★ 建出了全局热键对象（登记失败也不许是 nullptr）");
    if (hk) {
        const bool reg = hk->isRegistered(MainWindow::kCaptureHotkeyId);
        check(reg || !hk->lastError().isEmpty(),
              "★★ 全局快捷键要么登记成功、要么把原因说出来（绝不静默失败）");

        // ★★ 本组最值钱的一条：菜单上写的 == 真正向系统登记上的 ★★
        //   回落到别的键时这条才是真的会抓 bug：菜单如果还显示首选键，
        //   用户拿到的就是一把**错的钥匙** —— 他会一直按 Ctrl+Alt+S，
        //   然后认为整个功能坏了。而这条不可能靠"看代码"发现。
        const QKeySequence effective =
            hk->registeredSequence(MainWindow::kCaptureHotkeyId);
        if (reg) {
            checkStr(now ? now->shortcut().toString(QKeySequence::PortableText) : QString(),
                     effective.toString(QKeySequence::PortableText),
                     "★★ 菜单上写的 == 真正登记上的键（回落时也得成立）");
        }

        if (reg) {
            const bool primary = (effective.toString(QKeySequence::PortableText)
                                  == QLatin1String(MainWindow::kCaptureHotkey));
            std::printf("       全局快捷键：%s（登记成功%s）\n",
                        qUtf8Printable(effective.toString(QKeySequence::NativeText)),
                        primary ? "" : "；首选键被别的程序占用，已回落到它");
        } else {
            std::printf("       全局快捷键：%s —— 候选表一个都没登记上：%s\n",
                        MainWindow::kCaptureHotkey,
                        qUtf8Printable(hk->lastError()));
        }
    }

    delete w;
}

// ===========================================================================
//  [19] 全局快捷键：登记 / 系统消息通路 / 注销
// ===========================================================================
//
//  ★ 本组最重要的一条是"系统消息真的能走到我们的代码里" ★
//    登记成功只证明"系统收下了这个请求"；真正没把握的是**后半段**：
//    WM_HOTKEY 是投给**线程**的消息（不带窗口句柄），Qt 的事件循环会不会
//    把它交到 nativeEventFilter 手上，并没有明确承诺。
//    这条通路不通的话，症状就是"热键登记着、按下去毫无反应"—— 而且
//    只在真机上按才暴露。所以这里造一条真的 WM_HOTKEY 投进本线程队列。

static void testGlobalHotkey()
{
    beginGroup("全局快捷键：登记 / 系统消息通路 / 注销");

    constexpr int kId = 7;

    GlobalHotkey hk;

    // ── ① 登记：连试几个"几乎不可能被占用"的组合 ──
    //   ★ 为什么不用产品那个 Ctrl+Alt+S ★
    //     本组要验的是**机制**，不是某几个特定的键。用产品键的话，
    //     开发机上真被占用了就会红，而那是假失败。
    //     Ctrl+Alt+Shift 三修饰 + F13 及以上：只有扩展键位的键盘才发得出，
    //     全系统几乎不可能有程序登记它。
    const char *candidates[] = { "Ctrl+Alt+Shift+F13", "Ctrl+Alt+Shift+F14",
                                 "Ctrl+Alt+Shift+F15", "Ctrl+Alt+Shift+F16" };
    bool reg = false;
    QString why;
    for (const char *c : candidates) {
        if (hk.registerSequence(QKeySequence(QLatin1String(c)), kId)) {
            reg = true;
            break;
        }
        why = hk.lastError();
    }

    check(reg, "★ 登记一个几乎不会被占用的组合键 → 成功");
    if (!reg) {
        std::printf("       登记失败原因：%s\n", qUtf8Printable(why));
        check(!why.isEmpty(), "就算失败也必须带着原因");
        check(!hk.isRegistered(kId), "失败之后 isRegistered 为假");
        return;   // 后面的通路断言必须建立在"登记着"的前提下
    }
    check(hk.isRegistered(kId), "登记成功后 isRegistered 为真");
    check(hk.lastError().isEmpty(), "成功后不带错误信息");

    // ── ② ★ 系统消息 → 信号：这条通路必须真的通 ★ ──
    int    gotId = -1;
    int    count = 0;
    QObject::connect(&hk, &GlobalHotkey::activated, [&](int id) {
        gotId = id;
        ++count;
    });

    check(hk.simulateActivation(kId), "把一条 WM_HOTKEY 投进本线程消息队列");
    pumpUntil([&] { return count > 0; }, 500);

    checkEqInt(count, 1, "★★ 系统消息走通：WM_HOTKEY → nativeEventFilter → activated()");
    checkEqInt(gotId, kId, "信号带来的 id 就是登记时用的那一个");

    // ── ③ 没登记过的 id：什么都不该发生 ──
    //   ★ 这条防的是"来了消息就发信号"这种粗糙实现 ★
    //     同一个进程里将来可能有第二个热键使用者，靠 id 才分得清。
    hk.simulateActivation(kId + 100);
    pumpUntil([&] { return count > 1; }, 200);
    checkEqInt(count, 1, "★ 没登记过的 id 不触发任何东西");

    // ── ④ 注销之后不再响应 ──
    hk.unregister(kId);
    check(!hk.isRegistered(kId), "注销后 isRegistered 为假");

    hk.simulateActivation(kId);
    pumpUntil([&] { return count > 1; }, 200);
    checkEqInt(count, 1, "★ 注销之后同一条消息不再触发");

    // ── ⑤ 不合法的组合键：被拒绝**并且**给出能读的原因 ──
    //   ★ 这一条比"返回 false"重要得多 ★
    //     上层要靠这句话去告诉用户"换个键"，空字符串等于把问题藏起来。
    check(!hk.registerSequence(QKeySequence(), kId + 1), "空组合键被拒绝");
    check(!hk.lastError().isEmpty(), "★ 空组合键：给出了原因");

    check(!hk.registerSequence(QKeySequence(QStringLiteral("F9")), kId + 2),
          "★ 不带修饰键的单键被拒绝（否则全系统那个键都被我们吃掉）");
    check(!hk.lastError().isEmpty(), "★ 单键：给出了原因");

    check(!hk.registerSequence(QKeySequence(QStringLiteral("Ctrl+Alt+MediaPlay")), kId + 3),
          "★ 不支持的主键被拒绝（宁可不支持，也不登记到一个别的键上）");
    check(!hk.lastError().isEmpty(), "★ 不支持的主键：给出了原因");

    check(!hk.isRegistered(kId + 1) && !hk.isRegistered(kId + 2) && !hk.isRegistered(kId + 3),
          "被拒绝的三个都没进登记表");
}

// ===========================================================================
//  [20] 屏幕截图：立即 / 延迟 / 全局热键（三条入口）
// ===========================================================================
//
//  ★ 为什么要注入抓屏器 ★
//    自检跑在 offscreen 平台上，那里**没有真实屏幕**可抓 —— 不注入的话，
//    这三条入口在这里全都只能"优雅失败"，什么都验不到。
//    注入只换掉"图从哪来"这一步，其余代码路径一行不少：
//    菜单动作 / 定时器 / 全局热键信号 / 建页 / 还原窗口。

/// 核对"最新那一页"就是这张图变成的（尺寸 / 像素 / 标题 / 干净状态）。
static void checkLastPageIs(const DocumentTabs *tabs, const QImage &shot,
                            const QString &expectTitle)
{
    const int idx = tabs->count() - 1;
    Document *doc = tabs->tabAt(idx) ? tabs->tabAt(idx)->doc : nullptr;

    check(doc != nullptr, "★ 多出了一页");
    if (!doc)
        return;

    checkEqInt(doc->canvasSize().width(),  shot.width(),  "页宽 == 截图宽");
    checkEqInt(doc->canvasSize().height(), shot.height(), "页高 == 截图高");
    checkEqInt(doc->layerCount(), 1, "只有一层");

    Layer *l0 = doc->layers().layer(0);
    check(l0 != nullptr && l0->size() == shot.size(), "图层尺寸 == 截图尺寸");
    if (l0)
        checkEqInt(diffPixelCount(l0->image(), shot), 0,
                   "★★ 图层像素与截下来的图逐点相同（0 容差）");

    check(doc->filePath().isEmpty(), "没有文件路径（截图不落盘）");
    check(!doc->isModified(), "不算已修改（关它一下不该问「要不要保存」）");
    checkStr(doc->titleHint(), expectTitle, "带上了提示名（屏幕截图-N）");
    checkStr(tabs->tabTitle(idx), expectTitle,
             "★ 标签标题就是提示名，不是「未命名-N」");
    check(tabs->currentTab() && tabs->currentTab()->doc == doc, "★ 新页是当前页");
    check(tabs->tabAt(idx)->stack && tabs->tabAt(idx)->stack->isClean(), "撤销栈是干净的");
}

static void testScreenCaptureFlow()
{
    beginGroup("屏幕截图：立即 / 延迟 / 全局热键（注入抓屏器）");

    MainWindow *w = newWindow(QStringLiteral("move"));
    DocumentTabs *tabs = tabsOf(w);
    check(tabs != nullptr, "拿得到标签容器");
    if (!tabs) {
        delete w;
        return;
    }

    // 注入"抓屏"这一步（offscreen 平台没有真实屏幕）
    int grabCount = 0;
    const QImage shot = makeCaptureTestImage(37, 23);
    w->setScreenGrabber([&grabCount, shot] { ++grabCount; return shot; });

    const int base = tabs->count();

    // ── 本地序号账本 ──
    //   ★ 为什么不用字面量 "Screenshot-3" / "-4" ★
    //     ③ 那一节**可能整段跑不到** —— 本机就有别的程序占着 Ctrl+Alt+S 的时候
    //     （实测遇到过：同一台机器上前后两次全量，一次登记成功、一次被占用）。
    //     写死字面量的话，后面每一处序号都会跟着错位，那是把"环境差异"
    //     伪装成"功能坏了" —— 最难查的一类假红。
    //     所以这里按"成功截了几次"记账：③ 跑不跑，后面的期望值都自洽。
    int seq = 0;
    const auto nextTitle = [&seq] { return QStringLiteral("Screenshot-%1").arg(++seq); };

    // ── ① 立即截图：走菜单动作 ──
    QAction *actNow = namedAction(w, "actionCaptureNow");
    check(actNow != nullptr, "拿得到「立即截图」动作");
    if (actNow)
        actNow->trigger();
    QApplication::processEvents();

    checkEqInt(grabCount, 1, "触发一次只抓一次屏");
    checkEqInt(tabs->count(), base + 1, "★ 立即截图开出了新的一页");
    checkLastPageIs(tabs, shot, nextTitle());

    // ── ② 延迟截图：先最小化 → 到点抓 → 抓完还原 ──
    const int beforeDelay  = tabs->count();
    const int grabsBefore  = grabCount;

    w->scheduleScreenCapture(60);
    checkEqInt(w->pendingCaptureDelayMs(), 60, "排上了（剩余延时记录在案）");
    check(w->isMinimized(), "★★ 延迟期间本窗口已最小化（否则截到的是自己）");

    // ★ 重复排 = 重新计时 ★
    //   否则连点三次"延迟 3 秒"会攒出三张图，而用户想要的显然只有一张。
    w->scheduleScreenCapture(120);
    checkEqInt(w->pendingCaptureDelayMs(), 120, "★ 重复排 = 重新计时，不是排队");

    check(pumpUntil([&] { return w->pendingCaptureDelayMs() < 0; }, 3000),
          "定时器到点了");
    checkEqInt(grabCount, grabsBefore + 1, "★ 只抓了一次（排两次 ≠ 抓两次）");
    checkEqInt(tabs->count(), beforeDelay + 1, "★ 延迟截图也开出了新的一页");
    checkLastPageIs(tabs, shot, nextTitle());
    // ★ 这条同时也是"连排两次"那个 bug 的守门人 ★
    //   第二次排的时候，窗口正因为第一次而处于最小化；如果那时重新判定
    //   "是不是我们干的"，标记会被抹成 false —— 抓完就没人负责还原，
    //   程序看起来消失了。所以这条断言钉的是"连排之后也必须还原"。
    check(!w->isMinimized(), "★★ 抓完自动还原窗口（连排两次也必须还原）");

    // ── ③ 全局热键那条路：系统消息 → 信号 → 截图 → 新页 ──
    //   只有登记成功才走得通（登记失败的原因与提示见上一组）。
    GlobalHotkey *hk = w->globalHotkey();
    if (hk && hk->isRegistered(MainWindow::kCaptureHotkeyId)) {
        const int beforeHot = tabs->count();
        const int grabsHot  = grabCount;

        check(hk->simulateActivation(MainWindow::kCaptureHotkeyId),
              "投一条 WM_HOTKEY（等同于用户按下了那个组合键）");
        pumpUntil([&] { return tabs->count() > beforeHot; }, 1000);

        checkEqInt(grabCount, grabsHot + 1, "★ 热键触发了抓屏");
        checkEqInt(tabs->count(), beforeHot + 1,
                   "★★ 全局热键 → 信号 → 截图 → 新页（整条链路走通）");
        checkLastPageIs(tabs, shot, nextTitle());
    } else {
        // ★ 不静默跳过 ★：把"这一段没跑到"说出来，否则覆盖率掉了没人知道。
        //   同时说清"这是环境差异、不是功能坏了" —— 免得看日志的人去查代码。
        //   ★ 注意这一段已经比原来难走到了 ★
        //     候选表会自动回落，所以走到 else 意味着**整张表都被占着**
        //     （或平台不支持），而不是"首选键被占"这一种常见情况。
        std::printf("       跳过热键那一节：候选表里的键一个都没登记上"
                    "（原因见上一组），属环境差异。\n");
    }

    // ── ④ 序号只增不减 ──
    //   关掉当前这张再截一张，编号继续往后走而不是回到 -2：
    //   编号复用会让"我记得刚才那张叫 -3"变成一句没法验证的话。
    //
    //   ★ 这一组**必须与 ③ 是否跑到无关** ★
    //     基准取"关之前有几页"，而不是 base + 常量 —— 后者等于断言"③ 一定成功"，
    //     一有环境差异就红（本次全量就是这么红的，3 条失败全部源于此）。
    {
        const int beforeClose = tabs->count();
        QAction *actClose = namedAction(w, "actionClose");
        if (actClose)
            actClose->trigger();   // 未修改的页 → 不会弹"要不要保存"（不挂死）
        QApplication::processEvents();
        checkEqInt(tabs->count(), beforeClose - 1, "关掉一张之后剩下的页数对得上");
    }

    {
        const int afterClose = tabs->count();
        w->captureScreenNow();
        QApplication::processEvents();
        checkEqInt(tabs->count(), afterClose + 1, "关掉之后再截一张 → 又是一页");
        checkLastPageIs(tabs, shot, nextTitle());
    }

    // ── ⑤ 抓不到屏幕：什么都不做 + 返回 false ──
    //   ★ 这条是"静默失败"的守门人 ★
    //     没有显示器 / 会话被锁时，正确行为是**不开空页、不换当前页**；
    //     否则用户会看到一张 0×0 的空白标签，然后完全不知道发生了什么。
    {
        const int beforeNull = tabs->count();
        Document *curBefore  = tabs->currentTab() ? tabs->currentTab()->doc : nullptr;

        w->setScreenGrabber([] { return QImage(); });
        check(!w->captureScreenNow(), "抓屏返回空图 → 返回 false");
        checkEqInt(tabs->count(), beforeNull, "★ 没有多出任何页（不开空页）");
        check(tabs->currentTab() && tabs->currentTab()->doc == curBefore,
              "★ 当前页也没被换掉");
    }

    // ── ⑥ 失败文案：Wayland 那句必须"能照着做" ──
    //
    //   ★ 这条钉的是"提示有没有用"，不是"有没有提示" ★
    //     旧版只有一句"当前会话不允许抓屏"，用户看完仍然不知道该怎么办。
    //     所以断言里必须有**可执行的下一条路**：系统截图键 + Ctrl+V。
    //     这也是唯一能验的部分 —— 弹框本身在离屏环境里验不了（会挂死），
    //     所以文案被抽成静态函数，这里直接量字符串。
    {
        const QString wayland =
            MainWindow::captureFailureText(zl::CaptureBlock::Wayland);
        check(!wayland.isEmpty(), "★★ Wayland 失败文案非空");
        check(wayland.contains(QStringLiteral("Wayland")),
              "★ 文案点名了 Wayland（用户才知道不是程序坏了）");
        check(wayland.contains(QStringLiteral("Ctrl+V")),
              "★★ 文案给出了可照做的下一步（Ctrl+V 粘贴）");
        check(wayland.contains(QStringLiteral("system shortcut"))
                  || wayland.contains(QStringLiteral("system screenshot key")),
              "★ 文案提到用系统快捷键截图（否则用户不知道图从哪来）");

        const QString other =
            MainWindow::captureFailureText(zl::CaptureBlock::None);
        check(!other.isEmpty(), "★ 一般失败文案非空");
        check(other != wayland,
              "★★ 两种失败说法不同（否则用户会去 Wayland 之外的地方找 Ctrl+V）");
        check(!other.contains(QStringLiteral("Ctrl+V")),
              "★ 一般失败不提 Ctrl+V —— 那不是这次的解法");

        check(!MainWindow::captureFailureTitle().isEmpty(), "★ 弹框标题非空");
    }

    // ── ⑦ 失败时"弹框开关"确实生效，且关掉后**不会挂住测试** ──
    //
    //   ★ 上面 ⑤ 已经证明了"关着弹框时返回 false、不建页" ★
    //     这一节只补一条：开关的**默认值**必须是"开" —— 产品里要弹框，
    //     如果哪天有人把默认值写成 false，用户就又回到"点了没反应"。
    {
        MainWindow *fresh = new MainWindow;
        check(fresh->screenCaptureDialogsEnabled(),
              "★★ 弹框开关默认是开的（否则产品里又变成静默失败）");
        fresh->setScreenCaptureDialogsEnabled(false);
        check(!fresh->screenCaptureDialogsEnabled(), "开关能关");
        fresh->setScreenCaptureDialogsEnabled(true);
        check(fresh->screenCaptureDialogsEnabled(), "开关能再开");
        delete fresh;
    }

    delete w;
}

// ===========================================================================
//  [21] 从 QImage 建页（createImagePage）
// ===========================================================================
//
//  这是"图 → 一页"的唯一实现，「文件 → 打开」与屏幕截图都走它。
//  所以这一组同时也是那次重构的回归：路径、提示名、图层名、干净状态
//  这几条规则必须仍然成立。

static void testCreateImagePage()
{
    beginGroup("从 QImage 建页：尺寸 / 像素 / 标题 / 图层名 / 干净状态");

    MainWindow *w = newWindow(QStringLiteral("move"));
    DocumentTabs *tabs = tabsOf(w);
    if (!tabs) {
        delete w;
        return;
    }

    const int before = tabs->count();
    const QImage img = makeCaptureTestImage(41, 17);

    MainWindow::ImagePageSpec spec;
    spec.image     = img;
    spec.layerName = QStringLiteral("Screenshot");
    spec.titleHint = QStringLiteral("Screenshot-9");

    Document *doc = w->createImagePage(spec);
    check(doc != nullptr, "建出了一页");
    checkEqInt(tabs->count(), before + 1, "页数 +1");
    check(tabs->currentTab() && tabs->currentTab()->doc == doc,
          "★ 新页就是当前页（建完要切过去）");

    if (doc) {
        checkEqInt(doc->canvasSize().width(),  41, "画布宽 == 图像宽");
        checkEqInt(doc->canvasSize().height(), 17, "画布高 == 图像高");
        checkEqInt(doc->layerCount(), 1, "只有一层");
        check(doc->filePath().isEmpty(), "没有文件路径");

        checkStr(doc->titleHint(), QStringLiteral("Screenshot-9"), "带上了显示名提示");

        // ★ displayName 与 tabTitle 是**两个**消费者 ★
        //   只断言其中一个的话，另一个忘了用提示名照样绿 ——
        //   而用户看到的恰好是后者（标签页上那一行）。
        checkStr(doc->displayName(), QStringLiteral("Screenshot-9"),
                 "★ displayName 用的是提示名，不是「Untitled」");
        checkStr(tabs->tabTitle(tabs->currentIndex()), QStringLiteral("Screenshot-9"),
                 "★★ 标签标题用的也是提示名（无路径文档的标题来自 titleHint）");

        check(!doc->isModified(), "不算已修改");

        Layer *l0 = doc->layers().layer(0);
        check(l0 != nullptr && l0->name() == QStringLiteral("Screenshot"),
              "图层 0 用了传进来的名字");
        if (l0) {
            checkEqInt(diffPixelCount(l0->image(), img), 0,
                       "★★ 像素逐点相同（0 容差，含那格半透明）");
            check(l0->image().format() == QImage::Format_ARGB32_Premultiplied,
                  "图层是预乘格式（合成算法的前提）");
        }
    }

    // ── 反向：空图必须被拒绝，且不能留下半件事 ──
    //   建出一个 0×0 的页比"失败"更难查：它看起来像程序坏了，
    //   而且用户没法在上面做任何事。
    {
        const int n = tabs->count();
        MainWindow::ImagePageSpec bad;
        check(w->createImagePage(bad) == nullptr, "★ 空图被拒绝");
        checkEqInt(tabs->count(), n, "★ 拒绝之后页数没变（没留下半件事）");
    }

    // ── 不带路径也不带提示名：回落到"未命名-N" ──
    //   ★ 这条钉的是"提示名是**可选**的" ★
    //     它一旦变成必填，所有旧路径（新建 / 打开的空白页）都得跟着改。
    {
        MainWindow::ImagePageSpec plain;
        plain.image = makeCaptureTestImage(9, 9);

        Document *p = w->createImagePage(plain);
        check(p != nullptr, "既没有路径也没有提示名 → 仍然能建页");
        if (p) {
            check(p->titleHint().isEmpty(), "提示名是空的");
            checkStr(p->displayName(), QStringLiteral("Untitled"),
                     "★ displayName 回落到「Untitled」");
            check(tabs->tabTitle(tabs->currentIndex()).startsWith(QStringLiteral("Untitled-")),
                  "★ 标签标题回落到「Untitled-N」");
            Layer *l0 = p->layers().layer(0);
            check(l0 != nullptr && l0->name() == QStringLiteral("Background"),
                  "没给图层名时保留 Document 的默认名");
        }
    }

    delete w;
}

// ---------------------------------------------------------------------------
//  界面语言（core/UiLanguage）
// ---------------------------------------------------------------------------
//  ★ 本组在"没有 .qm"的环境下运行 ★
//    翻译资源只由主程序内嵌（构建期 lrelease → qrc），测试程序不编它。
//    于是可用语言表里只会出现源码语言 —— 这正是要钉的行为：
//    .qm 缺失时不许把"半语言"暴露给用户（语言菜单会因此整个不出现，
//    见 MainWindowActions.cpp 的 availableLanguages().size() > 1 门槛）。
static void testUiLanguage()
{
    beginGroup("界面语言：决策 / 持久化 / 装载");

    const QString src = uilang::sourceLanguage();
    const QStringList avail = uilang::availableLanguages();
    const QString autoCode = uilang::autoLanguage();

    check(avail.contains(src), "★ 源码语言永远在可用列表里（.qm 缺失也有兜底）");
    check(avail.indexOf(src) == 0, "★ 源码语言排在第一位（默认项）");
    check(!src.isEmpty(), "★ 源码语言非空");
    check(!avail.contains(autoCode),
          "★ auto 是哨兵值不是语言，永远不进可用列表");

    // ── 默认状态：没选过 = 自动 ──
    //   先清键，模拟一个新用户。不清的话断言会随执行顺序飘
    //   （上一轮留下的值决定这一轮看到什么）。
    {
        QSettings s;
        s.remove(QStringLiteral("ui/language"));
        s.sync();
    }
    checkStr(uilang::languagePreference(), autoCode,
             "★ 从未选过 → 偏好是 auto（语言菜单第一项的勾选态）");
    // 不同机器 locale 不同（中文环境给 zh_CN、其余给 en），结论可能不同；
    // 但**一定落在可用表里** —— 这条才是与机器无关的判据。
    check(avail.contains(uilang::currentLanguage()),
          "★ 自动 → locale 推断结果一定在可用表里（钳制有效）");

    // ── 显式选"自动"：偏好落盘，生效语言仍按 locale ──
    //   这一条锁的是"自动"作为一个**可选状态**存在，而不是"没选过"的
    //   另一种写法 —— 菜单里用户主动点"自动"必须留下痕迹。
    uilang::setCurrentLanguage(autoCode);
    checkStr(uilang::languagePreference(), autoCode,
             "★ 显式选自动 → 偏好读回 auto");
    check(avail.contains(uilang::currentLanguage()),
          "★ 显式选自动 → 生效语言仍在可用表里");
    {
        QSettings s;
        checkStr(s.value(QStringLiteral("ui/language")).toString(), autoCode,
                 "★ 自动也落进 QSettings（菜单勾选态因此可重建）");
    }

    // ── 显式选具体语言：写进去的原样读出来 ──
    uilang::setCurrentLanguage(src);
    checkStr(uilang::languagePreference(), src, "★ 显式选择后偏好原样读回");
    checkStr(uilang::currentLanguage(), src, "★ 生效语言跟随偏好");
    {
        QSettings s;
        checkStr(s.value(QStringLiteral("ui/language")).toString(), src,
                 "★ 选择真的落进了 QSettings（键名 ui/language）");
    }

    // ── 拒绝不可用语言 ──
    //   这条防的是"下次启动拿着一个不存在的语言，界面静默回落到
    //   源码文本，用户以为切换失败了"——而且没有任何地方能查。
    const QString bogus = QStringLiteral("xx_XX");
    check(!avail.contains(bogus), "★ xx_XX 不在可用列表里（测试前提）");
    uilang::setCurrentLanguage(bogus);
    checkStr(uilang::languagePreference(), src,
             "★ 拒绝不可用语言：偏好不动（不写，不是写了再钳制）");
    {
        QSettings s;
        checkStr(s.value(QStringLiteral("ui/language")).toString(), src,
                 "★ 设置里没有留下 xx_XX");
    }

    // ── 装载 ──
    check(!uilang::needsTranslation(src), "★ 源码语言不需要 .qm");
    // 路线 B 翻转后：非源码语言（zh_CN）是翻译目标，需要 .qm
    if (avail.size() > 1)
        check(uilang::needsTranslation(avail.at(1)),
              "★ 非源码语言需要 .qm（源码英文 + 内嵌翻译）");
    check(!uilang::installTranslator(qApp, src),
          "★ 源码语言不安装翻译器：返回 false 且不留已装的翻译");

    // 恢复现场：本组写过的设置清干净，别影响后面的组
    //（AppInfo 的标题断言会读 currentLanguage()）。
    {
        QSettings s;
        s.remove(QStringLiteral("ui/language"));
        s.sync();
    }
}

// ---------------------------------------------------------------------------
//  语言菜单装配（阶段 2 第一批后：菜单常驻，第一项是"自动"）
// ---------------------------------------------------------------------------
//  ★ 为什么值得单独立一组 ★
//    阶段 1 的语言菜单被 `availableLanguages().size() > 1` 门槛挡着
//    （当时只有一种语言，菜单不出现）。源码翻成 en 之后可用语言变成
//    {en, zh_CN}，菜单理应常驻 —— 但"理应"二字正是静默失效的温床：
//    门槛代码一天还在，菜单就一天可能不出现，而双构建 0 错 0 警。
//    这里直接把菜单结构和勾选态钉死。
//
//  ★ 勾选态看"偏好"而不是"生效语言" ★
//    自动项在 locale 推断出某种语言时依然勾中 —— 那正是用户选
//    "自动"的含义。构造两个不同偏好的窗口来验证两种勾选态。
//  ★ 不用 newWindow() ★
//    它内部有 s.clear()，会把刚设好的 ui/language 一起擦掉 —— 而语言
//    菜单的勾选态是在 MainWindow 构造时从 QSettings 读的，擦了就等于
//    测到"没选过"的状态。这里直接构造，只动 ui/language 一个键。
static MainWindow *newWindowForLanguageMenu()
{
    auto *w = new MainWindow;
    // 离屏环境不弹截图失败的模态框（理由见 newWindow()）
    w->setScreenCaptureDialogsEnabled(false);
    w->resize(1000, 700);
    w->show();
    QApplication::processEvents();
    return w;
}

static void testLanguageMenuAssembly()
{
    beginGroup("语言菜单：装配与勾选态");

    const QString src = uilang::sourceLanguage();
    const int expectItems = uilang::availableLanguages().size() + 1;  // + 自动项

    // 偏好 = 自动 → 自动项勾中
    {
        QSettings s;
        s.setValue(QStringLiteral("ui/language"), uilang::autoLanguage());
        s.sync();
    }
    MainWindow *wAuto = newWindowForLanguageMenu();
    if (QMenu *lm = wAuto->findChild<QMenu *>(QStringLiteral("menuLanguage"))) {
        check(lm->actions().size() == expectItems,
              "★ 菜单项数 = 1（自动）+ 可用语言数");
        check(!lm->actions().isEmpty() && lm->actions().first()->isCheckable(),
              "★ 第一项是「自动」，可勾选（互斥单选组）");
        check(!lm->actions().isEmpty() && lm->actions().first()->isChecked(),
              "★ 偏好=自动 → 自动项勾中");
    } else {
        check(false, "★ 语言菜单常驻显示（menuLanguage，size()>1 门槛已拆）");
    }
    delete wAuto;

    // 偏好 = 具体语言 → 自动项不勾、对应语言项勾中
    {
        QSettings s;
        s.setValue(QStringLiteral("ui/language"), src);
        s.sync();
    }
    MainWindow *wSrc = newWindowForLanguageMenu();
    if (QMenu *lm = wSrc->findChild<QMenu *>(QStringLiteral("menuLanguage"))) {
        check(!lm->actions().isEmpty() && !lm->actions().first()->isChecked(),
              "★ 偏好=具体语言 → 自动项不勾");
        int checkedIdx = -1;
        const QList<QAction *> acts = lm->actions();
        for (int i = 1; i < acts.size(); ++i) {
            if (acts[i]->isChecked())
                checkedIdx = i;
        }
        check(checkedIdx >= 1, "★ 偏好=具体语言 → 有且仅有一项语言被勾中");
    } else {
        check(false, "★ 语言菜单常驻显示（menuLanguage）");
    }
    delete wSrc;

    // 垃圾偏好（用户手改过设置文件，写了个不存在的语言代码）→ 按"自动"
    // 兜底勾中。currentLanguage() 对垃圾偏好同样落自动分支，菜单的勾选态
    // 必须跟这个口径一致 —— 否则出现"菜单一项都没勾、程序却说在按 locale
    // 推断"的对不上。setCurrentLanguage 本身拒绝写垃圾（testUiLanguage 已验），
    // 这里验的是"万一设置文件被外部改花了"的显示兜底。
    {
        QSettings s;
        s.setValue(QStringLiteral("ui/language"), QStringLiteral("xx_XX"));
        s.sync();
    }
    MainWindow *wBogus = newWindowForLanguageMenu();
    if (QMenu *lm = wBogus->findChild<QMenu *>(QStringLiteral("menuLanguage"))) {
        check(!lm->actions().isEmpty() && lm->actions().first()->isChecked(),
              "★ 垃圾偏好 → 自动项兜底勾中（菜单不会全空）");
    } else {
        check(false, "★ 语言菜单常驻显示（menuLanguage）");
    }
    delete wBogus;

    // 恢复现场
    {
        QSettings s;
        s.remove(QStringLiteral("ui/language"));
        s.sync();
    }
}

// ---------------------------------------------------------------------------
//  [7c] 主题菜单：装配 / 互斥 / 真的生效 / 持久化
// ---------------------------------------------------------------------------
//
//  ★ 为什么这组必须存在 ★
//    主题有三个环节，每一个失效都是静默的：
//      ① 菜单没接上 → 用户找不到入口（对账脚本只看"有没有图标"，看不到
//         三项是否在菜单里、是否互斥）；
//      ② 接了但没真的换 palette → 勾选态变了、界面纹丝不动；
//      ③ 换了但没落盘 → 下次启动回到默认，用户以为设置不记住。
//    ②③ 都不会报错，只能靠"点一下再量"。
//
//  ★ 判据是 palette 与图标像素，不是 isChecked ★
//    "勾上了"只说明 QActionGroup 工作了，不代表配色真的换了 ——
//    这两件事必须分开验，否则一个只改了勾选态的假实现也能全绿。
//
//  ★ 同时钉住"自动"能还原 ★
//    auto 不是第四套配色，是"撤销到系统原样"。这条最容易在重构里丢掉
//    （例如把 auto 也做成一套写死的浅色），所以要断样式也回到原值。
static void testThemeMenu()
{
    beginGroup("主题菜单：装配 / 互斥单选 / 切换真的生效 / 持久化");

    const QPalette systemPalette = QApplication::palette();
    const QString systemStyle =
        QApplication::style() ? QApplication::style()->objectName() : QString();

    QSettings s;
    s.remove(QStringLiteral("ui/theme"));
    s.sync();

    MainWindow *w = newWindow(QStringLiteral("move"));
    QMenu *tm = w->findChild<QMenu *>(QStringLiteral("menuTheme"));

    check(tm != nullptr, "文件菜单里有「主题」子菜单（menuTheme）");
    if (!tm) {
        delete w;
        return;
    }

    // ── 装配：三项、顺序、互斥、默认勾"自动" ──
    const QList<QAction *> acts = tm->actions();
    check(acts.size() == 3, "★ 恰好三项（默认 / 浅色 / 深色）");
    check(acts.size() == 3
              && acts[0]->data().toInt() == int(uitheme::Mode::Auto)
              && acts[1]->data().toInt() == int(uitheme::Mode::Light)
              && acts[2]->data().toInt() == int(uitheme::Mode::Dark),
          "★ 顺序是 默认 / 浅色 / 深色，且 data() 存的是 Mode");
    check(acts.size() == 3 && acts[0]->isChecked(), "★ 未设置过时勾「默认」");
    bool allCheckable = true;
    for (QAction *a : acts)
        if (!a->isCheckable())
            allCheckable = false;
    check(allCheckable, "三项都可勾选（互斥组）");

    QAction *aNew = w->findChild<QAction *>(QStringLiteral("actionNew"));
    check(aNew != nullptr, "拿得到一个带图标的菜单动作");
    if (!aNew) {
        delete w;
        return;
    }

    // ── 切深色：palette 真的变深、图标真的变浅 ──
    acts[2]->trigger();
    QApplication::processEvents();
    check(QApplication::palette().color(QPalette::Window).lightness() < 128,
          "★ 切深色后窗口底色真的变深");
    check(uitheme::isDark(), "★ 深色判据跟着亮（图标上色依赖它）");
    const QRgb darkInk = sampleOpaqueInk(aNew->icon());
    check(qRed(darkInk) > 0xC0, "★ 深色下图标被染成浅色");
    if (!(qRed(darkInk) > 0xC0))
        std::printf("       实测深色图标=%08x\n", darkInk);

    // ── 切浅色：反向，且只有一项勾中 ──
    acts[1]->trigger();
    QApplication::processEvents();
    check(QApplication::palette().color(QPalette::Window).lightness() >= 128,
          "★ 切浅色后窗口底色真的变浅");
    const QRgb lightInk = sampleOpaqueInk(aNew->icon());
    check(qRed(lightInk) < 0x80, "★ 浅色下图标被染成深色");
    int checkedCount = 0;
    for (QAction *a : acts)
        if (a->isChecked())
            ++checkedCount;
    check(checkedCount == 1 && acts[1]->isChecked(),
          "★ 互斥：只有「浅色」一项勾着");
    check(uitheme::preference() == uitheme::Mode::Light,
          "★ 选择已写进偏好（落盘）");

    // ── 落盘要能跨实例读回来（不只是同一个进程里的内存） ──
    {
        QSettings again;
        check(again.value(QStringLiteral("ui/theme")).toString()
                  == uitheme::keyOf(uitheme::Mode::Light),
              "★ QSettings 里存的是 light（重启后才记得住）");
    }

    // ── 自动：撤销回系统配色与系统样式 ──
    acts[0]->trigger();
    QApplication::processEvents();
    check(QApplication::palette().color(QPalette::Window)
              == systemPalette.color(QPalette::Window),
          "★ 选「默认」后底色回到系统原值");
    // ★ 覆盖必须被**整个清掉**，而不是只清一部分 ★
    //   ⚠ offscreen 平台没有实时系统配色，"清空覆盖"与"塞回启动快照"在这里
    //   结果相同（都是平台默认），光靠断言分不出来，所以多点采样几个角色，
    //   至少能抓住"只清了 Window 而漏了其余"这类写一半的还原。
    //   真正的差别在 Windows 上人工验：系统转深色 → 点默认 → 界面应变深
    //   （快照版会退回浅色，跟系统正好相反）。
    check(QApplication::palette().color(QPalette::Highlight)
              == systemPalette.color(QPalette::Highlight)
          && QApplication::palette().color(QPalette::Base)
              == systemPalette.color(QPalette::Base)
          && QApplication::palette().color(QPalette::WindowText)
              == systemPalette.color(QPalette::WindowText),
          "★ 其余角色（强调色 / 底色 / 正文）也一并还原，不是只还原窗口色");
    check(QApplication::style()
              && QApplication::style()->objectName().compare(
                     systemStyle, Qt::CaseInsensitive) == 0,
          "★ 选「默认」后样式也回到系统原样式（不是停在 Fusion）");
    check(uitheme::preference() == uitheme::Mode::Auto,
          "★ 偏好回到 auto");

    // ── 垃圾偏好按"默认"读取（用户手改坏设置文件） ──
    {
        QSettings bad;
        bad.setValue(QStringLiteral("ui/theme"), QStringLiteral("purple"));
        bad.sync();
    }
    check(uitheme::preference() == uitheme::Mode::Auto,
          "★ 设置文件里是垃圾值时按「默认」处理");

    // ── 垃圾偏好下的菜单勾选也要一致（不能一项都不勾） ──
    {
        MainWindow *w2 = newWindow(QStringLiteral("move"));
        if (QMenu *tm2 = w2->findChild<QMenu *>(QStringLiteral("menuTheme"))) {
            check(!tm2->actions().isEmpty() && tm2->actions().first()->isChecked(),
                  "★ 垃圾偏好 → 菜单勾「默认」，与读取口径一致");
        } else {
            check(false, "菜单在新窗口里也在");
        }
        delete w2;
    }

    // 恢复现场
    {
        QSettings cleanup;
        cleanup.remove(QStringLiteral("ui/theme"));
        cleanup.sync();
    }
    uitheme::applyTheme(uitheme::Mode::Auto);
    QApplication::processEvents();

    delete w;
}


// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  历史面板跳转必须刷新画布（2026-10-16 修复的回归钉）
// ---------------------------------------------------------------------------
//
//  病灶（用户实测）：历史面板里双击某条记录回退到那个状态，数据确实回退了，
//  但画布不刷新 —— 直到做一次全图框选之类的大范围重绘，才看到其实已经回退。
//  根因：QUndoView 点条目走的是 QUndoStack::setIndex()，命令的 undo()/
//  redo() 照常执行，但这条路**不经过** CommandStack::push()/undo()/redo()，
//  于是没人通知 Document 失效合成缓存，画面停在旧内容上。
//  修复：CommandStack 监听 QUndoStack::indexChanged 兜底失效，内部路径用
//  m_internalChange 抑制重复失效（见 CommandStack.cpp 构造函数）。
//
//  本组用可观察行为钉住四件事：
//    ① 点击历史条目后 Document 必须收到 contentChanged（画布刷新的事实来源）
//    ② 点击后撤销/重做动作状态翻转（证明条目点击**真的跳转了**，
//       否则 ① 可能被别的事件误触发，变成假绿）
//    ③ 跳回后数据确实复原（合成图像素回到笔画之前的状态）
//    ④ 之后普通 undo/redo 每次仍然只刷新一次（抑制标记没把内部路径带坏）
static void testHistoryJumpRefreshesCanvas()
{
    beginGroup("历史面板跳转：回退后画布必须刷新");

    MainWindow *w = newWindow(QStringLiteral("brush"));
    CanvasView *cv = canvasOf(w);
    Document *doc = cv ? cv->document() : nullptr;
    check(doc != nullptr, "拿得到当前文档");
    if (!doc) {
        delete w;
        return;
    }

    auto *undoAct = w->findChild<QAction *>(QStringLiteral("actionUndo"));
    auto *redoAct = w->findChild<QAction *>(QStringLiteral("actionRedo"));
    check(undoAct != nullptr && redoAct != nullptr, "有撤销 / 重做动作");
    if (!undoAct || !redoAct) {
        delete w;
        return;
    }

    // ── 用画笔在画布上画一笔（压一条 DrawCommand）──
    //
    //  ★ 为什么用画笔而不是「粘贴为新图层」★
    //    LayerCommand 的 undo()/redo() 自己就会调 markDirty()
    //    （LayerCommand.cpp 里的既有行为）—— 拿它做跳转断言会**假绿**：
    //    就算没有修复，contentChanged 也会被命令自己发出来，断言恒真。
    //    DrawCommand 完全不自失效，刷新只可能来自 CommandStack，
    //    这样才能真的钉住这个 bug。
    const QRectF strokeRect(20, 20, 60, 20);   // 图像坐标：画布角上一小条
    const QPoint probePt(40, 30);              // 笔画必经之路上的探针点

    const QRgb beforeStroke = doc->composite().pixel(probePt);
    // dragSelection 只是往 CanvasView 送一串真实鼠标事件（按下→移动→
    // 松开），与工具无关；画笔激活时，同一串事件就是画一笔。
    dragSelection(cv, strokeRect);
    QApplication::processEvents();

    check(undoAct->isEnabled() && !redoAct->isEnabled(),
          "画一笔后：可撤销、不可重做（压上了一条命令）");
    check(doc->composite().pixel(probePt) != beforeStroke,
          "笔画真的落进了合成图（探针点像素变了）");

    // ── 在历史面板里点「初始状态」那一行 ──
    auto *historyDock = w->findChild<QDockWidget *>(QStringLiteral("historyDock"));
    auto *undoView = historyDock ? qobject_cast<QUndoView *>(historyDock->widget())
                                 : nullptr;
    check(undoView != nullptr, "历史面板里是 QUndoView");
    if (!undoView) {
        delete w;
        return;
    }

    int refreshes = 0;
    QObject::connect(doc, &Document::contentChanged,
                     [&refreshes]() { ++refreshes; });

    const QModelIndex initialRow = undoView->model()->index(0, 0);
    check(initialRow.isValid(), "历史面板有「初始状态」行");
    // ★ 用「真实鼠标双击」模拟用户点击历史条目 ★
    //   之前直接调 selectionModel()->setCurrentIndex() 绕过视图内部路径，
    //   在 offscreen 环境下会踩 Qt 视图未就绪的内部状态（崩在
    //   QListView::scrollTo → QModelIndex::parent，栈上 current 是垃圾）。
    //   真实用户就是双击条目 —— 测试就该送真实鼠标事件，和别的测试一致。
    const QRect rowRect = undoView->visualRect(initialRow);
    const QPoint clickPt = (rowRect.isValid() && !rowRect.isEmpty())
                               ? rowRect.center() : QPoint(8, 8);
    QWidget *vp = undoView->viewport();
    const QPoint globalPt = vp->mapToGlobal(clickPt);
    auto sendMouse = [vp, clickPt, globalPt](QEvent::Type t,
                                             Qt::MouseButtons btns) {
        QMouseEvent e(t, clickPt, globalPt, Qt::LeftButton, btns,
                      Qt::NoModifier);
        QApplication::sendEvent(vp, &e);
    };
    sendMouse(QEvent::MouseButtonPress, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
    sendMouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
    QApplication::processEvents();

    //  ★ 本组的三颗钉子 ★
    checkEqInt(refreshes, 1,
               "★ 点击历史条目后 Document 正好收到一次 contentChanged"
               "（修复前是 0 —— 数据回了但画面不动）");
    check(doc->composite().pixel(probePt) == beforeStroke,
          "★ 合成图回到笔画之前（缓存真的失效重算了，不是残留的旧画面）");
    check(!undoAct->isEnabled() && redoAct->isEnabled(),
          "★ 点击后撤销/重做状态翻转（证明真的跳回了初始状态）");

    // ── 跳完之后，普通 undo/redo 每次仍然只刷新一次 ──
    //  （抑制标记生效：兜底槽没有给内部路径再加一次全量失效。
    //    DrawCommand 不自失效，所以这里必须是干净的 1。）
    refreshes = 0;
    redoAct->trigger();
    QApplication::processEvents();
    checkEqInt(refreshes, 1, "★ 重做一次只刷新一次（抑制标记生效）");
    check(doc->composite().pixel(probePt) != beforeStroke, "重做后笔画回来");

    refreshes = 0;
    undoAct->trigger();
    QApplication::processEvents();
    checkEqInt(refreshes, 1, "★ 撤销一次只刷新一次");
    check(doc->composite().pixel(probePt) == beforeStroke, "撤销后笔画消失");

    delete w;
}

// ===========================================================================
//  [22] 拖文件到窗口打开（拖放到 MainWindow）
// ===========================================================================
//
//  "把文件拖到程序上就打开"落在 MainWindow::dragEnterEvent / dropEvent 上，
//  最终与命令行参数走同一条 openDocumentPaths()。
//
//  这一组钉两件事：
//    ① 拖进来**能被本程序解码**的本地文件 → 必须 accepted；
//       解不了的文件 → 不许 accepted（否则用户看到"能放"的光标，
//       松手却什么都没发生）。
//    ② 画布**不许**接受拖放。
//       ② 不是内部细节：Qt 找拖放目标是从鼠标下最内层 widget 向上找
//       **第一个** acceptDrops() 的 widget，找到就只发给它，即便它
//       ignore 也**不会**回退父控件（qwidgetwindow.cpp::findDnDTarget）。
//       画布铺满客户区，一旦它接受拖放，拖到画布上就静默无反应 ——
//       这正是历史上 CanvasView 里那句孤立 setAcceptDrops(true) 的后果。
static void testDropToOpenFiles()
{
    beginGroup("拖文件打开：窗口接受可解码的本地文件 / 画布不许抢");

    QTemporaryDir dir;
    check(dir.isValid(), "临时目录可用");
    if (!dir.isValid())
        return;

    // 真造一个图片文件（QImageReader::canRead 探测的是文件头，不是扩展名）
    const QString pngPath = dir.filePath(QStringLiteral("drop.png"));
    QImage img(64, 48, QImage::Format_ARGB32);
    img.fill(QColor(200, 40, 40));
    check(img.save(pngPath, "PNG"), "造出一个真的 PNG");

    // 再造一个"后缀像图片、内容不是"的文件 —— 钉住"看文件头而不是看后缀"
    const QString fakePath = dir.filePath(QStringLiteral("fake.png"));
    {
        QFile f(fakePath);
        if (f.open(QIODevice::WriteOnly))
            f.write("this is definitely not a png");
    }

    MainWindow *w = newWindow(QStringLiteral("move"));

    // ── ① 画布不许接受拖放 ──
    CanvasView *cv = canvasOf(w);
    check(cv != nullptr, "拿得到画布");
    if (cv) {
        check(!cv->acceptDrops(),
              "★ 画布不接受拖放（它一接受，拖到窗口上的文件就被吞掉，"
              "Qt 不会再把事件回退给 MainWindow）");
    }

    // ── ② 拖入可解码的本地图片 → 接受 ──
    {
        QMimeData mime;
        mime.setUrls(QList<QUrl>() << QUrl::fromLocalFile(pngPath));
        QDragEnterEvent ev(QPoint(200, 200), Qt::CopyAction, &mime,
                           Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(w, &ev);
        check(ev.isAccepted(), "★ 拖入本地 PNG → 窗口接受拖放");
    }

    // ── ③ 拖入解不了的文件 → 不接受 ──
    {
        QMimeData mime;
        mime.setUrls(QList<QUrl>() << QUrl::fromLocalFile(fakePath));
        QDragEnterEvent ev(QPoint(200, 200), Qt::CopyAction, &mime,
                           Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(w, &ev);
        check(!ev.isAccepted(),
              "★ 拖入内容不是图片的文件 → 不接受（判据是文件头，不是后缀）");
    }

    // ── ④ 拖到图层面板上：不许被静默吞掉 ──
    //
    //  列表控件为了"拖拽排序"必须是 acceptDrops(true)，而 Qt 只把拖放事件
    //  发给第一个 acceptDrops 的控件 —— 所以拖文件到面板上会被它吃掉。
    //  修法是它自己接住并转成 filesDropped。
    {
        LayerPanel *panel = w->findChild<LayerPanel *>();
        check(panel != nullptr, "拿得到图层面板");
        QListWidget *list = panel ? panel->listWidget() : nullptr;
        check(list != nullptr, "拿得到面板里的列表控件");
        check(list && list->acceptDrops(),
              "列表控件接受拖放（拖拽排序要用）—— 这正是它会吞文件的原因");

        if (list) {
            int dropped = 0;
            QStringList got;
            QObject::connect(panel, &LayerPanel::filesDropped, panel,
                             [&dropped, &got](const QStringList &paths) {
                                 ++dropped;
                                 got = paths;
                             });

            QMimeData mime;
            mime.setUrls(QList<QUrl>() << QUrl::fromLocalFile(pngPath));

            // 送真实事件给 viewport（QAbstractScrollArea 的拖放都在它身上）
            QWidget *target = list->viewport();
            QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime,
                                  Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &enter);
            check(enter.isAccepted(), "拖入文件时列表控件接受拖放");

            QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &drop);
            checkEqInt(dropped, 1, "拖放结束发出 filesDropped（没有被静默吞掉）");
            check(got.size() == 1 && got.first() == pngPath, "转出的是原始路径");
        }
    }

    // ── ⑤ macOS 的「用本程序打开文件」事件 ──
    //
    //  这条不是 macOS 专属逻辑：事件发给 QApplication，过滤器把它翻译成
    //  一次 openDocumentPaths。在 offscreen 下同样能发这个事件，所以能钉。
    {
        DocumentTabs *tabs = tabsOf(w);
        const int before = tabs ? tabs->count() : -1;
        check(before >= 1, "事件之前至少有一页");

        // 上面第 ④ 段已经把 pngPath 开过了，这里用另一个文件 ——
        // 否则测的是"同一文件不重复开"，而不是 FileOpen 处理本身。
        const QString secondPath = dir.filePath(QStringLiteral("fileopen.png"));
        QImage img2(20, 20, QImage::Format_ARGB32);
        img2.fill(QColor(30, 160, 90));
        check(img2.save(secondPath, "PNG"), "造出第二个 PNG");

        QFileOpenEvent ev(secondPath);
        QApplication::sendEvent(qApp, &ev);

        checkEqInt(tabs ? tabs->count() : -1, before + 1,
                   "收到 QEvent::FileOpen → 开成一页（macOS 拖到 Dock 图标走这条）");

        // 同一个文件再来一次：只切到已有那一页，不重复开
        QFileOpenEvent again(secondPath);
        QApplication::sendEvent(qApp, &again);
        checkEqInt(tabs ? tabs->count() : -1, before + 1,
                   "同一文件再次 FileOpen → 不重复开页");
    }

    delete w;
}

// ===========================================================================
//  [28] 导航器面板：缩略图布局 / 坐标换算 / 拖动定位
// ===========================================================================
//
// 三件事值得钉：
//   ① 缩略图等比居中（算错就是图被拉变形或贴在角上）
//   ② 图像坐标与缩略图坐标的双向换算（"点哪跳哪"的前提）
//   ③ 装配与绑定（面板关掉后再也打不开、切页后指向旧文档，都是静默失效）
static void testNavigatorPanel()
{
    beginGroup("导航器：缩略图布局 / 坐标换算 / 拖动定位");

    const QRectF panelRect(0, 0, 200, 200);

    // ── ① 等比 + 居中 ──
    {
        const QRectF box = navigator::thumbnailRect(QSizeF(800, 400), panelRect, 8);
        checkEqInt(qRound(box.width()), 184, "宽文档按宽度铺满（扣掉两侧 8px）");
        checkEqInt(qRound(box.height()), 92, "高度按原比例");
        check(qAbs(box.center().x() - panelRect.center().x()) < 0.51
                  && qAbs(box.center().y() - panelRect.center().y()) < 0.51,
              "缩略图在面板内居中");
    }
    {
        const QRectF box = navigator::thumbnailRect(QSizeF(400, 800), panelRect, 8);
        checkEqInt(qRound(box.height()), 184, "高文档按高度铺满");
        checkEqInt(qRound(box.width()), 92, "宽度按原比例");
    }
    check(navigator::thumbnailRect(QSizeF(), panelRect, 8).isEmpty(),
          "文档尺寸为空时返回空矩形（不除零）");

    // ── ② 坐标双向换算 ──
    {
        const QRectF thumb(10, 20, 400, 200);
        const QSizeF docSize(800, 400);

        const QPointF centerImg = navigator::thumbToImage(thumb.center(), thumb, docSize);
        checkEqInt(qRound(centerImg.x()), 400, "缩略图中心对应图像中心 x");
        checkEqInt(qRound(centerImg.y()), 200, "缩略图中心对应图像中心 y");

        const QRectF back = navigator::imageRectToThumb(
            QRectF(380, 190, 40, 20), thumb, docSize);
        checkEqInt(qRound(back.width()), 20, "图像 40 宽换算成缩略图 20（缩放 0.5）");
        check(qAbs(back.center().x() - thumb.center().x()) < 0.51,
              "同一中心点换算回去仍在中心");
    }
    {
        const QPointF clamped = navigator::thumbToImage(
            QPointF(-100, 900), QRectF(0, 0, 400, 200), QSizeF(800, 400));
        checkEqInt(qRound(clamped.x()), 0, "左侧越界夹到 0");
        checkEqInt(qRound(clamped.y()), 400, "下方越界夹到图像高度");
    }

    // ── ③ 装配与绑定 ──
    MainWindow *w = newWindow(QStringLiteral("move"));

    QDockWidget *navDock = w->findChild<QDockWidget *>(QStringLiteral("navigatorDock"));
    check(navDock != nullptr, "建出了导航器 dock");

    NavigatorPanel *navPanel = w->findChild<NavigatorPanel *>(
        QStringLiteral("navigatorPanel"));
    check(navPanel != nullptr, "dock 里装的是 NavigatorPanel");
    check(navPanel && navPanel->document() != nullptr, "绑上了当前文档");
    check(navPanel && navPanel->view() != nullptr, "绑上了当前画布");

    QMenu *panelMenu = w->findChild<QMenu *>(QStringLiteral("menuPanel"));
    check(panelMenu && navDock
              && panelMenu->actions().contains(navDock->toggleViewAction()),
          "「视图 → 面板」里能找到它（关掉之后还能再打开）");

    CanvasView *cv = canvasOf(w);
    Document *doc = navPanel ? navPanel->document() : nullptr;
    check(cv != nullptr && doc != nullptr, "拿得到画布与文档");

    // ── ④ 缩略图真的生成了 ──
    if (navPanel && doc) {
        // setDocument 传同一个指针会早退，先解除再设回来，强制重建一次。
        // 这一步同时钉住"重建用的是当前面板尺寸"，而不是构造时那个尺寸。
        navPanel->setDocument(nullptr);
        navPanel->setDocument(doc);

        const QRectF box = navPanel->thumbnailRect();
        check(!box.isEmpty(), "缩略图矩形非空");
        check(!navPanel->thumbnail().isNull(), "缩略图已生成（不是空图）");
        check(qAbs(navPanel->thumbnail().width() - box.width()) <= 2.0,
              "缩略图宽度与目标矩形一致");
    }

    // ── ⑤ 点缩略图某处 → 该点移到画布中心 ──
    if (navPanel && cv && doc) {
        // 先让布局跑完：画布没有实际尺寸时，居中的目标位置就是 (0,0)，
        // 断言只会给出误导性的失败。
        QApplication::processEvents();
        check(cv->width() > 0 && cv->height() > 0, "画布已完成布局（有实际尺寸）");

        // 先放大，让可视区只占图像一小块。否则点击点落在可视框内，
        // 面板会保留"抓取偏移"，中心不等于点击点，断言就失去意义。
        // 注意 setZoomLevel 收的是百分比（800 = 800%）。
        cv->setZoomLevel(800.0);
        QApplication::processEvents();

        const QRectF box = navPanel->thumbnailRect();
        const QSizeF docSize(doc->canvasSize());
        const QPointF clickPt(box.left() + box.width() * 0.15,
                              box.top() + box.height() * 0.85);
        const QPointF targetImg = navigator::thumbToImage(clickPt, box, docSize);

        QMouseEvent press(QEvent::MouseButtonPress, clickPt,
                          QPointF(navPanel->mapToGlobal(clickPt.toPoint())),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navPanel, &press);
        QApplication::processEvents();

        const QRectF visible = cv->visibleImageRect();
        check(qAbs(visible.center().x() - targetImg.x()) < 12.0
                  && qAbs(visible.center().y() - targetImg.y()) < 12.0,
              "在缩略图上点某处，该点被移到画布中心附近");
    }

    // ── ⑥ 解除绑定要清空 ──
    if (navPanel) {
        navPanel->setDocument(nullptr);
        check(navPanel->document() == nullptr && navPanel->thumbnail().isNull(),
              "解除绑定后缩略图清空");
    }

    delete w;
}

// ===========================================================================
//  [29] 自动保存 / 崩溃恢复（只存扁平合成图）
// ===========================================================================
//
// 钉四件事：
//   ① 快照能写、能覆盖、能删（写不出来等于没有这个功能）
//   ② 元数据在、图不在的条目要被跳过（否则用户点一个打不开的恢复项）
//   ③ 只有"还有未保存修改"的文档才写快照
//   ④ 存回干净状态后快照要消失（否则下次启动会拿旧内容问用户）
static void testRecovery()
{
    beginGroup("自动保存：快照写入 / 覆盖 / 清理 / 只存脏文档");

    QTemporaryDir dir;
    check(dir.isValid(), "临时目录可用");
    if (!dir.isValid())
        return;

    // ── ① RecoveryManager 本身 ──
    {
        RecoveryManager mgr;
        mgr.setDirectory(dir.filePath(QStringLiteral("rec")));

        QImage img(40, 30, QImage::Format_ARGB32);
        img.fill(QColor(10, 120, 200));

        check(mgr.save(QStringLiteral("k1"), img, QStringLiteral("/tmp/a.png"),
                       QStringLiteral("a.png")),
              "写入一条快照");
        check(QFileInfo::exists(dir.filePath(QStringLiteral("rec/k1.png"))),
              "快照图落盘");
        checkEqInt(mgr.pendingEntries().size(), 1, "枚举到一条");

        if (!mgr.pendingEntries().isEmpty()) {
            const RecoveryManager::Entry entry = mgr.pendingEntries().first();
            check(entry.title == QStringLiteral("a.png"), "元数据里有原标题");
            check(entry.imageSize == QSize(40, 30), "元数据里有尺寸");
            check(entry.savedAt.isValid(), "元数据里有保存时间");
        }

        check(mgr.save(QStringLiteral("k1"), img, QString(), QStringLiteral("b.png")),
              "同名再存一次（覆盖）");
        checkEqInt(mgr.pendingEntries().size(), 1, "覆盖不产生第二条");

        QImage img2(8, 8, QImage::Format_ARGB32);
        img2.fill(Qt::red);
        mgr.save(QStringLiteral("k2"), img2, QString(), QStringLiteral("c.png"));
        QFile::remove(dir.filePath(QStringLiteral("rec/k2.png")));
        checkEqInt(mgr.pendingEntries().size(), 1,
                   "图丢了的条目被跳过（不会让用户点一个不存在的恢复项）");

        mgr.discard(QStringLiteral("k1"));
        check(mgr.pendingEntries().isEmpty(), "discard 之后清空");
    }

    // ── ② 与主窗口联动 ──
    MainWindow *w = newWindow(QStringLiteral("move"));
    const QString liveDir = dir.filePath(QStringLiteral("live"));
    w->setRecoveryDirectory(liveDir);
    w->setAutoSaveIntervalMs(10);
    checkEqInt(w->autoSaveIntervalMs(), 10, "自动保存间隔可调");

    CanvasView *cv = canvasOf(w);
    Document *doc = cv ? cv->document() : nullptr;
    check(doc != nullptr, "拿得到当前文档");

    auto snapshotCount = [&liveDir]() {
        return QDir(liveDir).entryList(QStringList() << QStringLiteral("*.json"),
                                       QDir::Files).size();
    };

    if (doc) {
        checkEqInt(snapshotCount(), 0, "一开始没有快照");

        doc->setModified(true);

        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 3000 && snapshotCount() == 0)
            QApplication::processEvents();

        checkEqInt(snapshotCount(), 1, "脏文档在间隔到点后写出快照");

        const QStringList metas = QDir(liveDir).entryList(
            QStringList() << QStringLiteral("*.json"), QDir::Files);
        if (!metas.isEmpty()) {
            const QString key = QFileInfo(metas.first()).completeBaseName();
            const QImage restored(QDir(liveDir).filePath(key + QStringLiteral(".png")));
            check(!restored.isNull() && restored.size() == doc->canvasSize(),
                  "快照是一张与画布同尺寸的真图");
        }

        doc->setModified(false);
        QApplication::processEvents();
        checkEqInt(snapshotCount(), 0, "回到干净状态后快照被删除");
    }

    delete w;
}

// ============================================================================
//  [30] 全屏与撤销步数
// ============================================================================
static void testFullScreenAndUndoLimit()
{
    beginGroup("全屏切换 / 撤销步数：菜单装配 + 立即生效 + 越界夹紧");

    // ── ① 菜单装配 ──
    MainWindow *w = newWindow(QStringLiteral("move"));
    QApplication::processEvents();

    QAction *fullScreen = namedAction(w, "actionFullScreen");
    check(fullScreen != nullptr, "视图 → 全屏：按 objectName 找得到");
    if (fullScreen) {
        check(!fullScreen->icon().isNull(), "全屏动作有图标");
        check(fullScreen->shortcut() == QKeySequence(Qt::Key_F11), "快捷键是 F11");
        // 可勾选动作的勾必须靠窗口真实状态回写，而"从系统菜单退出全屏"
        // 不经过 QAction —— 索性不做成可勾选，免得留下一个骗人的勾。
        check(!fullScreen->isCheckable(), "全屏不是可勾选动作");
    }

    QAction *undoLimitAction = namedAction(w, "actionUndoLimit");
    check(undoLimitAction != nullptr, "编辑 → 撤销步数：按 objectName 找得到");
    if (undoLimitAction)
        check(!undoLimitAction->icon().isNull(), "撤销步数动作有图标");

    // ── ② 全屏真的进得去、也出得来 ──
    if (fullScreen) {
        check(!w->isFullScreen(), "起始不在全屏");
        fullScreen->trigger();
        QApplication::processEvents();
        check(w->isFullScreen(), "触发一次进全屏");
        fullScreen->trigger();
        QApplication::processEvents();
        check(!w->isFullScreen(), "再触发一次退出全屏");
    }

    // ── ③ 撤销步数：默认值 / 立即生效 / 越界夹紧 ──
    DocumentTabs *tabs = tabsOf(w);
    CommandStack *stack =
        (tabs && tabs->currentTab()) ? tabs->currentTab()->stack : nullptr;
    check(stack != nullptr, "拿得到当前页的撤销栈");

    checkEqInt(w->undoLimit(), 30, "默认 30 步");
    if (stack)
        checkEqInt(stack->undoLimit(), 30, "当前页的栈也是 30");

    w->setUndoLimit(75);
    checkEqInt(w->undoLimit(), 75, "主窗口记住新值");
    if (stack)
        checkEqInt(stack->undoLimit(), 75,
                   "已打开的页立刻生效（不是只影响以后新开的页）");

    w->setUndoLimit(0);
    checkEqInt(w->undoLimit(), 1, "0 被夹到下限 1");
    w->setUndoLimit(9999);
    checkEqInt(w->undoLimit(), 200, "超上限被夹到 200");

    delete w;

    // ── ④ 多页：每一页的栈都要跟着变 ──
    if (!g_settingsDir) {
        check(false, "临时目录不可用");
        return;
    }
    {
        const QString dir = g_settingsDir->path() + QStringLiteral("/undolimit");
        QDir().mkpath(dir);
        const QString p1 = dir + QStringLiteral("/a.png");
        const QString p2 = dir + QStringLiteral("/b.png");

        QImage a(QSize(16, 12), QImage::Format_ARGB32_Premultiplied);
        a.fill(QColor(200, 60, 60));
        QImage b(QSize(16, 12), QImage::Format_ARGB32_Premultiplied);
        b.fill(QColor(60, 60, 200));
        check(a.save(p1, "PNG") && b.save(p2, "PNG"), "写出两张临时 PNG");

        MainWindow mw(nullptr, QStringList{p1, p2});
        DocumentTabs *mt = tabsOf(&mw);
        checkEqInt(mt ? mt->count() : -1, 2, "两个文件开成两页");
        if (mt && mt->count() == 2) {
            mw.setUndoLimit(120);
            checkEqInt(mt->tabAt(0)->stack->undoLimit(), 120, "第一页跟着变");
            checkEqInt(mt->tabAt(1)->stack->undoLimit(), 120, "第二页也跟着变");
        }
    }
}

// ============================================================================
//  [31] 打印 / 导出 PDF
// ============================================================================
static void testPrintAndPdf()
{
    beginGroup("打印 / 导出 PDF：菜单装配 + 真能写出 PDF");

    MainWindow *w = newWindow(QStringLiteral("move"));
    QApplication::processEvents();

    // ── ① 菜单装配 ──
    QAction *print = namedAction(w, "actionPrint");
    check(print != nullptr, "文件 → 打印：按 objectName 找得到");
    if (print) {
        check(!print->icon().isNull(), "打印动作有图标");
        check(print->shortcut() == QKeySequence(QKeySequence::Print),
              "快捷键是 QKeySequence::Print（Ctrl+P）");
    }

    QAction *pdf = namedAction(w, "actionExportPdf");
    check(pdf != nullptr, "文件 → 导出 PDF：按 objectName 找得到");
    if (pdf)
        check(!pdf->icon().isNull(), "导出 PDF 动作有图标");

    QMenu *fileMenu = fileMenuOf(w);
    check(fileMenu != nullptr, "拿得到「文件」菜单");
    if (fileMenu) {
        const QList<QAction *> acts = fileMenu->actions();
        check(acts.contains(print), "打印项挂在「文件」菜单里");
        check(acts.contains(pdf), "导出 PDF 项挂在「文件」菜单里");
    }

    // ── ② 空指针防御 ──
    check(!w->renderToPrinter(nullptr), "renderToPrinter(nullptr) 返回 false（不崩）");

    // ── ③ 真写一份 PDF ──
    if (!g_settingsDir) {
        check(false, "临时目录不可用");
        delete w;
        return;
    }

    const QString path = g_settingsDir->path() + QStringLiteral("/export.pdf");
    QFile::remove(path);

    QString error;
    check(w->exportPdf(path, &error), "导出 PDF 成功");
    check(error.isEmpty(), "成功时不带错误文案");

    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray head = f.read(5);
        const qint64 size = f.size();
        f.close();
        check(head.startsWith("%PDF"), "文件头是 %PDF（不是一张改了后缀的位图）");
        check(size > 500, "文件确实有内容");
    } else {
        check(false, "导出后的 PDF 打得开");
    }

    // 同一路径再导一次：覆盖写不该失败（这也是"另存为同名文件"的常见路径）
    check(w->exportPdf(path), "同一路径再导出一次（覆盖）成功");

    QFile::remove(path);
    delete w;
}

// ============================================================================
//  [32] 图层蒙版
// ============================================================================
static void testLayerMask()
{
    beginGroup("图层蒙版：合成 / 撤销 / 合并也遵守");

    // ── ① Layer 自身：设蒙版 / 缩放 / 删除 / 深拷贝 ──
    {
        Layer layer(QSize(40, 30), QStringLiteral("L"));
        check(!layer.hasMask(), "新图层没有蒙版");

        QImage gray(40, 30, QImage::Format_Grayscale8);
        gray.fill(255);
        layer.setMask(gray);
        check(layer.hasMask(), "设了蒙版之后 hasMask 为真");
        checkEqInt(layer.mask().width(), 40, "蒙版宽度与图层一致");
        check(layer.mask().format() == QImage::Format_ARGB32_Premultiplied,
              "蒙版被统一成预乘 ARGB（合成侧不必每次现转）");

        // 尺寸不一致时必须缩放，否则合成时蒙版会错位
        QImage small(8, 6, QImage::Format_Grayscale8);
        small.fill(0);
        layer.setMask(small);
        checkEqInt(layer.mask().width(), 40, "尺寸不同的蒙版被缩放到图层尺寸");

        layer.setMask(QImage());
        check(!layer.hasMask(), "传空图等于删除蒙版");

        // clone 必须带上蒙版，而且是深拷贝 ——
        // 共享的话撤销会连带改掉当前蒙版（与像素那个坑同源）
        layer.setMask(gray);
        Layer copy = layer.clone();
        check(copy.hasMask(), "clone 带上蒙版");
        check(copy.mask().constBits() != layer.mask().constBits(),
              "clone 的蒙版是深拷贝");
    }

    // ── ② 合成：蒙版决定露出哪一半 ──
    {
        Document doc(QSize(40, 30), QColor(0, 0, 255));   // 底：纯蓝
        checkEqInt(doc.layerCount(), 1, "初始一个图层");

        Layer top(QSize(40, 30), QStringLiteral("Top"));
        QImage red(40, 30, QImage::Format_ARGB32_Premultiplied);
        red.fill(QColor(255, 0, 0));
        top.setImage(red);

        // 左半白（露出）、右半黑（遮住）
        //
        // ★ 必须传 qRgb，不能直接传 255 ★
        //   Grayscale8 的 setPixel() 会把参数当成 QRgb 再取亮度：
        //   qGray(0x000000FF) = 39，于是"全白"会变成"几乎全黑"，
        //   而症状只是合成结果不对 —— 看不出是构造数据时写错了。
        QImage mask(40, 30, QImage::Format_Grayscale8);
        for (int y = 0; y < 30; ++y) {
            for (int x = 0; x < 40; ++x)
                mask.setPixel(x, y, x < 20 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
        }
        top.setMask(mask);
        doc.addLayer(top);

        // 诊断用：先确认蒙版图本身是"左白右黑"
        checkEqInt(qAlpha(top.mask().pixel(5, 15)), 255, "蒙版左半 alpha=255");
        checkEqInt(qAlpha(top.mask().pixel(35, 15)), 0, "蒙版右半 alpha=0");

        const QImage composed = doc.composite();
        check(composed.pixelColor(5, 15).red() > 200,
              "蒙版白色的那一半：看到上层（红）");
        check(composed.pixelColor(35, 15).blue() > 200,
              "蒙版黑色的那一半：透出下层（蓝）");
    }

    // ── ③ 合并可见 / 拼合也要遵守 ──
    //
    //  这是最容易漏的一处：漏了不会报错，只在合并后的图上才看得出来 ——
    //  即"屏幕上看着有蒙版，合并出来的却没有"。
    {
        const auto buildDoc = [](Document &doc) {
            Layer top(QSize(40, 30), QStringLiteral("Top"));
            QImage red(40, 30, QImage::Format_ARGB32_Premultiplied);
            red.fill(QColor(255, 0, 0));
            top.setImage(red);

            QImage mask(40, 30, QImage::Format_Grayscale8);
            for (int y = 0; y < 30; ++y) {
                for (int x = 0; x < 40; ++x)
                    mask.setPixel(x, y, x < 20 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
            }
            top.setMask(mask);
            doc.addLayer(top);
        };

        {
            Document doc(QSize(40, 30), QColor(0, 0, 255));
            buildDoc(doc);
            check(doc.mergeVisibleLayers(), "合并可见图层成功");
            const QImage merged = doc.composite();
            check(merged.pixelColor(5, 15).red() > 200, "合并可见：左半仍是上层");
            check(merged.pixelColor(35, 15).blue() > 200, "合并可见：右半仍透出下层");
        }
        {
            Document doc(QSize(40, 30), QColor(0, 0, 255));
            buildDoc(doc);
            check(doc.flattenImage(), "拼合图像成功");
            const QImage flat = doc.composite();
            check(flat.pixelColor(5, 15).red() > 200, "拼合：左半仍是上层");
            check(flat.pixelColor(35, 15).blue() > 200, "拼合：右半仍透出下层");
        }
    }

    // ── ④ 菜单 + 命令 + 撤销（走真实路径）──
    {
        MainWindow *w = newWindow(QStringLiteral("move"));
        CanvasView *cv = currentCanvasOf(w);
        Document *doc = cv ? cv->document() : nullptr;
        check(doc != nullptr, "拿得到当前文档");

        QAction *add = namedAction(w, "actionLayerMaskAdd");
        QAction *remove = namedAction(w, "actionLayerMaskRemove");
        check(add != nullptr, "图层 → 从选区添加蒙版：按 objectName 找得到");
        check(remove != nullptr, "图层 → 删除蒙版：按 objectName 找得到");

        if (doc && add && remove) {
            QMenu *layerMenu = w->findChild<QMenu *>(QStringLiteral("menuLayer"));
            check(layerMenu != nullptr, "拿得到「图层」菜单");
            if (layerMenu) {
                const QList<QAction *> acts = layerMenu->actions();
                check(acts.contains(add) && acts.contains(remove),
                      "两个蒙版菜单项挂在「图层」菜单里");
            }

            Layer *layer = doc->activeLayer();
            check(layer && !layer->hasMask(), "起始没有蒙版");

            // 新建文档的初始图层是**透明**的（Qt::transparent），
            // 透明层上量 alpha 量不出蒙版效果，所以先填成不透明。
            if (layer) {
                QImage filled(doc->canvasSize(),
                              QImage::Format_ARGB32_Premultiplied);
                filled.fill(QColor(255, 0, 0));
                layer->setImage(filled);
                doc->markDirty();
            }

            // 选区蒙版必须先按画布尺寸建好，否则 selectRect 会被静默吞掉
            // （Selection::selectRect 的第一行就是"没有蒙版就返回"）。
            doc->selection().prepareCanvas(doc->canvasSize());
            doc->selection().selectRect(QRect(0, 0, 20, 20));
            check(doc->selection().isActive(), "选区已建立");
            check(!doc->selection().mask().isNull(), "选区蒙版非空");
            check(add->isEnabled(), "「添加蒙版」菜单项是启用的");

            add->trigger();
            check(layer && layer->hasMask(), "触发菜单后有了蒙版");

            const QImage shown = doc->composite();
            check(shown.pixelColor(5, 5).alpha() > 200,
                  "选区内仍然可见");
            check(shown.pixelColor(600, 400).alpha() == 0,
                  "选区外被蒙版遮住（变成透明）");

            CommandStack *stack = tabsOf(w) ? tabsOf(w)->currentTab()->stack : nullptr;
            check(stack != nullptr, "拿得到撤销栈");
            if (stack) {
                stack->undo();
                check(layer && !layer->hasMask(), "撤销后蒙版消失");
                stack->redo();
                check(layer && layer->hasMask(), "重做后蒙版回来");
            }

            remove->trigger();
            check(layer && !layer->hasMask(), "删除蒙版");
            if (stack) {
                stack->undo();
                check(layer && layer->hasMask(), "撤销删除后蒙版回来");
            }
        }

        delete w;
    }
}

int main(int argc, char **argv)
{
    // ★ 必须在 QApplication 构造前设置 ★
    //   offscreen 插件让 Qt 在没有显示器的环境下也能建窗口、发事件。
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 主窗口装配 离线自检\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    redirectSettingsToTempDir();

    testToolBoxMatchesTools();
    testCanvasToolInvariant();
    testRestoreLastActiveTool();
    testToolInvariantSurvivesNewDocument();
    testCropMenuEnabledState();
    testCropToSelection();
    testIcons();
    testIconsFollowTheme();
    testAppIdentityAndIcon();
    testUiLanguage();
    testLanguageMenuAssembly();
    testThemeMenu();
    testLayerPanelAssembly();
    testMergeVisibleKeepsHiddenLayers();
    testRenameActiveLayerAction();
    testOpenAsLayerAssembly();
    testPasteAsNewLayerRegression();
    testImportImageAsLayerPolicies();
    testDocumentTabs();
    testRulersAndGuides();
    testGridSpacingAndSnap();
    testIcoExport();
    testCommandLineFiles();
    testScreenCaptureMenu();
    testGlobalHotkey();
    testScreenCaptureFlow();
    testCreateImagePage();
    testHistoryJumpRefreshesCanvas();
    testDropToOpenFiles();
    testNavigatorPanel();
    testRecovery();
    testFullScreenAndUndoLimit();
    testPrintAndPdf();
    testLayerMask();

    std::printf("------------------------------------------------\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
