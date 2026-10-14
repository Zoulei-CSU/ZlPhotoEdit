#include "ui/DocumentTabs.h"

#include "commands/CommandStack.h"
#include "core/Document.h"
#include "ui/CanvasView.h"

#include <QFileInfo>
#include <QSignalBlocker>
// qDeleteAll 在 QtAlgorithms 里。多数平台上 <QList> 会顺带拉进来，
// 但那是"实现细节"—— 换个 Qt 版本就报"找不到 qDeleteAll"，
// 而报错信息完全看不出是缺 include。
#include <QtAlgorithms>

namespace zl {

// ============================================================================
//  构造 / 析构
// ============================================================================

DocumentTabs::DocumentTabs(QWidget *parent)
    : QTabWidget(parent)
{
    setTabsClosable(true);   // 每页一个 ×
    setMovable(true);        // 允许拖拽排序（"把这两页换个位置比对"是很常见的动作）

    // ★ 为什么转发一次，而不是让外部直接连 QTabWidget::tabCloseRequested ★
    //   外部（MainWindow）只该看见本控件"说了什么"。直接连基类信号的话，
    //   将来本控件换实现（自绘标签栏、加"全部关闭"按钮…）就会把外面的
    //   连接全部打断，而打断是静默的 —— 症状是"点 × 没反应"。
    connect(this, &QTabWidget::tabCloseRequested,
            this, &DocumentTabs::closeTabRequested);
}

DocumentTabs::~DocumentTabs()
{
    // m_tabs 里的 struct 是本类 new 出来的，必须自己回收。
    // CommandStack / CanvasView **不需要**在这里删 ——
    // 它们的 QObject parent 就是本控件，基类析构时会一并销毁。
    qDeleteAll(m_tabs);
    m_tabs.clear();
}

// ============================================================================
//  增删页
// ============================================================================

int DocumentTabs::addDocumentTab(Document *doc)
{
    if (!doc)
        return -1;

    auto *tab = new DocumentTab;
    tab->doc = doc;

    // ── 这一页自己的撤销栈 ──
    //   parent = 本控件 → 随本控件销毁；Command 内部持有 doc 指针，
    //   所以栈与文档必须同生同死（见文件头）。
    tab->stack = new CommandStack(doc, this);

    // ── 这一页自己的画布 ──
    //   setDocument 会连上文档的三个信号并做一次"适合窗口"。
    //   视图状态（缩放/平移）从此属于这一页，切页不会再互相污染。
    tab->canvas = new CanvasView(this);
    tab->canvas->setDocument(doc);

    // ★ 先登记、再 addTab，并且整段屏蔽本控件的信号 ★
    //
    //  ① 先登记：addTab 之后立刻就会有人回查 tabAt()/currentTab()，
    //     那时 m_tabs 必须已经和控件对齐。
    //  ② 屏蔽信号：QTabWidget 在"这是第一页"时会**自己**把新页设为
    //     当前页并立刻发 currentChanged。但那一刻调用方还没把这一页的
    //     撤销栈接好、三个"当前页指针"也还没重绑 —— 外面收到的是一个
    //     **半成品当前页**。切页由调用方在准备好之后显式做
    //     （见 MainWindow::attachDocumentTab）。
    //
    //  ★ 屏蔽的只是本控件的信号 ★
    //    QTabWidget 内部靠 QTabBar::currentChanged 驱动的私有槽照常运行，
    //    所以页的显示切换不受影响 —— 被压住的只有"通知外界"这一步。
    int index = -1;
    {
        const QSignalBlocker blocker(this);

        m_tabs.append(tab);
        index = addTab(tab->canvas, QString());

        // QTabWidget::addTab 永远是"追加"，走到这里说明记账与控件错位了。
        // 不静默接受 —— 一旦错位，后面每一次 tabAt() 都会指向错的那一页。
        if (index != m_tabs.size() - 1)
            m_tabs.move(index, m_tabs.size() - 1);
    }

    // ★ 标题要跟着"修改状态"走 ★
    //   用户画一笔 → 栈变脏 → 标签上出现 *。
    //   不连这个信号，标签上的 * 永远不会出现 —— 而它恰恰是
    //   "这一页改了但没保存"的唯一提示（窗口标题只说得清当前那一页）。
    connect(tab->stack, &CommandStack::cleanChanged, this,
            [this, doc](bool) {
                const int i = indexOfDocument(doc);
                if (i >= 0)
                    refreshTabTitle(i);
            });

    refreshAllTabTitles();   // 序号会随页数变化，别的页标题也要跟着改
    return index;
}

void DocumentTabs::removeDocumentTab(int index)
{
    DocumentTab *tab = tabAt(index);
    if (!tab)
        return;

    // ★★ 第一件事：告诉外界"这一页的撤销栈马上就没" ★★
    //
    //  必须在这里 —— 这一行之前，栈、画布、文档**都还活着**；
    //  这一行之后（m_tabs.removeAt 起）就开始拆了。
    //
    //  ★ 为什么不是"拆完之后再通知" ★
    //    那样已经晚了：持有栈指针的一方（历史面板 QUndoView 的内部模型
    //    存的是裸 QUndoStack*）会在这中间被别的路径回调到，
    //    对已释放对象调 disconnect() 就是一次 use-after-free。
    //    实测症状：关掉一个标签页 → 段错误（0 错误 0 警告）。
    //
    //  ★ 这个信号**不会**被本类的信号屏蔽挡住 ★
    //    屏蔽只发生在下面 removeTab() 那一小段里（那是为了压住
    //    QTabWidget 自动挑相邻页时发的 currentChanged）。
    emit aboutToRemoveTab(index);

    m_tabs.removeAt(index);

    // ★ removeTab 只是把页从标签栏摘下来，**不 delete 控件** ★
    //   它还会把控件的 parent 置空 —— 也就是说从这一刻起这两样归我们管。
    //
    //   顺便屏蔽本控件的信号：摘页会让 QTabWidget 自动挑一个相邻页
    //   成为当前页并发 currentChanged，而"该切到哪一页"应当由调用方
    //   在文档也删掉之后再决定（否则会短暂指向一个即将消失的文档）。
    {
        const QSignalBlocker blocker(this);
        removeTab(index);
    }

    // ── 为什么用 delete 而不是 deleteLater ──
    //   摘页的调用方（MainWindow::closeDocumentTab）紧接着就会去销毁
    //   Document。若这里改用 deleteLater，画布会一直活到事件循环转起来
    //   的那一刻 —— 中间就存在"画布活着、它指着的那份 Document 已经没了"
    //   的窗口期。deleteLater 的初衷是"别在信号回调里自杀"，
    //   而这里的发送者是 QTabBar，不是画布，所以同步删除是安全的。
    delete tab->canvas;
    delete tab->stack;

    delete tab;

    // 序号会变（"未命名-3" 可能变成 "未命名-2"），全部重刷一遍
    refreshAllTabTitles();
}

void DocumentTabs::clearAllTabs()
{
    // ★ 这里**故意不加** QSignalBlocker ★
    //
    //  本函数原先在循环外整体屏蔽了自己的信号，理由是"摘页会一路把
    //  currentChanged 发出来"。但那个屏蔽现在是**多余的、而且有害的**：
    //
    //   ① 多余：removeDocumentTab() 已经把 removeTab() 那一小段
    //      （唯一会发 currentChanged 的地方）单独屏蔽了。
    //      "连续摘很多页"与"用户连点很多次 ×"在信号层面完全一样，
    //      而后者是有实测覆盖的（关到一页不剩的那组断言）。
    //
    //   ② 有害：整体屏蔽会连 aboutToRemoveTab 一起挡掉 ——
    //      于是"逐页销毁撤销栈"这件事外界完全看不到，历史面板里
    //      留下一个指向已释放栈的裸指针，程序退出时踩空。
    //      这正是"一个图省事的兜底屏蔽，吃掉了后来新增的信号"的典型。
    //
    //  所以屏蔽的责任下放到唯一需要它的那一段（见 removeDocumentTab）。
    while (count() > 0)
        removeDocumentTab(0);
}

// ============================================================================
//  查询
// ============================================================================

int DocumentTabs::indexOfDocument(const Document *doc) const
{
    if (!doc)
        return -1;

    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i)->doc == doc)
            return i;
    }
    return -1;
}

