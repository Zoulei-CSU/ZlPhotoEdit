// ============================================================================
//  document_tabs_test.cpp —— 多标签容器的离线自检
// ----------------------------------------------------------------------------
//  这个容器里放着"当前页"的全部信息（文档 + 撤销栈 + 画布）。
//  它出错的方式全部有同一个特征：**不报错、不崩溃，只是悄悄指错**。
//  所以这组测试盯的是那几条"错了也看不出来"的性质：
//
//    ① 三件套必须一一对应，而且**每页各不相同**
//       撤销栈全工程共用一个 → 在 B 页按 Ctrl+Z 撤销掉 A 页的操作；
//       画布共用一块 → 切页时缩放比被继承（200×200 的图标与 6000×4000
//       的照片用同一个比例，一个看不清、一个只看得见色块）。
//       断言方式是"三个页的三个指针两两不等"，而不是"栈不是空指针"。
//
//    ② 摘页之后账本必须跟着动
//       忘记从 m_tabs 里删掉对应元素的话，**后面每一页都指着前一个文档** ——
//       从第 2 页起，面板显示的内容与画布上的内容不是同一份。
//       这是本文件里最重要的一条不变量，所以摘中间那一页来验。
//
//    ③ 摘页只销毁"页自己的东西"，**绝不碰 Document**
//       文档的所有权在 DocumentManager。容器顺手把文档也删了的话，
//       MainWindow 随后再 closeDocument() 就是二次释放 ——
//       而且崩在看起来毫不相干的地方。
//
//    ④ 容器只报告，不关闭
//       用户点 × 之后，容器必须发信号、并且**什么都还没发生**。
//       容器若自己就把页摘了，MainWindow 的未保存确认就永远没有机会跑。
//
//    ⑤ 标题：空文档必须带序号，修改必须带 *
//       不编号的话"未命名-1"和"未命名-2"会长得一模一样，
//       并排开两个标签就分不清谁是谁 —— 而分不清的后果是关错页。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 tabs 分支。需要 QApplication（控件）与 Qt5Widgets，
//    用 offscreen 平台插件跑。**不需要 Qt5Svg / qrc** —— 容器不画图标，
//    这同时是一条"容器不依赖图标体系"的证据。
// ============================================================================

#include "ui/DocumentTabs.h"

#include "ui/CanvasView.h"
#include "commands/CommandStack.h"
#include "core/Document.h"
#include "core/DocumentManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QPointer>
#include <QString>
#include <QTabBar>
#include <QUndoCommand>
#include <QSize>

#include <cstdio>

using namespace zl;

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
    if (actual == expected) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    }
}

static void checkEqStr(const QString &actual, const QString &expected,
                       const char *what)
{
    if (actual == expected) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n    期望「%s」\n    实际「%s」\n", what,
                    qPrintable(expected), qPrintable(actual));
    }
}

// ---------------------------------------------------------------------------
//  一个最小的可撤销命令，用来把撤销栈弄"脏"
// ---------------------------------------------------------------------------
//
//  ★ 为什么不直接找个真命令（比如 DrawCommand）★
//    这组测试盯的是**容器**，不该被命令的实现细节牵连：
//    真命令要 Document、要图层、要脏矩形，任何一处改动都会让这组
//    莫名其妙地红起来，而问题根本不在容器里。
//    计数器命令只有一个 int，红了就一定是容器的问题。
//    （"撤销栈按页独立"才是要验的性质，命令做了什么无所谓。）
class CountCommand : public QUndoCommand
{
public:
    CountCommand(int *slot, int delta, const QString &text)
        : QUndoCommand(text), m_slot(slot), m_delta(delta) {}

    void undo() override { *m_slot -= m_delta; }
    void redo() override { *m_slot += m_delta; }

private:
    int *m_slot = nullptr;
    int  m_delta = 0;
};

// 造一个文档。测试里不需要任何像素，尺寸小到瞬建。
static Document *makeDoc(const QSize &size = QSize(32, 24))
{
    return new Document(size, Qt::transparent);
}

// ============================================================================
//  ① 三件套一一对应，且每页各不相同
// ============================================================================

