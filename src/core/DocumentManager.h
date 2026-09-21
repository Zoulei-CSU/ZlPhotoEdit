#pragma once

// ============================================================================
//  DocumentManager.h —— 文档管理抽象
// ----------------------------------------------------------------------------
//  为什么在第一版（单文档）就要做这层抽象？
//
//    MainWindow 如果直接持有 Document*，那么后面要做多标签页时，
//    所有引用 m_doc 的地方都要改成 m_docs->currentDocument()，
//    而且面板、状态栏、导航器的绑定逻辑都要重写。
//
//    成本对比：
//      · 现在抽象 —— 多写一个约 150 行的类，MainWindow 里多敲几个字符
//      · 以后重构 —— 改几十处调用点，且容易漏掉导致空指针崩溃
//
//    所以即使第一版只支持单个文档，也把这一层做出来。
//
//  同时这个类承担另一个职责：**文档的生命周期管理**。
//  Document 用裸指针存，由 DocumentManager 负责 delete。
//  用 QSharedPointer 也可以，但那会让剪贴板的 sourceDocumentId
//  （基于指针地址的比较）语义变得复杂，不值得。
// ============================================================================

#include "core/Document.h"

#include <QObject>
#include <QList>
#include <QString>

namespace zl {

class DocumentManager : public QObject
{
    Q_OBJECT

public:
    explicit DocumentManager(QObject *parent = nullptr);
    ~DocumentManager() override;

    // ── 文档的创建与销毁 ──

    // 新建空白文档并设为当前文档。返回新文档的指针。
    Document *createDocument(const QSize &size,
                             const QColor &fillColor = Qt::transparent);

    // 从文件打开。失败时返回 nullptr（错误信息通过 lastError() 取）。
    Document *openDocument(const QString &filePath);

    // 把已有文档纳入管理（转移所有权）。
    // 返回文档在列表中的索引。
    int addDocument(Document *doc);

    // 关闭指定文档（会 delete 它）。
    // 返回 false 表示索引无效。
    bool closeDocument(int index);

    // 关闭全部文档。用于程序退出。
    void closeAll();

    // ── 当前文档 ──

    Document *currentDocument() const;
    int currentIndex() const { return m_currentIndex; }

    // 切换当前文档。索引无效时忽略。
    void setCurrentIndex(int index);

    // ── 查询 ──

    int count() const { return m_docs.size(); }
    bool isEmpty() const { return m_docs.isEmpty(); }

    Document *documentAt(int index) const;

    // 某个文档是否由本管理器持有。
    // 剪贴板用它判断"这个数据是不是来自我们自己管理的文档"。
    bool isOwnerOf(const Document *doc) const;

    // 通过文件路径查找已打开的文档。找到返回索引，否则 -1。
    // 用途：用户重复打开同一个文件时，应该切换到已有标签而不是新开一个。
    int indexOfPath(const QString &filePath) const;

    // 通过文档指针查找下标。找到返回索引，否则 -1。
    //
    //  ★ 它是"关标签页"必需的一环 ★
    //    标签页只知道"要关的是这个 Document*"，而关闭文档要的是下标。
    //    不要用"标签页序号 == 文档序号"这个巧合来替代它 ——
    //    那个等价关系只在"增删总是成对且同步"时才成立，一旦哪天
    //    有人在中间插了一步（例如先建文档后开页），它就静默失效，
    //    而症状是"关掉的是另一份文档"。查一次指针是 O(n) 且永不失效。
    int indexOf(const Document *doc) const;

    // ── 错误信息 ──

    // 最近一次操作失败的原因（面向用户的中文描述）
    QString lastError() const { return m_lastError; }
    void    clearError() { m_lastError.clear(); }

    // ── 修改状态汇总 ──

    // 是否有任何文档有未保存的修改。
    // MainWindow 的 closeEvent 用它决定要不要弹提示。
    bool hasUnsavedChanges() const;

    // 返回所有有未保存修改的文档路径列表（空路径用"未命名 N"表示）
    QStringList unsavedDocumentNames() const;

    // ── 多标签预留 ──
    //
    // 下面这些方法在第一版（单文档）里返回值是平凡的，但接口先摆出来，
    // 让 MainWindow 的代码写成"多文档正确"的形式。
    // 接 QTabWidget 时只需要改 MainWindow 的中央控件，不必动调用逻辑。

    // 标签标题（含 * 修改标记）
    QString titleAt(int index) const;

    // 全部标签标题
    QStringList allTitles() const;

signals:
    // 当前文档切换。所有面板都监听它来重新绑定 ——
    // 这是多标签化的关键信号，见 架构规划.md 3.8 节的改造步骤。
    void currentChanged(Document *doc);

    void documentAdded(Document *doc);
    void documentRemoved(Document *doc);

    // 文档的修改状态变化（用于更新标签上的 * 标记）
    void documentModifiedChanged(Document *doc, bool modified);

private:
    // 建立与文档的信号连接，把文档状态变化转发成管理器级别的信号
    void connectDocument(Document *doc);

    QList<Document *> m_docs;
    int               m_currentIndex = -1;
    QString           m_lastError;
};

} // namespace zl
