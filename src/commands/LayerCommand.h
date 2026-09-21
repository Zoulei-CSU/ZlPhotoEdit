#pragma once

// ============================================================================
//  LayerCommand.h —— 图层结构变更的撤销命令
// ----------------------------------------------------------------------------
//  涵盖：新增图层、删除图层、复制图层、移动图层（顺序）、重命名、
//        修改不透明度 / 混合模式 / 可见性 / 锁定状态、合并图层。
//
//  设计：用一个类覆盖全部操作，而不是十几个小类。
//
//  理由：这些操作的"数据"高度相似 —— 无非是"图层列表的某个片段
//  从 A 变成 B"，或者"某个图层的某个属性从 x 变成 y"。用十几个类
//  会产生大量重复的样板代码，而收益只有"类名更具体"这一点。
//
//  做法：
//    · 结构变更（增删移）—— 存图层数据的副本 + 索引
//    · 属性变更 —— 存属性值（用 QVariant 或具体类型）
//
//  为什么不像 ImageCommand 那样压缩存储：
//    图层结构操作**不复制像素数据**。Layer 的拷贝通过 QImage 的
//    隐式共享完成（QImage 赋值是浅拷贝，只有写时才深拷贝）。
//    也就是说"存一份图层副本"的实际成本只有几十字节的元数据，
//    像素内存被共享。因此这里不需要压缩。
//
//    ★ 但要注意 ★
//    这意味着撤销栈里的图层副本与当前图层**共享像素内存**。
//    如果将来某个操作"就地修改"了共享的 QImage（例如 drawLine 直接
//    在 QImage 上画），Qt 的写时复制会触发，从而自动分离 —— 这是安全的。
//    但如果代码用 bits() 直接写而不走 QPainter，就会绕过写时复制，
//    破坏共享数据的另一方。所有像素写入都必须走 QImage API，
//    不能直接操作 bits() —— 这是全工程的硬性约定。
// ============================================================================

#include "commands/CommandStack.h"
#include "core/Layer.h"

#include <QUndoCommand>
#include <QList>
// ★ QVariant 必须完整定义 ★
//   本类有 QVariant 类型的成员变量（m_propertyBefore/m_propertyAfter），
//   成员变量不能用前置声明 —— 编译器需要知道它的大小。
//   缺少这个 include 的报错是 "使用未定义的 class QVariant"。
#include <QVariant>

namespace zl {

class Document;

class LayerCommand : public QUndoCommand, public IMemoryAware
{
public:
    // ── 操作类型 ──
    enum class Op {
        Add,            // 新增图层
        Remove,         // 删除图层
        Duplicate,      // 复制图层
        Move,           // 调整顺序
        Rename,         // 重命名
        SetOpacity,     // 改不透明度
        SetBlendMode,   // 改混合模式
        SetVisible,     // 改可见性
        SetLocked,      // 改锁定状态
        MergeDown,      // 向下合并
        MergeVisible,   // 合并可见图层
        Flatten         // 拼合图像
    };

    // ── 构造：结构类操作（增删移/合并）──
    //
    // 存"操作前的完整图层列表"与"操作后的完整图层列表"。
    //
    // 为什么存完整列表而不是只存差异：完整列表的实现简单可靠，
    // 而内存代价很低（像素共享）。存差异需要为每种操作写一套
    // 正向/反向的逻辑，容易出错。这是"用一点点内存换正确性"的交换，
    // 在这里非常划算。
    LayerCommand(Document *doc, Op op,
                 const QList<Layer> &before,
                 const QList<Layer> &after,
                 int activeBefore, int activeAfter,
                 const QString &text);

    // ── 构造：属性类操作 ──
    //
    // 只存一个图层的某个属性值。
    // 用模板构造函数支持任意属性类型，避免写十几个重载。
    template <typename T>
    LayerCommand(Document *doc, int layerIndex,
                 Op op, const T &before, const T &after,
                 const QString &text);

    // 由于模板构造函数需要在头文件里实现（否则链接会失败），
    // 这里提供一个非模板的辅助创建函数。实际使用时调用这个函数，
    // 它内部用 QVariant 承载属性值。
    static LayerCommand *createPropertyCommand(Document *doc, int layerIndex,
                                               Op op,
                                               const QVariant &before,
                                               const QVariant &after,
                                               const QString &text);

    void undo() override;
    void redo() override;

    qint64 memoryUsage() const override;

    Op op() const { return m_op; }

private:
    // 把整个图层列表替换为指定内容
    void applyLayerList(const QList<Layer> &layers, int activeIndex);

    // 应用一个属性值
    void applyProperty(const QVariant &value);

    Document *m_doc = nullptr;
    Op        m_op;

    // ── 结构类操作的数据 ──
    QList<Layer> m_beforeLayers;
    QList<Layer> m_afterLayers;
    int          m_activeBefore = 0;
    int          m_activeAfter  = 0;

    // ── 属性类操作的数据 ──
    int      m_propertyLayerIndex = -1;
    QVariant m_propertyBefore;
    QVariant m_propertyAfter;

    // 标记当前用的是哪套数据。构造时根据 op 决定。
    bool m_isStructureOp = false;
};

} // namespace zl