static void testTripleBinding()
{
    beginGroup("① 每页的三件套一一对应（文档 / 撤销栈 / 画布都各不相同）");

    DocumentTabs tabs;

    Document *d0 = makeDoc();
    Document *d1 = makeDoc(QSize(40, 30));
    Document *d2 = makeDoc(QSize(50, 40));

    checkEqInt(tabs.addDocumentTab(d0), 0, "第一页的下标是 0");
    checkEqInt(tabs.addDocumentTab(d1), 1, "第二页的下标是 1");
    checkEqInt(tabs.addDocumentTab(d2), 2, "第三页的下标是 2");
    checkEqInt(tabs.count(), 3, "共三页");

    DocumentTab *t0 = tabs.tabAt(0);
    DocumentTab *t1 = tabs.tabAt(1);
    DocumentTab *t2 = tabs.tabAt(2);

    check(t0 && t1 && t2, "tabAt(0..2) 都拿得到");
    if (!t0 || !t1 || !t2) {
        delete d0; delete d1; delete d2;
        return;
    }

    check(t0->doc == d0, "第 0 页的文档就是传进去的那个");
    check(t1->doc == d1, "第 1 页的文档就是传进去的那个");
    check(t2->doc == d2, "第 2 页的文档就是传进去的那个");

    // ★ 画布必须已经绑好它自己那一页的文档 ★
    //   没绑的话画布上没有内容（而且 zoomToFit 也没跑过）。
    check(t0->canvas && t0->canvas->document() == d0,
          "第 0 页的画布绑的是 d0（不是别人）");
    check(t1->canvas && t1->canvas->document() == d1,
          "第 1 页的画布绑的是 d1（不是别人）");
    check(t2->canvas && t2->canvas->document() == d2,
          "第 2 页的画布绑的是 d2（不是别人）");

    // ★★ 本组最关键的三条 ★★
    //   共用一块画布或一个撤销栈，功能"看起来"照样能用，
    //   只有切页时才露出马脚 —— 所以这里必须断言"互不相同"。
    check(t0->canvas != t1->canvas && t1->canvas != t2->canvas
              && t0->canvas != t2->canvas,
          "★ 三页各有自己的画布（视图状态不会跨页继承）");
    check(t0->stack != t1->stack && t1->stack != t2->stack
              && t0->stack != t2->stack,
          "★ 三页各有自己的撤销栈（撤销不会撤到别的文档上）");
    check(t0->stack && t0->stack->undoStack() != nullptr,
          "撤销栈背后真的有一个 QUndoStack");

    checkEqInt(tabs.indexOfDocument(d0), 0, "indexOfDocument(d0) == 0");
    checkEqInt(tabs.indexOfDocument(d1), 1, "indexOfDocument(d1) == 1");
    checkEqInt(tabs.indexOfDocument(d2), 2, "indexOfDocument(d2) == 2");

    // ── 查不到的两种情形 ──
    Document *notInTabs = makeDoc();
    checkEqInt(tabs.indexOfDocument(notInTabs), -1,
               "没被收进来的文档查不到（-1）");
    checkEqInt(tabs.indexOfDocument(nullptr), -1,
               "★ 传 nullptr 返回 -1 而不是崩溃");

    // ── 守卫：nullptr 不许开页 ──
    checkEqInt(tabs.addDocumentTab(nullptr), -1,
               "★ addDocumentTab(nullptr) 返回 -1（不许开出一页没有文档的标签）");
    checkEqInt(tabs.count(), 3, "被拒之后页数没变");

    // ── 越界查询 ──
    check(tabs.tabAt(-1) == nullptr && tabs.tabAt(3) == nullptr
              && tabs.tabAt(999) == nullptr,
          "★ tabAt 越界返回 nullptr（而不是越界读）");
    check(tabs.currentTab() != nullptr, "currentTab() 拿得到（第一页自动成为当前页）");

    delete notInTabs;
    // ★ 先销毁容器，再销毁文档 ★
    //   容器的每一页都握着一个 Document*（画布析构时会 disconnect 它）。
    //   反序的话，画布析构时对着已经释放的文档调 disconnect ——
    //   这是本文件里最容易在"随手加一行 delete"时踩到的坑。
    tabs.clearAllTabs();
    delete d0; delete d1; delete d2;
}

