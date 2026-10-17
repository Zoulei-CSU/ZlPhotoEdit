#include "ui/MainWindow.h"

#include "compat/QtCompat.h"
#include "compat/GlobalHotkey.h"

// ★ 程序显示名（"逐鹿 图片编辑器"）与窗口图标的唯一来源 ★
//   标题相关的一切都走它，别再往代码里写程序名字面量。
#include "ui/AppInfo.h"

#include "core/DocumentManager.h"
#include "core/RecoveryManager.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "core/ClipboardData.h"

#include "commands/CommandStack.h"
#include "commands/LayerCommand.h"
#include "commands/SelectionCommand.h"
#include "commands/ImageCommand.h"
#include "commands/GeometryChangeCommand.h"

#include "engine/Filters.h"
#include "engine/Geometry.h"
#include "engine/ImageMeta.h"
#include "engine/Transform.h"

#include "tools/ToolManager.h"
#include "tools/AbstractTool.h"
#include "tools/ToolOptions.h"
#include "tools/MoveTool.h"
#include "tools/BrushTool.h"
#include "tools/SelectionTool.h"
#include "tools/CropTool.h"
#include "tools/BucketTool.h"
#include "tools/ColorPickerTool.h"
#include "tools/MagicWandTool.h"
#include "tools/ShapeTool.h"
#include "tools/TextTool.h"
#include "tools/HandTool.h"
#include "tools/ZoomTool.h"

#include "ui/CanvasView.h"
#include "ui/StatusBarWidget.h"
#include "ui/ToolOptionsBar.h"
// ★ 多标签容器：中央控件就是它（每开一页，容器内部建一块自己的画布）★
#include "ui/DocumentTabs.h"
#include "ui/RulerWidget.h"
// ★ 必须完整定义，不能只靠 MainWindow.h 里的前置声明 ★
//   applyDocumentToPanels() 要调 m_layerPanel->setDocument(doc) ——
//   编译器需要知道 LayerPanel 有这个方法，前置声明不够。
//   报错是 C2027「使用了未定义类型」。
#include "ui/LayerPanel.h"
#include "ui/NavigatorPanel.h"

#include <QApplication>
#include <QFileOpenEvent>
#include <QAction>
#include <QActionGroup>
// 屏幕截图的全局热键要从 kCaptureHotkey（字符串）构造它 —— 只此一处构造，
// 菜单上的说明文字也从同一个 QKeySequence 取（见 setupScreenCapture）。
#include <QKeySequence>
#include <QGridLayout>
#include <QPalette>
#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <QDockWidget>
#include <QStatusBar>
#include <QLabel>
#include <QComboBox>
#include <QUndoView>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QClipboard>
#include <QSettings>
#include <QToolButton>
#include <QPainter>
#include <QTimer>
#include <QPixmap>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QFrame>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QScreen>
#include <QDesktopServices>
#include <QUrl>
#include <QScopedValueRollback>
#include <QDebug>

