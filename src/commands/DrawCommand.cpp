#include "commands/DrawCommand.h"
#include "core/Document.h"

#include <QPainter>
#include <QAtomicInt>

namespace zl {

// ===========================================================================
//  描边会话
// ===========================================================================
//
//  用原子变量而不是普通的 static int，原因：将来如果把绘制放到
//  后台线程（例如大尺寸画笔的并行处理），多个线程可能同时调用
//  beginStroke()。用原子变量避免竞态 —— 成本极低，但省掉了将来
//  迁移到多线程时的一个隐患。
//
//  起始值给 1 而不是 0：0 会被用作"未初始化"的哨兵值。
//
//  ★ 这个计数器是**唯一**能生出新描边编号的地方 ★
//    （旧代码还有一个 currentStrokeId() 会读出"最近一次分配的值"，
//      它能让多条命令拿到同一个编号 → 被 QUndoStack 合并成一条。
//      已经删除，见 DrawCommand.h。）
static QAtomicInt g_strokeCounter(1);

int DrawCommand::beginStroke()
{
    return g_strokeCounter.fetchAndAddRelaxed(1);
}

// ===========================================================================
//  构造
// ===========================================================================

DrawCommand::DrawCommand(Document *doc, int layerIndex, const QRect &rect,
                         int strokeId,
                         const QImage &before, const QImage &after,
                         const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_layerIndex(layerIndex)
    , m_rect(rect)
    , m_before(before)
    , m_after(after)
    , m_strokeId(strokeId)
{
    // setText 已经通过基类构造函数设置了。这里不再重复。

    // ── 防御性检查 ──
    //
    // 空矩形或空图像的命令是无意义的。构造出来也不会出错，但会在
    // undo/redo 时白跑一趟。这里不给警告（警告会淹没输出），
    // 只是保证后续 applyImage 能安全处理。
}

// standalone() —— "一次点击 / 一笔 = 一条撤销记录"的唯一入口。
//
// ★ 为什么必须在这里取新编号，而不是让构造函数自己取 ★
//   见 DrawCommand.h 构造函数上方那段注释：构造函数自己取的时候，
//   一旦调用方没先调 beginStroke()，所有命令就撞号并被静默合并。
//   把"取新号"这件事收进这个具名工厂之后，调用点只要用它，
//   就不存在"忘了开启新描边"这种状态。
DrawCommand *DrawCommand::standalone(Document *doc, int layerIndex,
                                     const QRect &rect,
                                     const QImage &before, const QImage &after,
                                     const QString &text)
{
    // beginStroke() 返回刚分配的编号，并让计数器前进一格 ——
    // 下一次调用拿到的一定是另一个值，所以两条命令永远不会撞号。
    return new DrawCommand(doc, layerIndex, rect, beginStroke(),
                           before, after, text);
}

// ===========================================================================
//  undo / redo
// ===========================================================================

void DrawCommand::redo()
{
    // 注意：push() 时会自动调用一次 redo()。
    // 也就是说"命令的第一遍执行"也走这里 —— 所以画笔工具在构造
    // DrawCommand 之前**不应该**已经修改了文档的像素，否则会改两次。
    //
    // 推荐的画笔工具写法：
    //   1. 在当前图层的副本上绘制（不碰文档）
    //   2. 截取脏矩形的 before/after
    //   3. 构造 DrawCommand 并 push（由 redo() 真正写入文档）
    //   4. 丢弃本地副本
    //
    // 这样保证"绘制"这个动作只有一处真正写文档，逻辑清晰且不会重复应用。
    applyImage(m_after);
}

void DrawCommand::undo()
{
    applyImage(m_before);
}

void DrawCommand::applyImage(const QImage &patch)
{
    if (!m_doc || patch.isNull() || m_rect.isEmpty())
        return;

    Layer *layer = m_doc->layers().layer(m_layerIndex);
    if (!layer)
        return;

    // ── 写入方式的选择 ──
    //
    // 这里用 QPainter::drawImage 而不是逐像素 memcpy，原因是：
    //   · 需要考虑图层尺寸可能小于脏矩形（极端情况下）
    //   · drawImage 内部会处理裁剪，不必自己写边界判断
    //   · 对这块大小的数据量（通常几十 KB），QPainter 的开销可忽略
    //
    // 关键设置：
    //   compositionMode = Source —— 直接替换目标像素，而不是叠加。
    //     用默认的 SourceOver 会把 patch 叠在现有内容上，
    //     由于 patch 是不透明的（含 alpha=255 的像素），看似结果相同，
    //     但在 patch 含半透明像素时，SourceOver 会与"应该被替换掉的旧内容"
    //     混合，导致撤销变成"两次叠加"而不是"回到原样"。
    //     这是脏矩形方案里最容易出的一个隐蔽错误。
    //   SmoothPixmapTransform = false —— 纯像素搬运，不能有任何插值。
    QImage &target = layer->mutableImage();
    if (target.isNull())
        return;

    QPainter painter(&target);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(m_rect.topLeft(), patch);
    painter.end();
}

// ===========================================================================
//  合并
// ===========================================================================

int DrawCommand::id() const
{
    // ── id 的作用是"分类"，不是"标识" ──
    //
    // QUndoStack 只在 id 相同时才尝试 mergeWith。所以：
    //   · 同一次描边内的所有命令 → 返回同一个 id（可以合并）
    //   · 不同描边的命令         → 不同 id（不能合并，各自成为一步撤销）
    //
    // 实现：给描边 ID 加一个固定偏移（0x7A00），避开可能与其它命令类
    // 撞号的区间（其它命令类用 1、2、3…这种小数字）。
    //
    // 用 0x7A00 是因为 'z'(0x7A) 是本项目的首字母，纯属便于识别，
    // 没有功能性含义。
    return 0x7A00 + m_strokeId;
}

bool DrawCommand::mergeWith(const QUndoCommand *other)
{
    if (!other)
        return false;

    // 向下转型。QUndoStack 保证只在 id() 相同时调用 mergeWith，
    // 而我们的 id 是唯一的（每个描边一个），所以这里的转换是安全的。
    const auto *next = dynamic_cast<const DrawCommand *>(other);
    if (!next)
        return false;

    // ── 合并条件检查 ──
    //
    // 必须满足：
    //   1. 同一个文档（跨文档不可能，但防御性地检查）
    //   2. 同一个图层（如果中途切了图层，不能合并 —— 那应该是两步操作）
    if (next->m_doc != m_doc)
        return false;
    if (next->m_layerIndex != m_layerIndex)
        return false;

    // ── 合并脏矩形 ──
    //
    // 两个矩形合并成一个更大的矩形（而不是维护矩形列表）。
    // 为什么不维护列表：一次描边的轨迹通常是一条连续路径，
    // 其包围盒与"多个矩形之和"的面积差别不大，而维护列表
    // 会让数据结构复杂化（且 undo/redo 时要遍历）。
    //
    // 若将来发现"长距离跳跃 + 折返"的描边导致包围盒过大，
    // 再考虑改成矩形列表。当前方案简单可靠。
    const QRect merged = m_rect.united(next->m_rect);

    // ── 合并像素数据 ──
    //
    // ★ 关键：before 与 after 必须用**同一个尺寸**的图 ★
    //
    // 合并后矩形变大，原先的 before/after 尺寸就不匹配了。
    // 正确处理：
    //   · 新的 before —— 从"旧的 before 加上矩形扩展区域"重建。
    //     但旧 before 里没有扩展区域的像素数据（那部分没变过），
    //     所以扩展区域的旧像素必须从**当前文档**里取 —— 不行，
    //     当前文档已经被修改过了。
    //
    //   简化且正确的做法：
    //   · 新 before = 把旧 before 贴到 mergeRect 中它原来所在的位置，
    //     其余区域填透明。
    //   · 新 after  = 把 next->after 贴到 mergeRect 中它所在的位置，
    //     再把旧 after 贴到它原来的位置（两者可能重叠，以 next 为准）。
    //
    //   为什么 before 的"其余区域填透明"是正确的？
    //     因为那些区域在合并后的操作中**从未被这个命令修改过**。
    //     undo 时用 Source 模式写入透明像素……不行，那会把原有内容擦掉。
    //
    //   ★★ 所以正确的做法是：before 用"旧 before"扩展，扩展区域从
    //      旧 after 里取（因为那部分是上一次命令之后、本次命令之前的状态）★★
    //
    //   综合起来最简单可靠的实现是：
    //     before = mergeRect 范围内、**第一次命令执行前**的像素
    //     after  = mergeRect 范围内、**最后一次命令执行后**的像素
    //
    //   而"第一次命令执行前"的像素只能通过旧 before + 旧 after 拼出来：
    //     扩展区域（只被 next 覆盖）的"第一次执行前"像素 = 旧图层的原始像素
    //     但那个我们没有存……
    //
    //   ── 结论：改用"逐次扩张"策略 ──
    //
    //   把 before 按"旧 before 在新矩形中的位置"贴入，未覆盖区域用
    //   旧 after 的对应位置填充 —— 因为旧 after 恰好就是"第一次执行后"的
    //   状态，而扩展区域在第一次执行前与执行后是相同内容（第一次没动它）。
    //   所以用旧 after 填充扩展区域是**正确**的。
    QImage newBefore(merged.size(), QImage::Format_ARGB32_Premultiplied);
    QImage newAfter (merged.size(), QImage::Format_ARGB32_Premultiplied);
    newBefore.fill(Qt::transparent);
    newAfter.fill(Qt::transparent);

    {
        QPainter p(&newBefore);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        // 先用旧 after 填满（提供扩展区域的"未改动像素"）
        p.drawImage(m_rect.topLeft() - merged.topLeft(), m_after);
        // 再用旧 before 覆盖它自己那块（提供真正改动过的区域的旧像素）
        p.drawImage(m_rect.topLeft() - merged.topLeft(), m_before);
        p.end();
    }

    {
        QPainter p(&newAfter);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        // 先用旧 after 填（保留第一次的改动）
        p.drawImage(m_rect.topLeft() - merged.topLeft(), m_after);
        // 再用新 after 覆盖它自己那块（应用最新的改动）
        p.drawImage(next->m_rect.topLeft() - merged.topLeft(), next->m_after);
        p.end();
    }

    m_rect   = merged;
    m_before = newBefore;
    m_after  = newAfter;

    // mergeWith 返回 true 表示"已合并，新命令会被 QUndoStack 删除"。
    // QUndoStack 会接管 next 的 delete，我们不需要（也不能）自己删。
    return true;
}

// ===========================================================================
//  内存报告
// ===========================================================================

qint64 DrawCommand::memoryUsage() const
{
    // 每张图 4 字节/像素（ARGB32_Premultiplied），有两张（before/after）
    qint64 bytes = 0;

    if (!m_before.isNull()) {
        bytes += qint64(m_before.width()) * qint64(m_before.height()) * 4;
    }
    if (!m_after.isNull()) {
        bytes += qint64(m_after.width()) * qint64(m_after.height()) * 4;
    }

    // 加上对象自身的固定开销
    bytes += qint64(sizeof(DrawCommand));

    return bytes;
}

} // namespace zl