// ============================================================================
//  ② 视图状态与撤销历史必须按页独立
// ============================================================================

static void testPerTabState()
{
    beginGroup("② 视图状态与撤销历史按页独立（切页不串味）");

    DocumentTabs tabs;

    Document *d0 = makeDoc();
    Document *d1 = makeDoc();
    tabs.addDocumentTab(d0);
    tabs.addDocumentTab(d1);

    DocumentTab *t0 = tabs.tabAt(0);
    DocumentTab *t1 = tabs.tabAt(1);
    if (!t0 || !t1) {
        check(false, "两页都建好了");
        tabs.clearAllTabs();
        delete d0; delete d1;
        return;
    }

    // ── 缩放比 ──
    const qreal zoom1Before = t1->canvas->zoom();
    const QTransform xform1Before = t1->canvas->viewTransform();

    t0->canvas->setZoom(2.0);

    check(t0->canvas->zoom() != t1->canvas->zoom(),
          "★ 改第 0 页的缩放，第 1 页的缩放不受影响");
    check(qFuzzyCompare(t1->canvas->zoom(), zoom1Before),
          "第 1 页的缩放还是原来那个值");
    check(t1->canvas->viewTransform() == xform1Before,
          "第 1 页的视图矩阵没被动过");

    // ── 撤销栈 ──
    int counter0 = 0;
    int counter1 = 0;

    check(!t0->stack->canUndo(), "第 0 页一开始不能撤销");
    check(!t1->stack->canUndo(), "第 1 页一开始不能撤销");

    t0->stack->push(new CountCommand(&counter0, 1, QStringLiteral("改一下")));

    checkEqInt(counter0, 1, "命令真的执行了（counter0 == 1）");
    check(t0->stack->canUndo(), "第 0 页现在可以撤销了");
    check(!t1->stack->canUndo(),
          "★ 第 1 页仍然不能撤销（撤销栈没有串页）");

    t0->stack->undo();
    checkEqInt(counter0, 0, "第 0 页撤销成功（counter0 回到 0）");
    checkEqInt(counter1, 0, "★ 撤销第 0 页完全没有碰到第 1 页的状态");

    // ★ clean 状态也只属于自己那一页 ★
    //   两个页都 clean 时才谈得上"每页的 * 是独立的"。
    check(t0->stack->isClean(), "撤销回原点是 clean（这一点决定标题上有没有 *）");

    tabs.clearAllTabs();
    delete d0; delete d1;
}

// ============================================================================
//  ③ 标题：序号 + 修改标记
// ============================================================================

