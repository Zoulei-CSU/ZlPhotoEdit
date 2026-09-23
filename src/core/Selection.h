#pragma once

// ============================================================================
//  Selection.h —— 选区
// ----------------------------------------------------------------------------
//  核心决策：选区用【8 位灰度蒙版】表示，而不是 QRegion。
//
//  为什么不用 QRegion？
//    QRegion 本质是"多边形的整数集合"，只能表达硬边缘的"在内/在外"。
//    而实际需要的选区能力包括：
//      · 羽化（边缘灰阶过渡）
//      · 魔棒 / 颜色范围（边缘本就不规则且带抗锯齿）
//      · 椭圆与任意形状的抗锯齿边缘
//      · 半透明的"部分选中"状态
//    这些 QRegion 都表达不了。灰度蒙版一次解决全部问题：
//      0   = 完全未选中（操作不影响）
//      255 = 完全选中
//      1~254 = 部分选中（按比例混合，用于羽化与抗锯齿）
//
//  性能考量：
//    灰度蒙版是每个像素 1 字节。4000×3000 的图 = 12MB，可接受。
//    为了效率，内部缓存一个"是否全选"标志：全选时几乎所有操作都可以
//    直接跳过蒙版运算，这是最常见的场景（用户没做选区），必须快。
// ============================================================================

#include <QImage>
#include <QRect>
#include <QPainterPath>

namespace zl {

class Selection
{
public:
    Selection() = default;

    // ── 状态查询 ──

    // 是否存在"非全选"的选区。
    // 这是最高频的查询 —— 工具在写入像素前都会调它，若返回 false 则可跳过
    // 全部蒙版运算，直接操作整张图。绝大多数编辑场景下它都是 false。
    bool isActive() const { return m_active; }

    // 当 isActive() 为 true 时，返回蒙版的引用；否则返回空 QImage。
    // 蒙版尺寸始终与画布尺寸一致。
    const QImage &mask() const { return m_mask; }

    // 选区的包围盒（蒙版中非零区域的边界）。用于优化遍历范围 ——
    // 只处理包围盒内的像素，而不是整张图。
    QRect bounds() const;

    // ── 修订号：选区**内容**变没变 ──
    //
    // ★ 为什么下游需要它 ★
    //   CanvasView 缓存着"选区轮廓"（把蒙版扫成一条边界线段路径，
    //   全选时扫的是整张画布，几十毫秒）。原先是"凡是像素脏了就让轮廓
    //   失效"，于是**每个画笔事件**都要重扫一遍 —— 而画笔改的是图层
    //   像素，与选区蒙版毫无关系（评估文档 §1.3）。
    //   有了修订号，缓存的有效性判据才能精确到"选区真的变了没有"。
    //
    // ★ 语义只有一条，但必须严格成立 ★
    //   修订号相同  ⇒  选区内容相同。
    //   逆命题不要求（同一次内容可以有不同的号，只是会多重建一次）。
    //   为了让这条成立，发号器是**全局单调递增**的，
    //   否则"撤销把旧快照赋回来"会让号回到一个旧值，
    //   而那个旧值对应的内容与被覆盖的内容往往不同（见 Selection.cpp）。
    //
    // ★ 只有**真的改了**才能递增 ★
    //   提前返回的分支（translate(0,0) / inactive 时 invert() / feather(0)…）
    //   一律不许递增 —— 否则"号没变就跳过"的优化会静默失效，
    //   表现是"功能全对，但那个优化根本不生效"。
    quint64 revision() const { return m_revision; }

    // 是否处于"全选 / 没有选区限制"的状态。
    //
    // ★ 语义修正记录（不要改回 m_active && m_allSelected）★
    //
    //   这两个标志的组合只有两种合法取值：
    //     (m_active=false, m_allSelected=true)  —— 全选（clear() 之后的状态）
    //     (m_active=true,  m_allSelected=false) —— 有蒙版选区
    //   "同时为 true"这一格永远不会出现。
    //
    //   所以原来写成 `m_active && m_allSelected` 会把结果恒定为 false ——
    //   即"明明处于全选，却被告知不是全选"。这个错误不会立刻炸，
    //   但任何按字面意思使用它的人（if (sel.isAllSelected()) …）
    //   都会得到与预期完全相反的分支。直接返回 m_allSelected 才是
    //   与名字相符、也等价于 !m_active 的真实状态。
    bool isAllSelected() const { return m_allSelected; }

    // ── 修改选区 ──

    // 清空选区（变为"全选"状态，即没有选区限制）
    void clear();

    // 全选：等价于 clear()，保留这个命名是为了调用处语义清晰
    void selectAll() { clear(); }

