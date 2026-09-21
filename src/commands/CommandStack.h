#pragma once

// ============================================================================
//  CommandStack.h —— 撤销栈
// ----------------------------------------------------------------------------
//  决策：直接使用 Qt 原生的 QUndoStack / QUndoCommand，**不自造框架**。
//
//  理由：
//    QUndoStack 已经提供了撤销限制、clean 状态跟踪、合并（mergeWith）、
//    文本描述、索引跳转等全部需要的能力。自己写一套只会重造一遍轮子，
//    而且几乎肯定会在某些边界条件上比 Qt 的实现更容易出问题。
//
//  本类是一个**薄封装**，只加两样东西：
//    1. 统一的撤销步数限制与内存压力提示
//    2. 与 Document 的自动联动（压栈后自动 markDirty + setModified）
//
//  注意：不要在 CommandStack 里做"操作前保存快照"这类事情 ——
//  那是各个 Command 自己的职责。CommandStack 只负责栈的管理。
// ============================================================================

#include <QUndoStack>
#include <QObject>
#include <QString>

namespace zl {

class Document;

class CommandStack : public QObject
{
    Q_OBJECT

public:
    explicit CommandStack(Document *doc, QObject *parent = nullptr);
    ~CommandStack() override;

    // ── 执行与撤销 ──

    // 执行一个命令并压入撤销栈。
    //
    // ★ 所有权转移 ★
    //   QUndoStack::push() 会接管命令的所有权，命令对象由栈来 delete。
    //   因此调用处必须用 new 创建命令，且**不要**自己 delete。
    //   这是使用 QUndoStack 时最常见的错误（双重释放或内存泄漏）。
    void push(QUndoCommand *command);

    // 开始/结束一个"宏命令"。
    // 宏内的多个命令会被视为一步撤销。
    // 用途：例如"粘贴为新图层"实际上要执行"新建图层 + 写入像素"两个操作，
    // 但用户希望一次撤销就全部回退。
    void beginMacro(const QString &text);
    void endMacro();

    // ── 撤销栈操作 ──

    bool canUndo() const;
    bool canRedo() const;

    QString undoText() const;
    QString redoText() const;

    void undo();
    void redo();

    // 清空撤销栈（打开/新建文件后调用）。
    // 同时会把栈标记为 clean 状态。
    void clear();

    // ── clean 状态 ──

    // 当前是否处于"干净"状态（对应文件已保存）。
    // 与 Document::isModified() 的区别：
    //   isClean 由栈自动跟踪（撤销回到保存点时自动变回 clean）
    //   isModified 只是个标志位
    // 实际使用中以栈的 clean 状态为准更可靠 —— 用户撤销到保存点后，
    // 文件确实就等同于已保存的状态了。
    bool isClean() const;

    void setClean();

    // ── 配置 ──

    // 设置撤销步数上限。默认 30。
    // 这个数值不能太大：整层快照类的命令（滤镜、几何变换）每步可能
    // 占用几十到几百 MB，30 步已经在内存上很紧张了。
    void setUndoLimit(int limit);
    int  undoLimit() const;

    // 底层 QUndoStack 的访问。
    // 历史面板（QUndoView）需要直接挂到它上面。
    QUndoStack *undoStack() { return m_stack; }
    const QUndoStack *undoStack() const { return m_stack; }

    // ── 内存估算 ──

    // 估算撤销栈当前占用的内存（字节）。
    //
    //  这是**估算**：QUndoCommand 没有统一的"内存占用"接口，
    //  所以这里通过一个约定实现 —— 实现了 IMemoryAware 接口的命令
    //  可以报告自己的占用，其他命令按一个保守的固定值估算。
    //
    //  用途：状态栏显示"撤销栈占用"，以及内存紧张时提示用户清理。
    qint64 estimatedMemoryUsage() const;

signals:
    // 转发 QUndoStack 的信号，方便 MainWindow 连接
    void canUndoChanged(bool canUndo);
    void canRedoChanged(bool canRedo);
    void cleanChanged(bool clean);
    void undoTextChanged(const QString &text);
    void redoTextChanged(const QString &text);
    void indexChanged(int index);

private:
    QUndoStack *m_stack = nullptr;
    Document   *m_doc = nullptr;
};

// ============================================================================
//  可选接口：让命令能报告自己的内存占用
// ============================================================================
//  实现方式：命令类额外继承这个接口并实现 memoryUsage()。
//  CommandStack 用 dynamic_cast 探测（每个命令只探测一次，开销可忽略）。
//
//  为什么不放进 QUndoCommand 的基类：那需要改动 Qt 的类，不可行。
//  为什么不用虚函数表约定：dynamic_cast 是标准的、安全的做法。
class IMemoryAware
{
public:
    virtual ~IMemoryAware() = default;
    virtual qint64 memoryUsage() const = 0;
};

} // namespace zl