static void testTitles()
{
    beginGroup("③ 标题：空文档带序号、有路径用文件名、修改带 *");

    DocumentTabs tabs;

    Document *d0 = makeDoc();
    Document *d1 = makeDoc();
    Document *d2 = makeDoc();
    tabs.addDocumentTab(d0);
    tabs.addDocumentTab(d1);
    tabs.addDocumentTab(d2);

    // ★ 空文档必须带序号 ★
    //   Document::displayName() 对它们一律返回"未命名" ——
    //   并排开三个标签就分不清谁是谁了。而分不清的后果是关错页。
    checkEqStr(tabs.tabTitle(0), QStringLiteral("未命名-1"),
               "空文档的标题带序号：未命名-1");
    checkEqStr(tabs.tabTitle(1), QStringLiteral("未命名-2"),
               "空文档的标题带序号：未命名-2");
    checkEqStr(tabs.tabTitle(2), QStringLiteral("未命名-3"),
               "空文档的标题带序号：未命名-3");

    // ── 有路径的用文件名 ──
    d1->setFilePath(QStringLiteral("C:/some/dir/photo.jpg"));
    tabs.refreshTabTitle(1);
    checkEqStr(tabs.tabTitle(1), QStringLiteral("photo.jpg"),
               "有文件路径时标题用文件名");

    // ── 修改标记 ──
    int counter = 0;
    tabs.tabAt(0)->stack->push(new CountCommand(&counter, 1, QStringLiteral("改一下")));
    checkEqStr(tabs.tabTitle(0), QStringLiteral("*未命名-1"),
               "★ 改过之后标题前面出现 *（脏的那一页）");
    checkEqStr(tabs.tabTitle(1), QStringLiteral("photo.jpg"),
               "★ 没改的那一页不带 *（* 是按页算的，不是全局的）");
    checkEqStr(tabs.tabTitle(2), QStringLiteral("未命名-3"),
               "第 2 页也不带 *");

    // 撤销回去 → * 应该消失（clean 状态是自动跟踪的）
    tabs.tabAt(0)->stack->undo();
    checkEqStr(tabs.tabTitle(0), QStringLiteral("未命名-1"),
               "★ 撤销回原点后 * 自动消失（用的是栈的 clean 状态）");

    // ── 悬停提示给完整路径 ──
    check(tabs.tabToolTip(1).endsWith(QStringLiteral("photo.jpg")),
          "悬停提示里带着完整路径（两页同名文件靠它区分）");

    // 实际写进控件的文字也要一致（不只是 tabTitle() 这个函数的返回值）
    tabs.refreshAllTabTitles();
    checkEqStr(tabs.tabText(1), QStringLiteral("photo.jpg"),
               "refreshAllTabTitles() 真的把文字写进了标签控件");

    tabs.clearAllTabs();
    delete d0; delete d1; delete d2;
}

// ============================================================================
//  ④ 摘页：账本、销毁范围、序号重排
// ============================================================================

static void testRemove()
{
    beginGroup("④ 摘页：账本跟着动、只销毁页自己的东西、序号重排");

    DocumentTabs tabs;

    Document *d0 = makeDoc();
    Document *d1 = makeDoc();
    Document *d2 = makeDoc();
    tabs.addDocumentTab(d0);
    tabs.addDocumentTab(d1);
    tabs.addDocumentTab(d2);

    QPointer<CanvasView>   canvas1   = tabs.tabAt(1)->canvas;
    QPointer<CommandStack> stack1    = tabs.tabAt(1)->stack;
    QPointer<Document>     doc1Alive = d1;

    // ── 摘掉**中间**那一页 ──
    //   ★ 为什么特意挑中间 ★
    //     摘最后一页时，"忘记 m_tabs.removeAt()" 这个 bug 也照样能过。
    //     只有摘中间才分得开"剩下的页有没有整体前移一格"。
    tabs.removeDocumentTab(1);

    checkEqInt(tabs.count(), 2, "页数从 3 变成 2");
    checkEqInt(tabs.indexOfDocument(d0), 0, "d0 还在第 0 页");
    checkEqInt(tabs.indexOfDocument(d1), -1,
               "★ d1 已经没有页了（账本里也删掉了）");
    checkEqInt(tabs.indexOfDocument(d2), 1,
               "★ d2 前移到了第 1 页（账本整体前移，没有滞留在第 2 位）");

    DocumentTab *t0 = tabs.tabAt(0);
    DocumentTab *t2 = tabs.tabAt(1);
    check(t0 && t0->doc == d0, "★ 第 0 页还是 d0");
    check(t2 && t2->doc == d2,
          "★ 第 1 页现在是 d2（忘掉账本前移的话这里会拿到 d1）");
    check(t0 && t2 && t0->stack != t2->stack,
          "剩下的两页仍然各有自己的撤销栈");

    // ── 页自己的东西必须被销毁 ──
    check(canvas1.isNull(),
          "★ 被摘掉那一页的画布已经销毁（不是留着占内存/挂空指针）");
    check(stack1.isNull(),
          "★ 被摘掉那一页的撤销栈已经销毁（它握着的命令也一起释放）");

    // ── 文档必须**活着** ──
    //   ★ 这条是本组的核心契约 ★
    //     文档的所有权在 DocumentManager。容器顺手把它删了的话，
    //     MainWindow 随后 closeDocument() 就是二次释放 ——
    //     而崩溃点看起来毫不相干。
    check(!doc1Alive.isNull(),
          "★ 摘页**不销毁文档**（所有权在 DocumentManager，不是容器）");
    if (!doc1Alive.isNull()) {
        checkEqInt(d1->layers().count(), 1, "摘页之后文档仍然可用（图层还在）");
        checkEqInt(d1->canvasSize().width(), 32, "文档尺寸也还在");
    }

    // ── 序号重排 ──
    checkEqStr(tabs.tabTitle(0), QStringLiteral("未命名-1"), "重排后第 0 页仍是未命名-1");
    checkEqStr(tabs.tabTitle(1), QStringLiteral("未命名-2"),
               "★ 重排后第 1 页变成未命名-2（原来是 -3，不能留着旧序号）");

    // ── 越界摘页不许崩 ──
    tabs.removeDocumentTab(-1);
    tabs.removeDocumentTab(99);
    checkEqInt(tabs.count(), 2, "★ 越界摘页被挡掉（页数不变、不崩）");

    // ── 摘到一页不剩 ──
    tabs.removeDocumentTab(0);
    tabs.removeDocumentTab(0);
    checkEqInt(tabs.count(), 0, "全部摘光");
    check(tabs.currentTab() == nullptr, "★ 一页不剩时 currentTab() 返回 nullptr");
    checkEqInt(tabs.indexOfDocument(d0), -1, "摘光之后什么都查不到");

    delete d0; delete d1; delete d2;
}