    // ── ★ 尺寸准备：第一次拉选区之前必须先调用它 ★ ──
    //
    // 【这个函数解决的是一个非常隐蔽的坑】
    //
    // setFromPath() / applyPath() 内部需要一份"尺寸参考"蒙版才能工作 ——
    // 它们要按画布尺寸分配目标位图，而从 QPainterPath 本身推不出画布大小。
    // 若当前 m_mask 为空（新文档从未做过选区时的状态），这两个函数会
    // **静默地什么都不做并直接 return**。
    //
    // 表现症状：用户在刚打开的图片上拖动矩形选框，松手后**毫无反应**，
    // 状态栏没有报错，日志里也没有任何异常 —— 因为代码确实"正常"执行了，
    // 只是走到了那个提前返回的分支。这是最难排查的一类问题。
    //
    // 所以：任何"从路径建立选区"的调用方，必须在第一次操作前调用本函数。
    // 选区工具在 mousePress 时调用它，魔棒、套索将来也一样。
    //
    // 【为什么底色填 255（全选）而不是 0】
    //   Add / Subtract / Intersect 三种运算要与"原本的状态"做布尔运算。
    //   新文档在语义上处于"全选"状态（所有操作可作用于整张图），
    //   用 255 打底才能得到正确结果：
    //     · 全选里减去一个矩形  → 挖了个洞（正确）
    //     · 全选与矩形求交集    → 就是那个矩形（正确）
    //   若填 0，上述两种运算都会得到"什么都没有"。
    //
    // 【为什么不动 m_active】
    //   全 255 的蒙版在语义上**等价于**"没有选区限制"。保持 m_active == false
    //   才能让 isActive() 继续返回 false，使绝大多数工具走
    //   "跳过全部蒙版运算"的快路径 —— 这是性能上很重要的一点。
    void prepareCanvas(const QSize &size);

    // 从 QPainterPath 设置选区（矩形、椭圆、套索等都走这里）。
    // antialiased 为 true 时边缘会生成 1~254 的过渡值，避免锯齿。
    void setFromPath(const QPainterPath &path, bool antialiased = true);

    // 从已有的灰度蒙版直接设置（魔棒、从剪贴板恢复等场景）
    void setMask(const QImage &grayMask);

    // 直接拿到可写蒙版。调用方负责维持格式与尺寸正确。
    // 工具在构建选区时用它逐像素写入，避免中间多次拷贝。
    QImage &mutableMask() { return m_mask; }

    // 把选区标记为"已修改"。工具直接操作 mutableMask() 后必须调用它，
    // 否则 isActive() 等状态标志不会更新。
    void markModified();

    // ── 选区运算 ──
    // 四种布尔运算，对应工具选项条上的"新建/添加/减去/交集"四个按钮。
    enum class Op {
        Replace = 0,    // 新建
        Add     = 1,    // 添加（并集）
        Subtract= 2,    // 减去（差集）
        Intersect = 3   // 交集
    };

    // 用一条路径按指定运算方式更新选区
    void applyPath(const QPainterPath &path, Op op, bool antialiased = true);

    // 用另一份灰度蒙版按指定运算方式更新选区
    void applyMask(const QImage &otherMask, Op op);

    // ── 羽化 ──
    // radius 为羽化半径（像素）。实现为对蒙版做高斯模糊。
    // 注意：羽化是对当前蒙版的**就地**处理，应作为选区操作的收尾步骤，
    // 而不是每改一次就羽化一次（会累积模糊）。
    void feather(qreal radius);

    // ── 便捷操作 ──
    void selectRect(const QRect &rect, Op op = Op::Replace, bool antialiased = false);
    void selectEllipse(const QRect &ellipseRect, Op op = Op::Replace, bool antialiased = true);

    // 原地平移整个蒙版（移动选区用）
    void translate(const QPoint &delta);

    // 反选
    void invert();

    // 深拷贝（撤销快照 / 剪贴板）
    Selection clone() const;

    // ── 尺寸同步 ──
    // 画布尺寸变化时调用。策略见实现文件中的说明。
    void resizeCanvas(const QSize &newSize, const QPoint &offset = QPoint());

private:
    // 确保蒙版已按指定尺寸分配，并返回是否需要重建
    void ensureMask(const QSize &size);

    // 内容变更的唯一记账点：置脏包围盒缓存 + 取一个新修订号。
    // ★ 所有改到 m_mask / m_active / m_allSelected 的地方都必须走它 ★
    void touch();

    QImage m_mask;                  // Format_Grayscale8，尺寸与画布一致
    bool   m_active      = false;   // false 表示"全选"（无选区限制）
    bool   m_allSelected = true;    // 与 m_active 配合表达状态
    quint64 m_revision   = 0;       // 内容修订号（见 revision()）
    // 包围盒缓存。选区频繁被查询 bounds()（每个工具在写入前都要问一次），
    // 每次遍历整个蒙版太浪费，所以缓存起来，markModified() 时失效。
    mutable QRect m_cachedBounds;
    mutable bool  m_boundsDirty = true;
};

} // namespace zl
