#pragma once

// ============================================================================
//  LayerList.h —— 图层栈容器
// ----------------------------------------------------------------------------
//  职责：维护图层的有序集合，并提供插入、删除、移动、查找等操作。
//        不含任何图像运算 —— 合成逻辑在 Document::composite() 中。
//
//  索引约定（务必统一，否则"图层顺序反了"这类 bug 会反复出现）：
//    索引 0        = 图层栈的**最底部**（最先绘制，被其他图层覆盖）
//    索引 size()-1 = 图层栈的**最顶部**（最后绘制，视觉上在最前面）
//    图层面板从上往下显示时，需要把列表**倒序**展示。
//  这个约定必须写进注释，因为不同人做面板时的直觉往往是相反的。
// ============================================================================

#include "core/Layer.h"

#include <QList>
#include <QSize>

namespace zl {

class LayerList
{
public:
    LayerList() = default;

    // ── 查询 ──
    int    count() const { return m_layers.size(); }
    bool   isEmpty() const { return m_layers.isEmpty(); }
    QSize  bounds() const;          // 所有图层尺寸的并集（用于确定画布尺寸）

    bool   isValidIndex(int index) const
    {
        return index >= 0 && index < m_layers.size();
    }

    // 以下访问器都做了越界检查。为了让调用方少写判空，
    // 越界或为空时返回一个函数内的静态空对象 —— 这样不会崩溃，
    // 但调用方拿到的是"看起来正常其实没数据"的对象，因此仍需自己判断有效性。
    Layer       *layer(int index);
    const Layer *layer(int index) const;

    Layer       *topLayer();        // 最顶层（索引最大）
    const Layer *topLayer() const;

    // ── 修改 ──
    // index 为插入位置（0 表示插到最底）。传 -1 或越界则追加到顶部。
    // 返回新图层的索引，便于调用方立即选中它。
    int  insert(int index, const Layer &layer);
    int  append(const Layer &layer) { return insert(m_layers.size(), layer); }

    // 在指定索引下方插入（图层面板的"新建图层"用：新图层出现在当前图层下方，
    // 这是 Photoshop 的行为，用户更习惯）。
    int  insertBelow(int index, const Layer &layer);

    bool remove(int index);

    // 移动图层位置。from 与 to 都是索引，语义为"把 from 处的图层移到 to 处"。
    // 内部会正确处理 from < to 时的索引偏移问题 —— 这是最容易写错的地方，
    // 所以只在这一处实现，其他地方不要自己算索引。
    bool move(int from, int to);

    // 把图层上移 / 下移一位。返回值表示是否真的移动了（到顶/到底时返回 false，
    // 调用方据此禁用菜单项）。
    bool moveUp(int index);
    bool moveDown(int index);

    // ── 批量操作 ──
    void clear() { m_layers.clear(); }

    // 所有 layer 的深拷贝，用于撤销快照
    QList<Layer> cloneAll() const;

    // 只读访问，用于遍历（合成、导出等）
    const QList<Layer> &all() const { return m_layers; }
    QList<Layer>       &mutableAll() { return m_layers; }

private:
    // 索引 0 = 最底部，索引 size()-1 = 最顶部。见文件头说明。
    QList<Layer> m_layers;
};

} // namespace zl