// ============================================================================
//  ⑤ 容器只报告、不关闭
// ============================================================================

static void testEmitsOnly()
{
    beginGroup("⑤ 点 × 只发信号：容器自己什么都不做");

    DocumentTabs tabs;

    // ★ 没有关闭按钮就没有"点 ×"这件事 ★
    //   声明了信号却没有对应的按钮，是"功能整个不存在却不报错"的典型。
    check(tabs.tabsClosable(), "★ tabsClosable 是开着的（每页才有 × 可点）");
    check(tabs.isMovable(), "标签可以拖拽排序");

    Document *d0 = makeDoc();
    Document *d1 = makeDoc();
    tabs.addDocumentTab(d0);
    tabs.addDocumentTab(d1);

    // 旁听这个信号
    int requested = -1;
    QObject::connect(&tabs, &DocumentTabs::closeTabRequested,
                     [&requested](int index) { requested = index; });

    // ── 触发"用户点了第 1 页的 ×" ──
    //
    //  ★ 为什么不真的去点那个按钮 ★
    //    关闭按钮在不在、在哪一侧由样式决定（Windows 风格在右侧，
    //    macOS 风格在左侧），offscreen 下不同 Qt 版本还可能根本不建它。
    //    而这里要钉的不是"Qt 有没有画按钮"（那是 Qt 的事），
    //    是本类**有没有把基类信号转发出来**。
    //    直接用元对象按名字触发信号，正是"转发那条线在不在"的直接检验 ——
    //    删掉构造函数里那句 connect，这一条立刻红。
    const bool emitted = QMetaObject::invokeMethod(
        &tabs, "tabCloseRequested", Qt::DirectConnection, Q_ARG(int, 1));
    check(emitted, "基类信号可以被触发（用于检验转发）");

    checkEqInt(requested, 1, "★ 转发生效：closeTabRequested(1) 到达了");

    // ★★ 本组的核心 ★★
    //   容器只报告。它若自己就把页摘了，MainWindow 的未保存确认
    //   就永远没有机会跑 —— 而"关一页丢了没保存的修改"就是这么来的。
    checkEqInt(tabs.count(), 2,
               "★★ 收到请求之后**什么都没有关闭**（关闭的决定权在 MainWindow）");
    check(tabs.indexOfDocument(d1) == 1, "第 1 页原封不动");

    tabs.clearAllTabs();
    delete d0; delete d1;
}

// ============================================================================
//  ⑥ clearAllTabs + 析构顺序
// ============================================================================

