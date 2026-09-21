#include "commands/LayerCommand.h"
#include "core/Document.h"

namespace zl {

// ===========================================================================
//  构造：结构类操作
// ===========================================================================

LayerCommand::LayerCommand(Document *doc, Op op,
                           const QList<Layer> &before,
                           const QList<Layer> &after,
                           int activeBefore, int activeAfter,
                           const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_op(op)
    , m_beforeLayers(before)
    , m_afterLayers(after)
    , m_activeBefore(activeBefore)
    , m_activeAfter(activeAfter)
    , m_isStructureOp(true)
{
    // 说明：m_beforeLayers / m_afterLayers 里的 Layer 对象都只做浅拷贝
    // （QImage 的隐式共享），所以这里的"存完整列表"成本很低 ——
    // 只是几十个 Layer 对象的元数据（名称、不透明度、混合模式等），
    // 像素数据仍被共享，直到某一方修改时才真正分离。
    //
    // 这正是"像素数据只有一份"这条架构铁律在内存层面的体现。
}

// ===========================================================================
//  构造：属性类操作
// ===========================================================================

// 由于头文件里已经声明了模板构造，但实际使用统一走
// createPropertyCommand()，这里不再单独定义模板实现。
// 保留模板声明是为了文档性 —— 它说明了"属性命令的本质是 (before, after)"。

LayerCommand *LayerCommand::createPropertyCommand(Document *doc, int layerIndex,
                                                  Op op,
                                                  const QVariant &before,
                                                  const QVariant &after,
                                                  const QString &text)
{
    // 先用结构类构造函数建一个空的对象，再填属性数据。
    // 这样避免再写一个几乎相同的构造函数。
    auto *cmd = new LayerCommand(doc, op, {}, {}, layerIndex, layerIndex, text);

    cmd->m_isStructureOp      = false;
    cmd->m_propertyLayerIndex = layerIndex;
    cmd->m_propertyBefore     = before;
    cmd->m_propertyAfter      = after;

    return cmd;
}

// ===========================================================================
//  undo / redo
// ===========================================================================

void LayerCommand::redo()
{
    if (!m_doc)
        return;

    if (m_isStructureOp) {
        applyLayerList(m_afterLayers, m_activeAfter);
    } else {
        applyProperty(m_propertyAfter);
    }
}

void LayerCommand::undo()
{
    if (!m_doc)
        return;

    if (m_isStructureOp) {
        applyLayerList(m_beforeLayers, m_activeBefore);
    } else {
        applyProperty(m_propertyBefore);
    }
}

void LayerCommand::applyLayerList(const QList<Layer> &layers, int activeIndex)
{
    if (!m_doc)
        return;

    // ── 整表替换 ──
    //
    // 看起来很暴力，但这是最可靠的做法：不管操作是新增、删除还是移动，
    // "把列表换成目标状态"总是正确的，不需要为每种操作写一套逻辑。
    //
    // 性能：Layer 的拷贝是浅拷贝，几十个图层也就几百字节的元数据，
    // 加上列表的重新分配，总开销在微秒级。相比一个滤镜操作要几百毫秒，
    // 这点开销完全可以忽略。
    m_doc->layers().mutableAll() = layers;

    // 修正活动图层索引，防止越界。
    // 删除图层时如果活动索引指向被删的那个，这里会把它拉回合法范围。
    const int maxIndex = qMax(0, m_doc->layers().count() - 1);
    m_doc->setActiveLayerIndex(qBound(0, activeIndex, maxIndex));

    // ★ 必须刷新 ★
    //
    // 这行不能省。整表替换后：
    //   · 合成缓存失效（否则画面还是旧内容）
    //   · 内容包围盒缓存失效
    //   · 图层面板需要重建列表（走 structureChanged 信号）
    m_doc->markDirty();
    emit m_doc->structureChanged();
}

void LayerCommand::applyProperty(const QVariant &value)
{
    if (!m_doc)
        return;

    Layer *layer = m_doc->layers().layer(m_propertyLayerIndex);
    if (!layer)
        return;

    // 按操作类型分派。这里用 QVariant 承载值，取出来时按类型还原。
    // 好处是一个函数处理所有属性；代价是一次 QVariant 的装箱拆箱，
    // 对"修改一个属性"这种低频操作完全无所谓。
    switch (m_op) {
    case Op::Rename:
        layer->setName(value.toString());
        break;

    case Op::SetOpacity:
        // 不透明度用 int 存储（0~255），避免 QVariant 对 quint8 的支持
        // 在某些 Qt 版本上不一致。
        layer->setOpacity(quint8(qBound(0, value.toInt(), 255)));
        break;

    case Op::SetBlendMode:
        layer->setBlendMode(static_cast<BlendMode>(value.toInt()));
        break;

    case Op::SetVisible:
        layer->setVisible(value.toBool());
        break;

    case Op::SetLocked:
        layer->setLocked(value.toBool());
        break;

    default:
        // 结构类操作走到这里说明用错了，静默忽略而不是崩溃
        return;
    }

    // 属性变化会影响合成结果：
    //   · 不透明度 / 混合模式 / 可见性 —— 直接影响合成
    //   · 名称 / 锁定 —— 不影响像素，但图层面板需要刷新
    m_doc->markDirty();

    // 名称与锁定状态的变化不影响合成，但面板要更新显示。
    // 统一发 structureChanged 虽然有点重（面板会重建列表而不是只更新一项），
    // 但图层面板的列表项数通常不超过十几个，重建的开销可以忽略，
    // 换来的是"不会漏更新"的可靠性。
    emit m_doc->structureChanged();
}

// ===========================================================================
//  内存报告
// ===========================================================================

qint64 LayerCommand::memoryUsage() const
{
    qint64 bytes = qint64(sizeof(LayerCommand));

    if (m_isStructureOp) {
        // ★ 这里刻意**不**计入像素数据 ★
        //
        // 因为 Layer 之间共享 QImage 的像素缓冲区，像素内存只存在于
        // Document 的图层里，撤销栈中的副本并不额外占用。
        // 计入的话会严重高估（每份看起来都是 48MB × 图层数），
        // 让状态栏显示一个荒谬的数字。
        //
        // 只计算 Layer 对象的元数据开销（名称字符串、属性字段等）。
        // 每个 Layer 对象的固有大小（含 QString、QImage 头部）约 100 字节。
        const int layerCount = m_beforeLayers.size() + m_afterLayers.size();
        bytes += qint64(layerCount) * 100;
    } else {
        // 属性命令：只有几个 QVariant 的开销，忽略不计
        bytes += 64;
    }

    return bytes;
}

} // namespace zl
