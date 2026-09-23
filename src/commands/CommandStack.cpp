#include "commands/CommandStack.h"
#include "core/Document.h"

#include <QUndoCommand>

namespace zl {

CommandStack::CommandStack(Document *doc, QObject *parent)
    : QObject(parent)
    , m_stack(new QUndoStack(this))
    , m_doc(doc)
{
    // 默认撤销上限 30 步。理由见头文件说明。
    m_stack->setUndoLimit(30);

    // ── 转发信号 ──
    connect(m_stack, &QUndoStack::canUndoChanged,
            this, &CommandStack::canUndoChanged);
    connect(m_stack, &QUndoStack::canRedoChanged,
            this, &CommandStack::canRedoChanged);
    connect(m_stack, &QUndoStack::undoTextChanged,
            this, &CommandStack::undoTextChanged);
    connect(m_stack, &QUndoStack::redoTextChanged,
            this, &CommandStack::redoTextChanged);
    connect(m_stack, &QUndoStack::indexChanged,
            this, &CommandStack::indexChanged);

    // ── clean 状态联动 ──
    //
    // QUndoStack 的 cleanChanged 语义是"栈是否处于 clean 点"。
    // 关键细节：clean 是**栈的属性**，撤销回到 clean 点后它会自动变回 true。
    // 这正是我们想要的 —— 用户撤销掉所有改动后，文件确实就等同于
    // 保存时的状态，此时关闭不该弹"要保存吗"。
    connect(m_stack, &QUndoStack::cleanChanged, this,
            [this](bool clean) {
                emit cleanChanged(clean);

                if (!m_doc)
                    return;

                // 同步到 Document 的修改标志。注意这里是"取反"：
                // 栈 clean == 文档未修改。
                m_doc->setModified(!clean);
            });
}

CommandStack::~CommandStack()
{
    // m_stack 是 QObject 的子对象，会由 Qt 自动析构。
    // 这里显式声明析构函数是为了让编译器在 .cpp 里生成它 ——
    // 这样 QUndoStack 的完整定义在此时可见（头文件里只前置声明也行，
    // 但用完整包含更清晰）。
}

// ===========================================================================
//  失效范围的推导
// ===========================================================================

namespace {

// 取一个命令的"失效区域提示"。
//
//   · 命令实现了 IRegionAware   → 用它的矩形
//   · 否则若是宏命令（有子命令）→ 取全部子命令矩形的并集
//   · 都说不清                  → 返回空矩形 = 调用方走全量失效（兜底）
//
// ★ 空矩形是"我不知道"，不是"什么都没变" ★
//   "什么都没变"的命令根本不该被压栈。把"不知道"和"没变化"混成一个值，
//   就会在某个分支上把全量失效静默降级成"什么都不重绘"。
QRect regionHintOf(const QUndoCommand *cmd)
{
    if (!cmd)
        return QRect();

    if (const auto *region = dynamic_cast<const IRegionAware *>(cmd))
        return region->dirtyRegion();      // 空就是空 → 全量

    // 宏命令：只要有一个子命令说不清，整体就当说不清。
    // 部分生效比全量失效危险得多 —— 后者只是慢，前者会留下一块旧画面。
    if (cmd->childCount() > 0) {
        QRect acc;
        for (int i = 0; i < cmd->childCount(); ++i) {
            const QRect r = regionHintOf(cmd->child(i));
            if (r.isEmpty())
                return QRect();
            acc = acc.isNull() ? r : acc.united(r);
        }
        return acc;
    }

    return QRect();
}

// 命令执行完后通知 Document 刷新。
//
// ★ 全量是默认，局部是"命令敢于精确声明"时的奖励 ★
//   改造前这里只有一句 m_doc->markDirty()。现在把"失效多大范围"
//   也收敛到这一个函数里，集中在一处的好处（新加命令不可能忘记失效）
//   继续成立 —— 而且下面三个调用点不论走哪条分支都必然失效。
void notifyDocument(Document *doc, const QRect &hint)
{
    if (!doc)
        return;

    if (!hint.isEmpty()) {
        doc->markDirtyRegion(hint);
        return;
    }

    doc->markDirty();
}

} // namespace

// ===========================================================================
//  执行
// ===========================================================================

void CommandStack::push(QUndoCommand *command)
{
    if (!command)
        return;

    // ★ 区域提示必须在 push() **之前**取 ★
    //   QUndoStack::push() 在 mergeWith() 成功时会**删除**这个命令对象 ——
    //   push() 返回之后再访问 command 就是 use-after-free
    //   （而它只在"连续同类命令"时才发生，属于典型的偶发崩溃）。
    //
    //   合并时取"新命令自己那块"也是正确的：被并入的前一条命令
    //   当时已经失效并合成了，缓存里早就是它的最新结果，
    //   这次真正新增的像素变化只有新命令这一块。
    const QRect hint = regionHintOf(command);

    // push 会立即执行 command->redo()，然后压入栈。
    // 所有权转移给栈 —— 调用方不要再 delete。
    m_stack->push(command);

    // ── 自动通知 Document 刷新 ──
    //
    // 这一处是"自动同步"的关键。命令执行后必须让 Document 的合成缓存
    // 失效，否则画面上看到的内容不会变（缓存还在用旧数据）。
    //
    // 为什么不让每个 Command 自己在 redo() 里调 markDirty()？
    // 因为那样容易漏 —— 新加一个命令时忘记调用，症状是"操作后画面没反应，
    // 但再点一下别的工具又好了"，极难定位。
    // 集中在这里调用就万无一失：任何压栈的命令都自动触发刷新。
    //
    // ★ 为什么值得为 DrawCommand 单独走局部失效 ★
    //   DrawCommand 是最高频的命令类（画笔/橡皮/油漆桶/形状/文字）。
    //   改造前每画完一笔松手都要整张画布重算一次 ——
    //   实测 4K 431 ms、8K 4134 ms 的停顿，而那一笔往往只改了几十 KB。
    notifyDocument(m_doc, hint);
}

void CommandStack::beginMacro(const QString &text)
{
    m_stack->beginMacro(text);
}

void CommandStack::endMacro()
{
    m_stack->endMacro();

    // 宏结束时也要刷新 —— 宏内的命令虽然各自也会触发 push()，
    // 但为避免将来的实现变化（例如宏内不走 push），这里保险起见再刷一次。
    // 宏的成员往往类型不一（新建图层 + 写像素），范围不好统一推导，
    // 这里就用全量：宏命令本来就是"低频、成组"的操作。
    if (m_doc)
        m_doc->markDirty();
}

// ===========================================================================
//  栈操作
// ===========================================================================

bool CommandStack::canUndo() const
{
    return m_stack->canUndo();
}

bool CommandStack::canRedo() const
{
    return m_stack->canRedo();
}

QString CommandStack::undoText() const
{
    return m_stack->undoText();
}

QString CommandStack::redoText() const
{
    return m_stack->redoText();
}

void CommandStack::undo()
{
    if (!m_stack->canUndo())
        return;

    m_stack->undo();

    // undo 不会走 push()，所以这里必须手动刷新 ——
    // 这是最容易漏的一处：漏了之后症状是"撤销后画面不变，直到下一次操作才更新"。
    //
    // 被撤销的命令现在位于 index() 处（undo() 把索引减了 1）。
    // ★ 从栈里取，不要自己记指针 ★ 撤销的是"栈里那条"，可能是个宏，
    //   也可能在 undoLimit 裁剪后换了对象；问栈最可靠。
    notifyDocument(m_doc, regionHintOf(m_stack->command(m_stack->index())));
}

void CommandStack::redo()
{
    if (!m_stack->canRedo())
        return;

    m_stack->redo();

    // 刚被重做的命令现在位于 index() - 1 处。
    const int done = m_stack->index() - 1;
    notifyDocument(m_doc,
                   regionHintOf(done >= 0 ? m_stack->command(done) : nullptr));
}

void CommandStack::clear()
{
    m_stack->clear();
    // clear() 会把栈重置为 clean 状态（默认行为），
    // 适用于"新建/打开文件后，历史记录从头开始"的场景。
}

// ===========================================================================
//  clean 状态
// ===========================================================================

bool CommandStack::isClean() const
{
    return m_stack->isClean();
}

void CommandStack::setClean()
{
    m_stack->setClean();
}

// ===========================================================================
//  配置
// ===========================================================================

void CommandStack::setUndoLimit(int limit)
{
    // 下限 1 —— 设为 0 会禁用撤销，但那不是我们想要的语义。
    // 上限 200 —— 再多对内存没有实际意义，反而让用户困惑。
    m_stack->setUndoLimit(qBound(1, limit, 200));
}

int CommandStack::undoLimit() const
{
    return m_stack->undoLimit();
}

// ===========================================================================
//  内存估算
// ===========================================================================

qint64 CommandStack::estimatedMemoryUsage() const
{
    qint64 total = 0;

    // QUndoStack 提供了按索引访问命令的能力。
    // 注意 count() 返回的是命令总数（含已撤销但仍保留的）。
    const int count = m_stack->count();

    for (int i = 0; i < count; ++i) {
        const QUndoCommand *cmd = m_stack->command(i);
        if (!cmd)
            continue;

        // 探测是否实现了内存报告接口
        if (const auto *aware = dynamic_cast<const IMemoryAware *>(cmd)) {
            total += aware->memoryUsage();
        } else {
            // 未实现接口的命令按保守值估算。
            // 512 字节是一个经验值：一个普通的 QUndoCommand 子类
            // （例如改一个属性值）加上 Qt 内部的开销大约是几百字节。
            total += 512;
        }
    }

    return total;
}

} // namespace zl
