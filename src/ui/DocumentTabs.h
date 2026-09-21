#pragma once

// ============================================================================
//  DocumentTabs.h —— 多标签页的中央控件
// ----------------------------------------------------------------------------
//  这是"阶段八：多标签"唯一**新增**的类。它回答一个问题：
//
//      一个标签页里到底有哪些东西？
//
//  答：三样，而且必须同生同死 ——
//
//      Document     *doc;      // 内容（所有权不在这里，见下）
//      CommandStack *stack;    // 这一页自己的撤销历史
//      CanvasView   *canvas;   // 这一页自己的视图状态（缩放 / 平移 / 选区蚂蚁线）
//
//  ★ 为什么撤销栈必须**每页一个** ★
//    Command 内部持有 Document*（一个命令要知道自己作用在哪个文档上）。
//    全工程共用一个栈的后果是：在 B 页按 Ctrl+Z，撤销掉的是 A 页的操作 ——
//    而 A 页的画面不会跟着变，用户看到的是"撤销了但什么都没发生"。
//    所以栈与页一一对应。
//
//  ★ 为什么画布也必须**每页一个** ★
//    "视图状态"（缩放比、平移量、是否适合窗口）是用户对**这一张图**的设置。
//    共用一个画布控件的话，切到 B 页就会继承 A 页的缩放 ——
//    一张 200×200 的图标和一张 6000×4000 的照片用同一个缩放比，
//    前者小得看不清、后者大到只看见一个色块。
//
//  ★ 本类**只管容器**，不碰业务 ★
//    它不弹"要保存吗"、不 delete Document、不压任何命令。
//    理由与图层面板完全相同（见 ui/LayerPanel.h）：
//    "关闭一页"要问用户、要调用保存、要处理取消 —— 那些都是 MainWindow
//    的职责。容器只报告"用户点了第 N 页的 ×"，然后**什么都不做**。
//    这样"关一页"这件事全工程只有一条路径，不会出现
//    "从菜单关"与"点 × 关"行为不同的情况。
//
//  ★ 所有权 ★
//    · CommandStack 与 CanvasView 的 QObject parent 都是本控件 ——
//      它们随本控件一起销毁，不需要谁手工 delete。
//    · Document **不归本控件**。它由 DocumentManager 持有，
//      本控件只借一个指针（与 LayerPanel 借文档指针是同一个约定）。
//      所以销毁页的那一刻必须**先摘页、后销毁文档**（见 .cpp 里的说明）。
// ============================================================================

#include <QTabWidget>
#include <QList>
#include <QString>

namespace zl {

class Document;
class CommandStack;
class CanvasView;

// ---------------------------------------------------------------------------
//  一个标签页的全部内容
// ---------------------------------------------------------------------------
struct DocumentTab
{
    Document     *doc    = nullptr;   // 借用（所有权在 DocumentManager）
    CommandStack *stack  = nullptr;   // 本控件持有（QObject parent）
    CanvasView   *canvas = nullptr;   // 本控件持有（同时是 QTabWidget 的一页）
};

// ---------------------------------------------------------------------------
//  DocumentTabs
// ---------------------------------------------------------------------------
class DocumentTabs : public QTabWidget
{
    Q_OBJECT

public:
    explicit DocumentTabs(QWidget *parent = nullptr);
    ~DocumentTabs() override;

    // ── 增删页 ──

    // 为 doc 新建一页（同时建好它的撤销栈与画布）。
    // 返回新页的下标。**不会**自动把它设为当前页 —— 切页由调用方显式做，
    // 因为"新建一个文档就跳到它"与"在后台打开一个文档"是两种不同的意图，
    // 本控件不该替调用方猜。
    //
    // ★ 为什么在这里建栈与画布，而不是让调用方建好再传进来 ★
    //   三件套（doc/stack/canvas）必须一一对应，任何一处错配都是
    //   "撤销撤到别的文档上"这类灾难。让外部各建一个再传进来，
    //   就等于把这个不变式交给了调用方去守。
    //   收在一个函数里，"一页 = 一份栈 + 一块画布"就是结构性的。
    int addDocumentTab(Document *doc);

    // 摘掉第 index 页，并销毁它的撤销栈与画布。
    // ★ 不碰 Document ★ —— 调用方紧接着去 DocumentManager 里关闭文档。
    void removeDocumentTab(int index);

    // 摘掉全部页（程序退出用）。
    // ★ 存在的理由是**析构顺序** ★
    //   MainWindow::closeEvent 里若直接 closeAll() 销毁全部文档，
    //   各页的撤销栈还活着，而栈里的命令持有 Document* ——
    //   于是出现"栈还在、文档已经没了"的窗口期。
    //   先清空页（栈与画布一起走），再销毁文档，这个窗口期就不存在。
    void clearAllTabs();

    // ── 查询 ──

    int          indexOfDocument(const Document *doc) const;
    DocumentTab *tabAt(int index) const;
    DocumentTab *currentTab() const;

    // ── 标题 ──

    // 第 index 页的标题文字（含修改标记）。
    //
    // ★ 为什么要自己拼，而不直接用 Document::displayName() ★
    //   两个都还没保存过的文档，displayName() 都返回"未命名" ——
    //   并排开两个标签就分不清谁是谁了（而窗口标题只有一个文档，
    //   用不编号的"未命名"反而是对的，所以那边不改）。
    //   所以标签页给它补一个序号："未命名-1"、"未命名-2"。
    QString tabTitle(int index) const;

    // 重建标签文字。修改标记（*）与序号都可能变化。
    void refreshTabTitle(int index);
    void refreshAllTabTitles();

signals:
    // 用户点了某一页的 ×（还没有关闭任何东西）。
    // ★ 只报告，不关闭 ★ —— 见文件头"本类只管容器"。
    void closeTabRequested(int index);

    // ★ 第 index 页的撤销栈**即将**被销毁 ★
    //
    //  发出时机：removeDocumentTab() 的第一行 —— 此时栈、画布、文档
    //  **都还活着**，收到信号的一方可以从容解绑。
    //
    //  ★ 为什么必须有这个信号（实测踩到的段错误）★
    //    "谁持有撤销栈的指针"这个问题，答案不在本类里：
    //      · QUndoView（历史面板）的内部模型存的是 **裸** QUndoStack*
    //        —— 它换绑时会先对旧栈 disconnect()，对已释放对象调
    //        disconnect 就是一次 use-after-free，**当场段错误**；
    //      · 状态栏存了 CommandStack*，而它在文档内容变化时会被回调
    //        （refreshDocumentInfo 里要读栈的内存占用）。
    //
    //    这些消费者都在 MainWindow 那一侧。而销毁栈的是本类 ——
    //    两边只有靠一个信号才能对齐时机。
    //
    //  ★ 为什么不做成"调用方记得先解绑" ★
    //    那样"销毁栈"这件事就有了一条隐藏的前置条件。将来任何一个
    //    新的调用点（"关闭其他标签页"、"关闭所有"…）忘了它，症状都是
    //    随机段错误 —— 而且只在"被销毁的恰好是当前页"时才出现。
    //    做成信号，前置条件就变成了**结构性的**：想漏也漏不掉。
    void aboutToRemoveTab(int index);

private:
    // 与 QTabWidget 的页一一对应的数据（元素所有权归本类）。
    // 不变式：m_tabs.size() == count()，且 m_tabs[i] 就是第 i 页。
    QList<DocumentTab *> m_tabs;
};

} // namespace zl