DocumentTab *DocumentTabs::tabAt(int index) const
{
    if (index < 0 || index >= m_tabs.size())
        return nullptr;
    return m_tabs.at(index);
}

DocumentTab *DocumentTabs::currentTab() const
{
    return tabAt(currentIndex());
}

// ============================================================================
//  标题
// ============================================================================

QString DocumentTabs::tabTitle(int index) const
{
    const DocumentTab *tab = tabAt(index);
    if (!tab || !tab->doc)
        return QString();

    QString base;
    if (tab->doc->filePath().isEmpty()) {
        // ★ 没有磁盘来源的页优先用文档自带的"提示名" ★
        //   屏幕截图生成的页就是这种：它没路径，但"未命名-3"什么也说明不了。
        //   提示名由创建者给（MainWindow 给"屏幕截图-1"这种带序号的名字），
        //   这里不做编号 —— 编号是创建者的事，因为它才知道"第几张"。
        base = tab->doc->titleHint();
        if (base.isEmpty())
            base = tr("Untitled-%1").arg(index + 1);
    } else {
        base = QFileInfo(tab->doc->filePath()).fileName();
        if (base.isEmpty())
            base = tr("Untitled-%1").arg(index + 1);
    }

    // ★ 修改标记用栈的 clean 状态，不用 Document::isModified() ★
    //   理由与 maybeSave() 完全相同：栈的状态是自动跟踪的 ——
    //   用户撤销回到保存点时它会自己变回 clean，而 isModified 只是个
    //   手动维护的标志位，容易与实际状态脱节。
    const bool dirty = tab->stack ? !tab->stack->isClean()
                                  : tab->doc->isModified();

    return dirty ? QStringLiteral("*%1").arg(base) : base;
}

void DocumentTabs::refreshTabTitle(int index)
{
    if (index < 0 || index >= count())
        return;

    setTabText(index, tabTitle(index));

    // ★ 悬停提示给完整路径 ★
    //   两个不同目录下的 photo.jpg 从标题上完全分不出来，
    //   鼠标停一下就能看清开的到底是哪一份。
    const DocumentTab *tab = tabAt(index);
    setTabToolTip(index, (tab && tab->doc) ? tab->doc->filePath() : QString());
}

void DocumentTabs::refreshAllTabTitles()
{
    for (int i = 0; i < count(); ++i)
        refreshTabTitle(i);
}

} // namespace zl