namespace zl {

// ============================================================================
//  MainWindow —— 构造
// ============================================================================
//
//  ★ 本文件只放"组装"代码 ★
//
//    职责划分（三个同名 .cpp 文件各管一块）：
//      MainWindow.cpp          —— 构造/析构、各 setupXxx、文档切换、标题
//      MainWindowActions.cpp   —— QAction 定义、菜单、工具栏、停靠面板、设置持久化
//      MainWindowCommands.cpp  —— 全部菜单命令的实现（文件/编辑/调整/图层/视图）
//
//    按"职责"而不是按"类"拆分文件，是因为这个类天然承担了三种不同的工作，
//    全塞进一个文件会变成 3000 行的巨物 —— 而它又确实不该被拆成三个类
//    （那样会引入大量跨类访问与转发函数，得不偿失）。
//
//  ★ 注意本文件里没有任何匿名命名空间的辅助函数 ★
//    曾经这里的 kDefaultNewWidth / makeColorSwatch / colorText 与
//    MainWindowCommands.cpp 里的同名定义重复了。由于两处都在匿名命名空间里，
//    编译器不会报"重复定义"（内部链接），但会产生两套各自独立的值 ——
//    将来改默认画布尺寸时只改了一处，就会出现"新建用 1200×800，
//    状态栏提示说 1024×768"这种诡异的症状。
//    所以这里已经全部删除，只保留 MainWindowCommands.cpp 里那一份。
// ============================================================================
// ============================================================================
//  MainWindow —— 构造
// ============================================================================

MainWindow::MainWindow(QWidget *parent, const QStringList &initialFiles)
    : QMainWindow(parent)
{
    setWindowTitle(zl::appinfo::titleSuffix());

    // ★ 窗口图标也在这里设一次（除了 main.cpp 里的 QApplication 那一层）★
    //
    //  为什么两处都设：
    //    · QApplication::setWindowIcon 是"进程级默认值"，要有人调它 ——
    //      调它是 main.cpp 的职责。但如果 MainWindow 被**别的宿主**创建
    //      （自检的截图工具就是一个），那条路没人走，窗口就没图标。
    //    · 在窗口上再设一次，语义是"这个窗口显示这个图"，与宿主无关。
    //      代价是零（QIcon 是隐式共享的），而且**立刻**能在标题栏上看到
    //      结果 —— 而进程级默认值要等窗口显示时才继承。
    //
    //  ★ 空图标不设、也不报错 ★
    //    setWindowIcon(空) 是合法的，效果是"没有图标"。
    //    真失败时的响亮提示在 main.cpp（那里有 qWarning）。
    const QIcon appIcon = zl::appinfo::windowIcon();
    if (!appIcon.isNull())
        setWindowIcon(appIcon);

    setMinimumSize(900, 600);
    setAcceptDrops(true);

    // 中央控件会由 setupCanvas() 设置
    // 停靠面板的允许区域：四个方向都行
    setDockOptions(QMainWindow::AnimatedDocks
                   | QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks);

    // ── 初始化顺序很重要 ──
    //
    //  1. DocumentManager —— 其他一切都需要它
    //  2. Canvas / 标签容器—— 中央控件
    //  3. Tools           —— 需要 CommandStack；但先不绑定文档
    //  4. Actions / Menus / ToolBars
    //  5. Docks
    //  6. StatusBar
    //  7. 屏幕截图的全局热键与定时器（要状态栏，所以排在 6 之后）
    //  8. Connections
    //
    //  这个顺序不是随意的：例如第 3 步必须在第 2 步之后，
    //  因为 ToolManager 需要把工具推给 CanvasView。
    //  第 7 步必须在第 6 步之后 —— 全局热键登记失败时要在状态栏说明原因，
    //  而"被别的程序占用"是最常见的失败形态（症状恰好是"按了没反应"，
    //  与程序坏了无法区分）。见 setupScreenCapture() 里的说明。
    //
    //  ★ 撤销栈不在这里建 ★
    //    它属于**标签页**（一页一个）。建栈的时机是"开一页"，
    //    也就是 newDocument() / openDocumentFromPath() 里 —— 见
    //    attachDocumentTab()。所以构造函数里没有任何 CommandStack。
    setupDocumentManager();
    setupCanvas();
    setupActions();
    setupMenus();
    setupToolBars();
    setupTools();
    setupDocks();
    setupStatusBar();
    setupScreenCapture();
    setupConnections();

    loadSettings();

    // macOS 的"用本程序打开文件"是发给 QApplication 的事件，窗口自己收不到。
    // 过滤器跟着本对象走：QObject 析构时会自动从 qApp 上摘掉。
    qApp->installEventFilter(this);

    // ── 启动时的文档 ──
    //
    //  有命令行参数 → 每个文件开一页（`ZlPhotoEdit a.png b.png`）；
    //  没有参数   → 一个空白画布。
    //
    //  ★ 为什么不"两个都做" ★
    //    `ZlPhotoEdit a.png` 的用户想要的是"打开 a.png"，
    //    再白送一个空白标签只会让他多一次 Ctrl+W。
    //
    //  ★ 为什么不留空 ★
    //    留空的话用户看到的是一句"没有打开的文档"，
    //    必须点一次"新建"才能开始工作。多一次点击而没有任何收益。
    //    新建的文档在 CommandStack::clear() 之后处于 clean 状态，
    //    标题不带 *，关闭时也不会提示保存。
    //
    //  ★ 参数全部无效时回落到空白文档 ★
    //    否则程序会启动成"一个标签都没有、菜单全灰"的样子 ——
    //    用户会以为程序坏了，而不是"我给的路径不对"。
    if (initialFiles.isEmpty() || openDocumentPaths(initialFiles) == 0)
        newDocument();

    // ★ "恢复上次使用的工具"不在这里做 ★
    //
    //  工具参数与"上次活动的工具"由 ToolManager::loadAllOptions() 一并
    //  处理（它在 setupTools() 里已经被调用）。在这里再读一次
    //  tools/lastActive 是重复劳动，而且会造成两个问题：
    //    · 两次 setActiveTool 触发两轮 activeToolChanged 信号，
    //      选项条被重建两次（控件短暂闪烁）
    //    · 两处代码分别读同一个配置键，将来改键名时要记得改两处
}

MainWindow::~MainWindow()
{
    // 保存设置要在成员销毁之前 —— 特别是工具参数，
    // 它们由 ToolManager 持有，析构后就取不到了。
    saveSettings();

    // ★ 撤销栈的消费者必须在这里松手 ★
    //
    //  本窗口的子对象按**创建顺序**析构，而 DocumentTabs 是在
    //  setupCanvas() 里创建的、排在历史面板（setupDocks）之前 ——
    //  也就是说各页的撤销栈会先消失，历史面板后消失。
    //
    //  析构路径不经过 aboutToRemoveTab（对象销毁期不该发业务信号），
    //  所以这里显式解一次。传 nullptr = "全部解开"。
    detachStackConsumers(nullptr);
}

// ============================================================================
//  setupDocumentManager
// ============================================================================

void MainWindow::setupDocumentManager()
{
    m_docManager = new DocumentManager(this);

    // ★ 本类**不再监听** DocumentManager::currentChanged ★
    //
    //  单文档时代它是"当前文档变了"的唯一信号，所有面板靠它重绑。
    //  引入标签页之后，"当前是哪个文档"由标签页说得更准 ——
    //  它同时知道这一页该配哪份撤销栈、哪块画布，而 DocumentManager
    //  只知道文档列表。
    //
    //  于是同步方向反过来：**标签页是真相**，activateTab() 把结果推给
    //  DocumentManager（m_docManager->setCurrentIndex）。面板的重绑仍然
    //  只有一个入口 —— 还是 applyDocumentToPanels()，只是调用它的人
    //  从"信号"换成了 activateTab()。
    //
    //  ★ 为什么要单向 ★
    //    双向同步要两个方向的"防重入"标志，而任何一处遗漏都会变成
    //    "切页递归"或者"当前页卡住不动"。单向只有一个方向可能出错。
    //
    //  ★ 撤销栈也不在这里建了 ★
    //    它属于页，不属性"当前文档"。旧代码在这里 new CommandStack
    //    并 deleteLater 旧的 —— 那正是"切页丢失撤销历史"的根源。
    //    现在栈随页创建（DocumentTabs::addDocumentTab），切页只是换指针。
    connect(m_docManager, &DocumentManager::documentRemoved,
            this, [this](Document *) {
        updateWindowTitle();
    });

    setupRecovery();
}

// ============================================================================
//  自动保存 / 崩溃恢复
// ============================================================================

void MainWindow::setupRecovery()
{
    m_recoveryManager = new RecoveryManager(this);

    m_recoveryTimer = new QTimer(this);
    m_recoveryTimer->setSingleShot(true);
    m_recoveryTimer->setInterval(15000);
    connect(m_recoveryTimer, &QTimer::timeout,
            this, &MainWindow::writeRecoverySnapshots);

    if (m_docManager) {
        connect(m_docManager, &DocumentManager::documentModifiedChanged,
                this, [this](Document *doc, bool modified) {
            if (modified)
                scheduleRecovery();
            else
                discardRecoveryFor(doc);
        });
        connect(m_docManager, &DocumentManager::documentRemoved,
                this, [this](Document *doc) { discardRecoveryFor(doc); });
    }
}

void MainWindow::setAutoSaveIntervalMs(int ms)
{
    if (m_recoveryTimer)
        m_recoveryTimer->setInterval(qMax(0, ms));
}

int MainWindow::autoSaveIntervalMs() const
{
    return m_recoveryTimer ? m_recoveryTimer->interval() : -1;
}

void MainWindow::setRecoveryDirectory(const QString &dir)
{
    if (m_recoveryManager)
        m_recoveryManager->setDirectory(dir);
}

QString MainWindow::recoveryKeyFor(Document *doc)
{
    if (!doc)
        return QString();

    const auto it = m_recoveryKeys.constFind(doc);
    if (it != m_recoveryKeys.constEnd())
        return it.value();

    const QString key = RecoveryManager::makeKey(++m_recoverySeq);
    m_recoveryKeys.insert(doc, key);
    return key;
}

void MainWindow::scheduleRecovery()
{
    // 已在计时就不重置：连续落笔时重置会让快照永远写不出去。
    if (m_recoveryTimer && !m_recoveryTimer->isActive())
        m_recoveryTimer->start();
}

void MainWindow::writeRecoverySnapshots()
{
    if (!m_recoveryManager || !m_docManager)
        return;

    bool stillDirty = false;

    for (int i = 0; i < m_docManager->count(); ++i) {
        Document *doc = m_docManager->documentAt(i);
        if (!doc || !doc->isModified())
            continue;

        stillDirty = true;

        const QString key = recoveryKeyFor(doc);
        const QImage snapshot = doc->composite();
        if (snapshot.isNull())
            continue;

        m_recoveryManager->save(key, snapshot, doc->filePath(), doc->displayName());
    }

    if (stillDirty)
        m_recoveryTimer->start();
}

void MainWindow::discardRecoveryFor(Document *doc)
{
    if (!doc || !m_recoveryManager)
        return;

    const QString key = m_recoveryKeys.take(doc);
    if (!key.isEmpty())
        m_recoveryManager->discard(key);
}

void MainWindow::promptRecoveryOnStartup()
{
    if (!m_recoveryManager)
        return;

    const QList<RecoveryManager::Entry> entries = m_recoveryManager->pendingEntries();
    if (entries.isEmpty())
        return;

    QStringList names;
    for (const RecoveryManager::Entry &entry : entries)
        names << (entry.title.isEmpty() ? tr("(Untitled)") : entry.title);

    const int count = entries.size();
    const auto answer = QMessageBox::question(
        this, tr("Recover Unsaved Work"),
        tr("Found %1 unsaved document(s) from the previous session:\n\n%2\n\n"
           "Recover them? Layer information is not preserved.")
            .arg(count)
            .arg(names.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (answer != QMessageBox::Yes) {
        m_recoveryManager->clearAll();
        return;
    }

    int restored = 0;
    for (const RecoveryManager::Entry &entry : entries) {
        const QImage image(entry.imagePath);
        if (!image.isNull()) {
            ImagePageSpec spec;
            spec.image = image;
            spec.titleHint = entry.title.isEmpty()
                                 ? tr("Recovered")
                                 : tr("%1 (Recovered)").arg(entry.title);
            if (createImagePage(spec))
                ++restored;
        }
        m_recoveryManager->discard(entry.key);
    }

    if (m_statusBar)
        m_statusBar->showMessage(tr("Recovered %1 document(s)").arg(restored), 6000);
}

// ============================================================================
//  setupCanvas —— 中央控件（多标签容器）
// ============================================================================
//
//  ★ 阶段八：中央控件从"一个 CanvasView"变成"一个 QTabWidget"★
//
//    这是本阶段唯一一处**结构性**改动。之所以没有牵扯到别的地方，
//    是因为每块画布都只属于自己那一页 —— 外面的代码看到的永远是
//    "当前页的画布"（m_canvas），它由 activateTab() 维护。
//
//  ★ 画布的三个信号是"每块画布各连一次"的 ★
//    它们原来连在唯一那块画布上。现在每开一页都会新建一块画布，
//    所以连接必须跟着画布走 —— 否则第二页起的光标坐标、缩放提示、
//    状态消息都不会到达状态栏（而这些都**不报错**，只是"没反应"）。
//    见 CanvasView 的创建点：DocumentTabs::addDocumentTab()。

void MainWindow::setupCanvas()
{
    // ── 中央控件 = 拐角 + 上标尺 + 左标尺 + 标签容器 ──
    //
    // ★ 为什么需要一个网格宿主，而不是把标尺浮在画布上 ★
    //   标尺必须在**画布外面**：浮在画布上会盖住图像边缘若干像素，
    //   而且缩放时它自己也得跟着重算位置 —— 纯粹的负担。
    //   放进 QGridLayout，则"标尺的长度 == 画布那一维的长度"由 Qt 保证。
    //
    // ★ 为什么拐角是独立控件 ★
    //   它只是补上两把尺子交会处那个小方块；不补的话那里会露出宿主背景，
    //   两把尺子看起来像断开的。
    m_canvasHost = new QWidget(this);
    auto *hostLayout = new QGridLayout(m_canvasHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);

    m_rulerCorner = new QWidget(m_canvasHost);
    m_rulerCorner->setFixedSize(RulerWidget::kThickness, RulerWidget::kThickness);
    m_rulerCorner->setAutoFillBackground(true);
    {
        QPalette pal = m_rulerCorner->palette();
        pal.setColor(QPalette::Window, QColor(238, 238, 238));
        m_rulerCorner->setPalette(pal);
    }

    m_hruler = new RulerWidget(Qt::Horizontal, m_canvasHost);
    m_vruler = new RulerWidget(Qt::Vertical, m_canvasHost);

    m_tabs = new DocumentTabs(m_canvasHost);

    hostLayout->addWidget(m_rulerCorner, 0, 0);
    hostLayout->addWidget(m_hruler,      0, 1);
    hostLayout->addWidget(m_vruler,      1, 0);
    hostLayout->addWidget(m_tabs,        1, 1);
    hostLayout->setColumnStretch(1, 1);
    hostLayout->setRowStretch(1, 1);

    setCentralWidget(m_canvasHost);

    // ── 从标尺拖出参考线 ──
    //
    //  两根尺子共用同一段处理：把"还没落下的那根线"交给**当前页**的画布，
    //  松手时按 commit 决定落下还是丢弃。
    //
    //  ★ 用 m_canvas 而不是捕获某一块具体画布 ★
    //    尺子是跨页共享的（只有两根），而它发信号的那一刻，m_canvas 已经被
    //    activateTab() 换成正确的那一页了 —— 捕获具体画布就会出现
    //    "切了页之后从尺子拖出的线落在上一页"。
    const QList<RulerWidget *> rulers = { m_hruler, m_vruler };
    for (RulerWidget *ruler : rulers) {
        connect(ruler, &RulerWidget::guideDragMoved, this,
                [this](Qt::Orientation orientation, int posImage) {
            if (m_canvas)
                m_canvas->setPendingGuide(orientation, posImage);
        });

        connect(ruler, &RulerWidget::guideDragFinished, this,
                [this](Qt::Orientation, bool commit) {
            if (!m_canvas)
                return;
            if (commit && m_canvas->hasPendingGuide()) {
                const Guide g = m_canvas->pendingGuide();
                m_canvas->clearPendingGuide();
                m_canvas->addGuide(g.orientation, g.position);
            } else {
                m_canvas->clearPendingGuide();
            }
        });
    }

    // ── 用户点了某一页（切页）──
    //
    //  ★ 这里只做一件事：重绑"当前页"的那一套 ★
    //    真相是标签页，所以切页不需要先通知 DocumentManager 再绕回来。
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (m_switchingTabs)
            return;               // 增页/摘页的中间态，见 attachDocumentTab()
        activateTab(index);
    });

    // ── 用户点了某一页的 × ──
    //
    //  ★ 容器只报告"点了第几页"，关不关由这里决定 ★
    //    与图层面板同一条纪律：控件不碰业务。所以"点 × 关闭"与
    //    "菜单 → 关闭"走的是**同一条** closeDocumentTab()，
    //    不会出现两条路径行为不一致（例如一条记得问保存、另一条忘了）。
    connect(m_tabs, &DocumentTabs::closeTabRequested,
            this, &MainWindow::closeDocumentTab);

    // ── 某一页的撤销栈即将被销毁 ──
    //
    //  ★ 这条连接是"防段错误"的，不是业务逻辑 ★
    //    历史面板（QUndoView）与状态栏都持有撤销栈的**裸指针**，
    //    而销毁栈的是 DocumentTabs —— 两边必须对齐时机。
    //    不接这一条，关掉一个标签页就会踩到已释放的栈
    //    （实测：0 错误 0 警告，直接段错误）。
    //    详见 MainWindow::detachStackConsumers() 与 DocumentTabs.h 的
    //    aboutToRemoveTab 说明。
    connect(m_tabs, &DocumentTabs::aboutToRemoveTab, this, [this](int index) {
        DocumentTab *tab = m_tabs ? m_tabs->tabAt(index) : nullptr;
        detachStackConsumers(tab ? tab->stack : nullptr);
    });
}

// ============================================================================
//  connectCanvas —— 每块画布都要接的那几条线
// ============================================================================
//
//  ★ 抽成函数的理由：它必须对**每一块**画布调用一次 ★
//    画布是"一页一块"的（DocumentTabs::addDocumentTab 里 new 出来），
//    所以连接不能像单文档时代那样在 setupCanvas() 里连一次就完事。
//    漏连的症状全部是**静默**的：状态栏不再显示光标坐标、不再显示
//    像素颜色、缩放比不再跟着走 —— 界面看起来没坏，只是"没反应"。

void MainWindow::connectCanvas(CanvasView *canvas)
{
    if (!canvas)
        return;

    connect(canvas, &CanvasView::cursorMoved, this, [this](const QPointF &pos) {
        if (m_statusBar)
            m_statusBar->setCursorPos(pos);
    });

    connect(canvas, &CanvasView::cursorColorSampled,
            this, [this](const QColor &color, const QPointF &pos) {
        if (m_statusBar)
            m_statusBar->setSampledColor(color, pos);
    });

    // ★ 只有"当前页的"画布才允许改状态栏与窗口标题 ★
    //   每块画布都会发 viewChanged（初始化时的 zoomToFit 就发一次）。
    //   不过滤的话，后台那几页在 resize 时会轮流把状态栏的缩放比
    //   改成自己的数值 —— 用户会看到数字莫名其妙地跳。
    connect(canvas, &CanvasView::viewChanged, this, [this, canvas]() {
        if (canvas != m_canvas)
            return;
        if (m_statusBar)
            m_statusBar->setZoom(canvas->zoom());
        updateWindowTitle();
    });

    connect(canvas, &CanvasView::statusMessage,
            this, [this](const QString &msg, int timeout) {
        if (m_statusBar)
            m_statusBar->showMessage(msg, timeout);
    });
}

// ============================================================================
//  setupTools
// ============================================================================

void MainWindow::setupTools()
{
    // ToolManager 需要有撤销栈与文档才能工作。
    // 此时 m_commandStack 已经在 newDocument() 之前建立了吗？
    // 不一定 —— setupTools 在 newDocument() 之前调用，
    // 所以这里传 nullptr，之后由 applyDocumentToPanels 补上。
    m_toolManager = new ToolManager(m_commandStack, this);

    // ── 注册工具 ──
    //
    // ★ 注册顺序 = 工具箱里从上到下的顺序 ★
    //   把最常用的放前面（移动、画笔），少用的放后面（文字、缩放）。
    //   这是 Photoshop 等软件的共同排列，用户有肌肉记忆。
    //
    // ★ Phase 1 只注册已实现的工具 ★
    //   未实现的工具注册进来会让用户点到一个"什么都不做"的按钮，
    //   体验上比"按钮不存在"更差。所以逐个加上，不预留禁用项。

    m_toolManager->addTool(new MoveTool(nullptr, m_commandStack, m_toolManager));

    // ── Phase 2：画笔与橡皮 ──
    //
    // ★ 为什么画笔排在移动之后、别的工具之前 ★
    //   移动与画笔是使用频率最高的两个工具，放在工具箱最上面。
    //   工具箱的按钮顺序就是注册顺序（buildToolBox 遍历 tools()），
    //   所以这里的顺序直接决定用户看到什么。
    //
    // ★ 画笔与橡皮是同一个类的两个实例 ★
    //   交互逻辑完全一样（按住拖动、沿轨迹落笔、整笔一条撤销），
    //   区别只在最后一步合成：一个盖颜色，一个擦 alpha。
    //   拆成两个类会让"补点/蒙版累积/撤销打包"这三块逻辑复制两遍。
    //   BrushTool 内部用 Kind 区分，参数对象也共用同一个设置分组。
    m_toolManager->addTool(new BrushTool(BrushTool::Kind::Brush, nullptr,
                                        m_commandStack, m_toolManager));
    m_toolManager->addTool(new BrushTool(BrushTool::Kind::Eraser, nullptr,
                                        m_commandStack, m_toolManager));

    // ── Phase 6a：吸管与油漆桶 ──
    //
    // ★ 为什么紧挨着画笔橡皮，而不是放到工具箱最末尾 ★
    //   这四个工具是"同一件事的四个面"：画笔盖色、橡皮擦色、
    //   吸管取色、油漆桶灌色。用户在它们之间来回切换的频率最高
    //   （取一个色 → 画两笔 → 换个色 → 填一块），排在一起
    //   能让鼠标少走一大段路。Photoshop 的工具栏也是这个分组方式。
    //
    //   放在选区之前而不是之后，还避免了"选区把绘制类工具拦腰截断"：
    //   工具箱的顺序 = 注册顺序，一旦插错位置，用户就要跨过两个
    //   选框按钮才能从画笔走到油漆桶。
    //
    // ★ 这两个工具都不需要在这里接线 ★
    //   吸管取色后发 requestForegroundColor()，CanvasView::setActiveTool()
    //   已经把它接到了 CanvasView::setForegroundColor —— 那条链路
    //   （画布持有前景色 → 推给活动工具 → 推给选项条与工具箱色块）
    //   是**所有取色来源共用**的，吸管只是其中一个发信人。
    //   在这里再连一次会造成颜色被设两遍（当前无副作用，但两处真相）。
    //
    //   油漆桶更简单：它自己 push DrawCommand，不向任何人发请求。
    m_toolManager->addTool(new ColorPickerTool(nullptr, m_commandStack, m_toolManager));
    m_toolManager->addTool(new BucketTool(nullptr, m_commandStack, m_toolManager));

    // ── Phase 6b：直线 / 矩形 / 椭圆 ──
    //
    // ★ 为什么紧跟油漆桶、排在选框之前 ★
    //   注册顺序 = 工具箱从上到下的顺序。这五个（画笔、橡皮、吸管、油漆桶、
    //   直线/矩形/椭圆）是"往图层上直接放像素"的一组，选框与裁剪是
    //   "标范围 / 改画布"的另一组。分组之后，用户从"画东西"切到"圈范围"
    //   时鼠标只需跨过一个分隔带，而不是在两个组之间来回穿。
    //
    // ★ 三个实例 = 同一个类的三种 Kind ★
    //   交互骨架（按下定锚点 → 拖动改几何 → 松手压一条撤销）完全一致，
    //   差别只有"几何怎么算"与"调哪个光栅化函数"。
    //   拆成三个类会让"快照 / 选区 / 幂等重算 / 撤销打包 / 取消回滚"
    //   这五块复制三遍 —— 见 ShapeTool.h 顶部的说明。
    //
    // ★ 顺序：矩形必须在椭圆之前 ★
    //   ToolManager::findByShortcut() 是"命中第一个就返回"，而这两个工具
    //   故意返回同一个快捷键 U（按 U 进形状组、再按 U 换另一种形状，
    //   轮换逻辑在 MainWindow::activateToolByShortcut）。
    //   把椭圆提到前面的话，按 U 的首次落点就变成椭圆了 ——
    //   功能不会坏，但用户会觉得"U 键今天怎么不一样了"。
    //
    // ★ 它们都不需要在这里接线 ★
    //   颜色由 CanvasView 在"工具被激活"与"用户改了前景/背景色"时推给
    //   工具（setForegroundColor → 线/描边色，setBackgroundColor → 填充色）；
    //   撤销命令由工具自己 push。所以这里只有 addTool。
    m_toolManager->addTool(new ShapeTool(ShapeTool::Kind::Line, nullptr,
                                        m_commandStack, m_toolManager));
    m_toolManager->addTool(new ShapeTool(ShapeTool::Kind::Rectangle, nullptr,
                                        m_commandStack, m_toolManager));
    m_toolManager->addTool(new ShapeTool(ShapeTool::Kind::Ellipse, nullptr,
                                        m_commandStack, m_toolManager));

    // ── Phase 6c：文字 ──
    //
    // ★ 位置放在"直接往图层放像素"那一组的末尾（形状之后、选框之前）★
    //   文字与画笔、油漆桶、直线/形状是同一类：点一下就往活动图层上写东西。
    //   排在形状之后，正好收住这一组 —— 用户从"画东西"切到"圈范围"时，
    //   鼠标只需跨过一个分隔带。
    //
    // ★ 它是 14 个工具里唯一"有第二份界面"的一个 ★
    //   输入中文需要一个真的 QTextEdit 浮层，而 QTextEdit 必须挂在画布上
    //   （tools 层不允许依赖 ui 层）。所以浮层由 CanvasView 创建，工具只发
    //   requestTextEditor()、只收 setEditingText()。
    //   这里**不需要接线** —— 那条协议挂在 AbstractTool 上，
    //   CanvasView::setActiveTool() 对所有工具一视同仁地连（见那里的注释）。
    //
    // ★ 与"文字图层 + 栅格化"无关 ★
    //   本工程没有文字图层（见「明确不做的事」），文字一次落进像素层，
    //   之后就是普通像素 —— 不存在"双击再改字"，也不存在栅格化这一步。
    m_toolManager->addTool(new TextTool(nullptr, m_commandStack, m_toolManager));

    // ── Phase 3：矩形 / 椭圆选框 ──
    //
    // ★ 位置放在画笔之后、绘制类工具之前 ★
    //   选区是"先划定范围、再在范围内作画"的前置动作，
    //   紧挨着画笔放在前面符合使用顺序上的直觉。
    //
    // ★ 同样是"一个类两个实例" ★
    //   拖动、修饰键换算、预览、撤销打包这四块逻辑完全一致，
    //   只有"把路径画成矩形还是椭圆"这一行之差。
    //   参数对象共用同一个 QSettings 分组 —— 用户在矩形选框里
    //   设了羽化 5px，切到椭圆选框时不应该变回 0。
    //
    // ★ 快捷键 ★
    //   只有矩形选框绑了 M（见 SelectionTool::shortcutKey 的说明）。
    //   "已经是选区工具时按 M 切到另一种形状"由本类的
    //   activateToolByShortcut() 拦截实现，对齐 Photoshop 的手感。
    m_toolManager->addTool(new SelectionTool(SelectionTool::Kind::Rectangle, nullptr,
                                            m_commandStack, m_toolManager));
    m_toolManager->addTool(new SelectionTool(SelectionTool::Kind::Ellipse, nullptr,
                                            m_commandStack, m_toolManager));

    // ── 魔术棒（按相似颜色建选区）──
    //
    // ★ 位置紧跟两个选框之后、裁剪之前 ★
    //   它们同属"划定范围"那一组：选框按几何划、魔术棒按颜色划，
    //   裁剪则把划出来的范围变成新画布。工具箱顺序 = 注册顺序，
    //   排在一起用户从一个走到另一个不用来回找（Photoshop 也是这么排的）。
    //
    // ★ 它不需要在这里接线 ★
    //   与油漆桶同类：自己 floodFill、自己把蒙版接到选区、自己压
    //   SelectionCommand。工具之间零耦合 —— 唯一共享的是
    //   engine/Fill 的内核与 core/Selection 的布尔运算。
    m_toolManager->addTool(new MagicWandTool(nullptr, m_commandStack, m_toolManager));

    // ── Phase 4：裁剪 ──
    //
    // ★ 位置紧跟着两个选框工具 ★
    //   裁剪与选区的关系最近：选区是"标出这一块"，裁剪是"把画布收成那一块"。
    //   工具箱里排在一起，用户从一个走到另一个不用来回找。
    //
    // ★ 它与其他工具最不同的地方：不自己压撤销命令 ★
    //   框好之后发 cropRequested()，由 MainWindow 走几何操作那条统一收尾
    //   （快照 → Document::crop() → 内存预检 → 压栈）。理由见
    //   CropTool.h 顶部第 ③ 条与 MainWindow::commitGeometryChange() 的注释：
    //   内存预检这道防线不能有第二个入口。
    auto *cropTool = new CropTool(nullptr, m_commandStack, m_toolManager);
    connect(cropTool, &CropTool::cropRequested,
            this, &MainWindow::applyCropFromTool);
    m_toolManager->addTool(cropTool);

    // ── Phase 6d：视图导航（抓手 / 缩放）──
    //
    // ★ 位置放在最后 ★
    //   它们不改内容、只改"怎么看"，是工具箱里性质最特殊的一组。
    //   排在末尾与 Photoshop 的工具箱一致，也让工具箱有个
    //   "从改内容到改视图"的自然收束。
    //
    // ★ 它们不需要在这里接线 ★
    //   两个工具都不压撤销命令、不碰一个像素，只发视图请求信号
    //   （requestPan / requestZoom / requestZoomToRect）。
    //   这些信号由 CanvasView 在 setActiveTool() 里统一连接 ——
    //   与 requestForegroundColor 同一个规律：**工具只表达意图，
    //   实现归视图层**。所以这里只有两行 addTool。
    //
    // ★ 与"按住空格平移"不冲突 ★
    //   空格平移仍然归 CanvasView（它在事件派发阶段就把事件拦下了，
    //   工具根本收不到）。所以两者是不同层次的东西：抓手是"选中的工具"，
    //   空格是"随时可用的临时修饰键"，可以同时存在、互不干扰。
    m_toolManager->addTool(new HandTool(nullptr, m_commandStack, m_toolManager));
    m_toolManager->addTool(new ZoomTool(nullptr, m_commandStack, m_toolManager));

    // 至此 15 个工具全部注册完毕：
    //   移动 / 画笔 / 橡皮 / 吸管 / 油漆桶 / 直线 / 矩形 / 椭圆 / 文字 /
    //   矩形选框 / 椭圆选框 / 魔术棒 / 裁剪 / 抓手 / 缩放
    //   （Phase 6a 吸管+油漆桶、6b 直线/矩形/椭圆、6c 文字、6d 抓手/缩放，
    //     之后补上魔术棒 —— 它是第一个"算法全在 engine 里现成"的工具）

    // ★ 加载工具参数 ★
    //   必须在工具注册之后、绑定文档之前。
    //   有的工具会依赖参数来建立内部缓存，先有参数更省一次重建。
    m_toolManager->loadAllOptions();

    // ── 工具箱按钮：必须在这里建，不能更早 ──
    //
    // ★ 这是一处实测踩到的 bug（"工具箱一个按钮都没有"）★
    //   buildToolBox() 原先被放在 setupToolBars() 的末尾，而构造函数里
    //   setupToolBars() 在 setupTools() **之前**执行 —— 那时 m_toolManager
    //   还是 nullptr，buildToolBox() 第一行的卫语句直接 return，
    //   于是工具箱永远是空的，而且**悄无声息**（编译器不管、离线自检不建窗口）。
    //
    //   症状：用户找不到任何工具按钮，只能靠菜单与快捷键唤起工具。
    //   排序原则：**按钮来自 ToolManager 的工具列表，所以只能在工具
    //   全部注册完之后建。**（setupToolBars 里保留工具栏本体的创建。）
    buildToolBox();

    // ── 工具切换时通知画布 / 选项条 / 工具箱按钮 ──
    //
    // ★ 顺序：先 connect，再触发；最后显式同步一次 ★
    //
    //   这是"信号只能传达变化、传达不了现状"这个坑的正面写法。
    //   ToolManager 在连接之前已经变更过两次活动工具：
    //     ① addTool() 把第一个注册的工具设为默认活动工具；
    //     ② loadAllOptions() 恢复 QSettings 里的"上次使用的工具"。
    //   旧代码把 connect 写在 loadAllOptions() **之后**，两次 emit 全部
    //   落入真空 —— CanvasView::m_activeTool 停在 nullptr。
    //
    //   症状（实测）：打开程序后画布上**任何鼠标操作都没有反应**，
    //   因为 CanvasView::shouldDispatchToTool() 第一行就因 m_activeTool
    //   为空而返回 false。更麻烦的是"点一下工具箱里那个已经选中的按钮"
    //   也修不好：ToolManager::setActiveTool() 发现"已经是这个工具"就
    //   早退、不发信号 —— 用户必须先切到别的工具再切回来。
    connect(m_toolManager, &ToolManager::activeToolChanged,
            this, &MainWindow::applyActiveToolToUi);

    // 连接建立后**必须**再显式推一次当前状态（理由见上）。
    applyActiveToolToUi(m_toolManager->activeTool());

    // 工具请求的缩放：转给画布
    connect(m_toolManager, &ToolManager::statusMessage,
            this, [this](const QString &msg, int timeout) {
        if (m_statusBar)
            m_statusBar->showMessage(msg, timeout);
    });
}

// ============================================================================
//  applyActiveToolToUi —— 活动工具 → UI 的唯一同步入口
// ============================================================================
//
// ★ 为什么抽成函数而不是内联进 lambda ★
//   它有两个调用点：activeToolChanged 信号、以及 setupTools() 收尾处的
//   一次显式同步。各写一份的话，将来加一项同步（例如"高亮选项条标题"）
//   只会改到其中一处，出现"启动时少这一项、切换工具之后才有"的差异 ——
//   而这类差异只有重启程序才观察得到，代价极高。
void MainWindow::applyActiveToolToUi(AbstractTool *tool)
{
    if (m_canvas)
        m_canvas->setActiveTool(tool);

    if (m_optionsBar)
        m_optionsBar->setTool(tool);

    // 工具箱面板里的选中状态
    for (auto it = m_toolActions.constBegin(); it != m_toolActions.constEnd(); ++it) {
        if (it.value())
            it.value()->setChecked(tool && it.key() == tool->id());
    }

    if (m_statusBar && tool)
        m_statusBar->showMessage(tr("Current tool: %1").arg(tool->displayName()), 1500);
}

// ============================================================================
//  attachDocumentTab —— 给一个文档开一页，并切到它
// ============================================================================

int MainWindow::attachDocumentTab(Document *doc)
{
    if (!m_tabs || !doc)
        return -1;

    const int index = m_tabs->addDocumentTab(doc);
    if (index < 0)
        return -1;

    DocumentTab *tab = m_tabs->tabAt(index);
    if (!tab)
        return -1;

    // ── 这块画布的三条连接 ──
    //   每块画布各连一次（见 connectCanvas 的说明）。漏了不会报错，
    //   只是状态栏与窗口标题"不跟着走了"。
    connectCanvas(tab->canvas);

    // ── 这一页的撤销栈 → 撤销/重做动作 ──
    //   连一次就够：回调里用 stack != m_commandStack 过滤非当前页（见下）。
    wireCommandStack(tab->stack);

    // 撤销步数沿用应用级设置（新页也必须遵守，不只是启动时那几页）。
    tab->stack->setUndoLimit(m_undoLimit);

    // ── 让"控件上的当前页"与"逻辑上的当前页"一致，然后显式激活 ──
    //
    //  ★ 为什么不能只靠信号 ★
    //    DocumentTabs 在增页/摘页期间屏蔽了自己的信号（那是有意的，
    //    见那边的说明），所以 setCurrentIndex() 在"值没变"时是**彻底的静默**
    //    —— 而第一页恰好就是这种情况（QTabWidget 在第一次 addTab 时
    //    已经把它当作当前页了）。
    //    依赖信号就等于"第一页永远不会被激活"：启动后所有菜单都是灰的、
    //    画布上什么都干不了 —— 与"工具箱一个按钮都没有"同一种味道。
    {
        const QScopedValueRollback<bool> guard(m_switchingTabs, true);
        m_tabs->setCurrentIndex(index);
    }
    activateTab(index);

    return index;
}

// ============================================================================
//  activateTab —— "当前页"唯一的变化点
// ============================================================================

void MainWindow::activateTab(int index)
{
    if (!m_tabs)
        return;

    // ── ① 让活动工具先收尾 ──
    //
    //  ★ 这是"一页一块画布"带来的新义务 ★
    //    工具是**跨页共享**的（ToolManager 只有一份，工具不随页重建），
    //    而"正在画的那一笔"是工具内部的状态。
    //    旧实现里切文档会走 CanvasView::setDocument()，那里有一句
    //    "工具正忙就先 cancel()"；现在切页不再碰 setDocument，
    //    那条保护就消失了 —— 不补回来的话，在 A 页画到一半切到 B 页，
    //    松手时这一笔会落在 **B 页** 的图层上。
    //
    //    这里用 commit() 而不是 cancel()：用户已经画下去的部分应当保留，
    //    只是要在"大脑切换到另一页"之前结算掉。
    if (m_toolManager)
        m_toolManager->commitActiveTool();

    // ── ② 重指"当前页三件套" ──
    DocumentTab *tab = m_tabs->tabAt(index);
    m_doc          = tab ? tab->doc    : nullptr;
    m_commandStack = tab ? tab->stack  : nullptr;
    m_canvas       = tab ? tab->canvas : nullptr;

    // ── ②b 标尺跟着当前页走 ──
    //
    //  ★ 漏了这一步的症状极难发现 ★
    //    标尺的刻度是"当前画布那个变换"的函数。不重指的话，切页之后
    //    尺子仍按上一页的缩放画 —— 它看起来仍然是一条完全正常的尺子，
    //    只是数字与图像对不上位（"尺子上写着 200 的那一格并不在图像的第
    //    200 列"）。这属于"看起来对、其实全错"，只能靠断言钉住。
    if (m_hruler)
        m_hruler->setView(m_canvas);
    if (m_vruler)
        m_vruler->setView(m_canvas);

    // ── ③ 让 DocumentManager 跟上 ──
    //
    //  它是"真相"的镜像：负责记账（未保存汇总、按路径查重）。
    //  序号从**文档指针**反查，而不是直接用 index —— 两者本该相等，
    //  但"本该"不是一条可以依赖的性质（见 DocumentManager::indexOf 的说明）。
    if (m_docManager && m_doc) {
        const int docIndex = m_docManager->indexOf(m_doc);
        if (docIndex >= 0 && m_docManager->currentIndex() != docIndex)
            m_docManager->setCurrentIndex(docIndex);
    }

    // ── ④ 重绑所有面板（约定 7 的唯一入口）──
    applyDocumentToPanels(m_doc);

    // ── ⑤ 撤销/重做的可用状态与文本 ──
    //
    //  ★ 必须显式推 ★
    //    canUndoChanged 只在**变化**时发。从"能撤销的 A 页"切到
    //    "一次都没改过的 B 页"，B 的栈从头到尾没发过任何信号 ——
    //    菜单项会停在 A 页的状态上：明明不能撤销，却是亮的。
    syncUndoRedoActions(m_commandStack);

    updateWindowTitle();
}

// ============================================================================
//  wireCommandStack —— 撤销栈 → 撤销/重做动作
// ============================================================================
//
//  ★ 为什么"连一次 + 回调里过滤"，而不是"换页时断开旧的、连上新的" ★
//
//    后者必须把"上一个连过的栈"记在一个成员或动态属性里，然后在切换时
//    disconnect。而"断开之后、连接新栈之前"这一段，任何一次取用都可能
//    踩到已经析构的旧栈 —— 而**关掉一页的那一刻正是这个时刻**。
//
//    改成"每个栈创建时连一次、回调里用 stack != m_commandStack 过滤"之后，
//    连接的寿命就等于栈的寿命：栈被销毁，连接自动消失。
//    于是"忘记断开"和"断到野指针"这两种失败模式都不存在了。

void MainWindow::wireCommandStack(CommandStack *stack)
{
    if (!stack)
        return;

    connect(stack, &CommandStack::canUndoChanged, this,
            [this, stack](bool canUndo) {
                if (stack != m_commandStack || !m_actionUndo)
                    return;
                m_actionUndo->setEnabled(canUndo);
            });

    connect(stack, &CommandStack::canRedoChanged, this,
            [this, stack](bool canRedo) {
                if (stack != m_commandStack || !m_actionRedo)
                    return;
                m_actionRedo->setEnabled(canRedo);
            });

    // 文本（"撤销 画笔描边"比只显示"撤销"有用得多）
    connect(stack, &CommandStack::undoTextChanged, this,
            [this, stack](const QString &text) {
                if (stack != m_commandStack || !m_actionUndo)
                    return;
                m_actionUndo->setText(text.isEmpty()
                                          ? tr("Undo(&U)")
                                          : tr("Undo %1(&U)").arg(text));
            });

    connect(stack, &CommandStack::redoTextChanged, this,
            [this, stack](const QString &text) {
                if (stack != m_commandStack || !m_actionRedo)
                    return;
                m_actionRedo->setText(text.isEmpty()
                                          ? tr("Redo(&R)")
                                          : tr("Redo %1(&R)").arg(text));
            });

    // ── 修改标记 → 窗口标题 ──
    //
    //  ★ 这里**不再**调 m_doc->setModified() ★
    //    CommandStack 自己在 cleanChanged 里已经做了（见它的构造函数）。
    //    再写一遍就是"同一个事实有两处真相"：将来改一处忘一处，
    //    症状是"标题带了 * 但关闭时不问保存"。
    //    （标签页上的 * 由 DocumentTabs 自己连同一个信号刷新。）
    connect(stack, &CommandStack::cleanChanged, this,
            [this, stack](bool) {
                if (stack != m_commandStack)
                    return;
                updateWindowTitle();
            });

    // ── 撤销栈的内存占用会随步骤增减变化 → 刷新状态栏 ──
    connect(stack, &CommandStack::indexChanged, this,
            [this, stack](int) {
                if (stack != m_commandStack)
                    return;
                if (m_statusBar)
                    m_statusBar->refreshDocumentInfo();
            });
}

// ============================================================================
//  syncUndoRedoActions —— 把栈的**当前**状态显式推给动作
// ============================================================================
//
//  信号只能传达"变化"，传达不了"现状"。切页时新栈可能一次信号都没发过，
//  所以初始值必须由这里显式推一次 —— 与 setupTools() 收尾处补推
//  活动工具是同一个规律。

void MainWindow::syncUndoRedoActions(CommandStack *stack)
{
    const bool canUndo = stack && stack->canUndo();
    const bool canRedo = stack && stack->canRedo();

    if (m_actionUndo) {
        m_actionUndo->setEnabled(canUndo);
        const QString text = stack ? stack->undoText() : QString();
        m_actionUndo->setText(text.isEmpty() ? tr("Undo(&U)")
                                            : tr("Undo %1(&U)").arg(text));
    }

    if (m_actionRedo) {
        m_actionRedo->setEnabled(canRedo);
        const QString text = stack ? stack->redoText() : QString();
        m_actionRedo->setText(text.isEmpty() ? tr("Redo(&R)")
                                            : tr("Redo %1(&R)").arg(text));
    }
}

// ============================================================================
//  detachStackConsumers —— 撤销栈销毁前的统一解绑点
// ============================================================================
//
//  ★ 它解决的问题（实测段错误，不是假想）★
//
//    关掉一个标签页时，DocumentTabs::removeDocumentTab() 会
//    `delete` 掉这一页的 CommandStack（连带它内部的 QUndoStack）。
//    而**至少两个部件还握着那个栈的裸指针**：
//
//      · QUndoView（历史面板）—— QUndoView 内部的 QUndoModel 里存的是
//        `QUndoStack *m_stack`（裸指针）。换绑（setStack）的第一件事
//        就是 `disconnect(m_stack, nullptr, this, nullptr)` ——
//        对一个已释放对象调 disconnect，就是一次 use-after-free。
//
//      · StatusBarWidget —— setCommandStack() 存下裸指针，
//        而 refreshDocumentInfo() 会读 `m_stack->estimatedMemoryUsage()`；
//        它挂在 Document 的 contentChanged/canvasSizeChanged 上，
//        也就是**任何一次内容变化都可能回调到它**。
//
//    症状：关掉一个标签页 → 段错误。0 错误、0 警告，因为编译器
//    看不见"谁指向谁"，而析构后的内存往往还是原样，读得出一个
//    看似合法的指针。
//
//  ★ 为什么必须在"销毁之前"、而不是"销毁之后补绑" ★
//    补绑的那一刻已经晚了：setStack(新栈) 内部会先对**旧栈**做
//    disconnect —— 旧栈就是刚被 delete 的那一份。
//    （这与"关掉一页时先摘页、再切页、最后删文档"是同一条规律：
//      凡是"某个对象要消失"，都要先让指向它的人松手。）
//
//  ★ dyingStack == nullptr 的含义 ★
//    析构路径（~MainWindow）上没有一个"正在死的栈"可传 ——
//    那时全部都要解开，所以 nullptr 被定义为"全部解开"。
//    见 MainWindow.h 对本函数的说明。

void MainWindow::detachStackConsumers(CommandStack *dyingStack)
{
    // ── 历史面板 ──
    //  先比指针再解绑：销毁的是**后台页**的栈时，当前页的绑定不能动
    //  （clearAllTabs 那条路径上不会再有 activateTab 把它接回来）。
    if (m_undoView) {
        if (!dyingStack || m_undoView->stack() == dyingStack->undoStack())
            m_undoView->setStack(nullptr);
    }

    // ── 状态栏与活动工具 ──
    //  这两者只跟随"当前页"，所以判据是"正在销毁的是不是当前栈"。
    if (!dyingStack || dyingStack == m_commandStack) {
        if (m_statusBar)
            m_statusBar->setCommandStack(nullptr);
        if (m_toolManager)
            m_toolManager->setCommandStack(nullptr);
    }
}

// ============================================================================
//  applyDocumentToPanels —— 约定 7 的落地点
// ============================================================================

void MainWindow::applyDocumentToPanels(Document *doc)
{
    // ★ 这一个函数就是"约定 7"的全部价值所在 ★
    //
    //  所有需要文档的部件都在这里统一接收。新增一个面板时，
    //  只要在这里加一行 setDocument(doc)，就自动支持文档切换 ——
    //  不可能出现"某个面板还绑在旧文档上"的 bug。
    //
    //  多标签之后它依然是**唯一入口**，只是调用者从"DocumentManager 的
    //  信号"换成了 activateTab()。新增面板时该加的那一行没变。

    if (m_canvas)
        m_canvas->setDocument(doc);

    // ★ 新画布必须"接上"当前的工具与颜色 ★
    //
    //  每开一页都是新建一块 CanvasView，它默认没有活动工具、也没有
    //  前景/背景色。不加这几行，新页上**任何鼠标操作都没有反应** ——
    //  CanvasView::shouldDispatchToTool() 第一行就因 m_activeTool 为空
    //  返回 false。而这个症状与"工具箱一个按钮都没有"（见 setupTools()
    //  的说明）一模一样，排查时极容易跑偏。
    //
    //  颜色同理：颜色保存在 MainWindow 上（应用级状态，不随文档切换而重置），
    //  而画布是每页一份 —— 不同步的话，用户在颜色面板选了红色，
    //  在旧页上画笔是红的，切到新页却还是默认的黑。
    //
    //  网格与棋盘格也一样：它们是"怎么看"的应用级选项（存在 QSettings），
    //  不该因为开了一个新标签就回到默认值。
    if (m_canvas) {
        if (m_toolManager)
            m_canvas->setActiveTool(m_toolManager->activeTool());
        m_canvas->setForegroundColor(m_foregroundColor);
        m_canvas->setBackgroundColor(m_backgroundColor);
        if (m_actionShowGrid)
            m_canvas->setGridVisible(m_actionShowGrid->isChecked());
        if (m_actionShowCheckerboard)
            m_canvas->setCheckerboardVisible(m_actionShowCheckerboard->isChecked());

        // ★ 网格的间距/颜色/吸附同样要补推 ★
        //   它们是"应用级偏好"（存 QSettings），但**载体是画布**（每页一块）。
        //   不补的话，用户在旧页把间距调成 16，切到新页又变回 64 ——
        //   而菜单上还勾着 16，看起来像"菜单骗人"。
        //
        //   ★ 单一来源在 MainWindow 的 m_gridSpacing / m_gridColor 两个成员 ★
        //     而不是"从画布回读"——切页时旧画布已经要被换掉了，
        //     回读拿到的正是要被丢弃的那个值。
        if (m_actionSnapToGrid)
            m_canvas->setSnapToGrid(m_actionSnapToGrid->isChecked());
        m_canvas->setGridSize(m_gridSpacing);
        if (m_gridColor.isValid())
            m_canvas->setGridColor(m_gridColor);

        // 参考线的"显示 / 锁定"与网格同理：是**应用级偏好**（存 QSettings），
        // 不该因为开了一个新标签就回到默认值。不补这两行的话，
        // 用户明明勾了"锁定参考线"，新开的页上却能拖动 —— 而旧页拖不动。
        if (m_actionShowGuides)
            m_canvas->setGuidesVisible(m_actionShowGuides->isChecked());
        if (m_actionLockGuides)
            m_canvas->setGuidesLocked(m_actionLockGuides->isChecked());
    }

    if (m_statusBar)
        m_statusBar->setDocument(doc);

    // ★ 缩放比必须**显式推**一次（与 syncUndoRedoActions 是同一条规律）★
    //
    //  信号只传达"变化"，传达不了"现状"：
    //  CanvasView::viewChanged 只在缩放真的改变了的时候才发，而切页时
    //  新画布的缩放**本来就已经是它自己的值** —— applyDocumentToPanels
    //  第一行那句 setDocument() 会因此早退，一个信号都不会发。
    //  结果状态栏上那个数字停在上一个页的数值上。
    //
    //  ★ 实测症状（多标签截图里一眼看到）★
    //    在 1200×800 的页上状态栏写着「99.9%」，切到 3840×2160 的页
    //    之后仍然是「99.9%」（真实值是 31.6%）。
    //    而**窗口标题是对的** —— 它直接读 m_canvas->zoom()。
    //    同一件事有两处显示、只有一处走信号，不一致就从这里来。
    //
    //  ★ setZoom 是安全的 ★
    //    StatusBarWidget 内部有 m_updatingZoomCombo 守卫，程序化设置
    //    不会反过来触发 onZoomComboChanged 把缩放再改一遍。
    if (m_statusBar && m_canvas)
        m_statusBar->setZoom(m_canvas->zoom());

    if (m_layerPanel)
        m_layerPanel->setDocument(doc);

    // 导航器要两样东西：文档出缩略图，画布出可视框。
    if (m_navigatorPanel) {
        m_navigatorPanel->setDocument(doc);
        m_navigatorPanel->setView(m_canvas);
    }

    if (m_toolManager) {
        m_toolManager->setDocument(doc);
        m_toolManager->setCommandStack(m_commandStack);
    }

    if (m_statusBar)
        m_statusBar->setCommandStack(m_commandStack);

    if (m_undoView) {
        // QUndoView 绑定到具体的 QUndoStack。
        // 切换文档时必须重新绑定，否则历史面板显示的是旧文档的操作记录。
        m_undoView->setStack(m_commandStack ? m_commandStack->undoStack() : nullptr);
    }

    // ── 更新动作的可用状态 ──
    const bool hasDoc = (doc != nullptr);

    const QList<QAction *> docActions = {
        m_actionSave, m_actionSaveAs, m_actionClose,
        // 「打开为图层」要有文档才可用 —— 它是往**当前**文档里加东西，
        // 没有当前文档时点了只能弹一句"请先打开图像"。
        m_actionOpenAsLayer,
        m_actionSelectAll, m_actionDeselect, m_actionInvertSelection,
        m_actionGrayscale, m_actionInvert,
        m_actionRotate90CW, m_actionRotate90CCW, m_actionRotate180,
        m_actionFlipH, m_actionFlipV,
        m_actionLayerNew, m_actionLayerDuplicate, m_actionLayerDelete,
        m_actionLayerMoveUp, m_actionLayerMoveDown,
        m_actionMergeDown, m_actionFlatten,
        m_actionLayerMergeVisible, m_actionLayerRename,
        m_actionZoomIn, m_actionZoomOut, m_actionZoomFit, m_actionZoomActual,
        m_actionCut, m_actionCopy, m_actionPaste, m_actionPasteInto,
    };

    for (QAction *action : docActions) {
        if (action)
            action->setEnabled(hasDoc);
    }
}

// ============================================================================
//  updateWindowTitle
// ============================================================================

void MainWindow::updateWindowTitle()
{
    if (!m_doc) {
        setWindowTitle(zl::appinfo::titleSuffix());
        return;
    }

    // 标题格式： "文件名 * [50%] - 逐鹿 图片编辑器 (ZlPhotoEdit)"
    //   · * 表示有未保存修改（这是所有编辑软件的共同约定）
    //   · 缩放比例放在方括号里，方便用户随时知道当前比例
    //     （放在标题里比放在状态栏更容易被看到）
    //   · 后缀由 AppInfo::windowTitleFor() 统一拼 —— 拼法只有那一处，
    //     以前在这里写死 " - ZlPhotoEdit"，改中文名时漏掉就会变成
    //     "标题是中文、后缀是英文"的拼接怪
    QString name = m_doc->displayName();

    QString zoomText;
    if (m_canvas) {
        const qreal percent = m_canvas->zoom() * 100.0;
        if (qAbs(percent - qRound(percent)) < 0.05)
            zoomText = QStringLiteral("%1%").arg(qRound(percent));
        else
            zoomText = QStringLiteral("%1%").arg(percent, 0, 'f', 1);
    }

    setWindowTitle(zl::appinfo::windowTitleFor(name, zoomText));
}

} // namespace zl