static void testClearAll()
{
    beginGroup("⑥ clearAllTabs 与析构（退出路径）");

    DocumentTabs tabs;

    Document *d0 = makeDoc();
    Document *d1 = makeDoc();
    tabs.addDocumentTab(d0);
    tabs.addDocumentTab(d1);

    QPointer<Document> p0 = d0;
    QPointer<Document> p1 = d1;

    tabs.clearAllTabs();

    checkEqInt(tabs.count(), 0, "全部清空");
    check(tabs.currentTab() == nullptr, "清空后没有当前页");
    check(!p0.isNull() && !p1.isNull(),
          "★ 清空页**不销毁文档**（随后由 DocumentManager 统一销毁）");

    // 对空容器再清一次：必须幂等，不能崩
    tabs.clearAllTabs();
    checkEqInt(tabs.count(), 0, "对空容器再清一次也安全（幂等）");

    delete d0; delete d1;

    // ── 再验一次析构顺序 ──
    //   让容器在**文档还活着**的时候析构（这才是正确的顺序）。
    //   反序会在画布析构时 disconnect 一个已释放的文档。
    {
        DocumentTabs inner;
        Document *a = makeDoc();
        Document *b = makeDoc();
        inner.addDocumentTab(a);
        inner.addDocumentTab(b);
        // inner 在这里析构，此时 a / b 都还活着
        Q_UNUSED(a);
        Q_UNUSED(b);
    }
    check(true, "★ 容器先于文档析构时不会崩（画布 disconnect 的是活着的文档）");
}

// ============================================================================
//  ⑦ DocumentManager::indexOf —— 关页时"标签序号 → 文档序号"的翻译器
// ============================================================================

static void testManagerIndexOf()
{
    beginGroup("⑦ DocumentManager::indexOf 与关页时的索引翻译");

    DocumentManager mgr;

    Document *a = mgr.createDocument(QSize(16, 16));
    Document *b = mgr.createDocument(QSize(16, 16));
    Document *c = mgr.createDocument(QSize(16, 16));

    check(a && b && c, "三个文档都建好了");
    checkEqInt(mgr.indexOf(a), 0, "indexOf(a) == 0");
    checkEqInt(mgr.indexOf(b), 1, "indexOf(b) == 1");
    checkEqInt(mgr.indexOf(c), 2, "indexOf(c) == 2");
    checkEqInt(mgr.indexOf(nullptr), -1, "★ indexOf(nullptr) == -1");

    Document *outsider = makeDoc();
    checkEqInt(mgr.indexOf(outsider), -1, "★ 不归它管的文档返回 -1");
    delete outsider;

    // ── 关掉中间那个之后，后面的索引要整体前移 ──
    //
    //  ★ 这正是 closeDocumentTab 依赖的性质 ★
    //    它先反查出下标、再调用 closeDocument(下标)。
    //    若 indexOf 不随删除前移，"关掉第 1 页"之后就会去关第 2 个 ——
    //    而症状是"关掉了一页，但关错了"。
    check(mgr.closeDocument(1), "关掉下标 1 的文档");
    checkEqInt(mgr.indexOf(a), 0, "a 还在 0");
    checkEqInt(mgr.indexOf(c), 1, "★ c 前移到了 1");
    checkEqInt(mgr.indexOf(b), -1, "★ b 查不到了（已经销毁）");

    // ── 关掉最后一页之后管理器进入空状态 ──
    mgr.closeDocument(0);
    mgr.closeDocument(0);
    checkEqInt(mgr.count(), 0, "全部关掉");
    check(mgr.currentDocument() == nullptr, "空状态下没有当前文档");
}

// ============================================================================

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");

    // 这组测试**不量文字像素**，但 CanvasView 的 zoomToFit 会用到字体度量，
    // 字体目录缺失时 Qt 会打警告。跟同目录的 layer_panel_test 用同一条规则：
    // 只在目录真的存在时才设，免得在别的机器上把 Qt 指向不存在的路径。
    if (QDir(QStringLiteral("C:/Windows/Fonts")).exists())
        qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");

    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 多标签容器 离线自检\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    testTripleBinding();
    testPerTabState();
    testTitles();
    testRemove();
    testEmitsOnly();
    testClearAll();
    testManagerIndexOf();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
