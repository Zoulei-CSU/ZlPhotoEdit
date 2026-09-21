#pragma once

// ============================================================================
//  Curves.h —— 曲线调整（控制点 → LUT 烘焙）
// ----------------------------------------------------------------------------
//  曲线是本工程的重点功能（用户明确表示高频使用），因此单独成文件。
//
//  ★ 核心算法选择：Fritsch–Carlson 单调三次样条 ★
//
//    最直观的做法是 Catmull-Rom 三次样条，但它有一个致命问题：**过冲**。
//    当控制点之间有陡峭的升降时，样条会在段内反向回勾，数值超出两端的
//    范围。映射到图像上就是：
//      · 暗部出现色带（原本连续的渐变色被压成几段）
//      · 高光与暗部出现反相的噪点
//      · 用户拉一条 S 形曲线，画面出现不该有的"回勾"细节
//    这是所有曲线实现都会遇到的经典坑。
//
//    Fritsch–Carlson 的做法是：在计算每个控制点处的切线时，先算出三点
//    差商的调和平均，然后检查是否会导致该区间非单调；若不满足单调条件，
//    就把切线置为 0。这样得到的样条**保证在每个区间内单调**，从而彻底
//    消除过冲。
//
//    代价是曲线在控制点处的过渡不如 Catmull-Rom 那么"圆润"，但这一点
//    肉眼几乎不可察觉，而消除过冲的收益是决定性的。
//
//  ★ LUT 烘焙 ★
//    曲线只在 256 个离散输入上有定义，直接烘焙成 LUT 再查表（见 Lut.h）。
//    烘焙一次的成本是微秒级，拖动滑块时可以实时重算。
//
//  ★ 通道叠乘 ★
//    最终映射 = curveBlue(curveGreen(curveRed(curveRgb(x))))
//    用 composeLut 串成单表，逐像素只查一次。
// ============================================================================

#include "engine/Lut.h"

#include <QList>
#include <QString>

namespace zl {

// ---------------------------------------------------------------------------
//  曲线通道
// ---------------------------------------------------------------------------
enum class CurveChannel : int {
    Rgb  = 0,   // 主通道（同时作用于 RGB）
    Red  = 1,
    Green= 2,
    Blue = 3,
    Count = 4
};

QString curveChannelName(CurveChannel ch);

// ---------------------------------------------------------------------------
//  控制点
// ---------------------------------------------------------------------------
//  x 与 y 都在 [0,1] 范围。x 表示输入亮度，y 表示输出亮度。
//
//  约定：
//    · 曲线必须至少有两个点（通常是两个端点）
//    · 必须有一个点位于 x == 0（起点）和一个点位于 x == 1（终点）
//    · 中间的点可以任意增减，x 必须严格递增
struct CurvePoint
{
    qreal x = 0.0;
    qreal y = 0.0;

    CurvePoint() = default;
    CurvePoint(qreal ix, qreal iy) : x(ix), y(iy) {}
};

// ---------------------------------------------------------------------------
//  单条曲线
// ---------------------------------------------------------------------------
class Curve
{
public:
    // 默认构造 = 恒等曲线（两个端点，y == x）。
    // 这是"未做任何调整"的状态，所有对话框都从这里开始。
    Curve();

    // 从控制点构造。点列表会先做规范化（排序、去重、补端点）。
    explicit Curve(const QList<CurvePoint> &points);

    // ── 控制点操作 ──
    const QList<CurvePoint> &points() const { return m_points; }
    void setPoints(const QList<CurvePoint> &points);

    // 添加一个点。返回新点在列表中的索引。
    // 若已有同 x 的点，则替换其 y 值而不是添加新点 —— 因为曲线必须满足
    // "单值函数"，同一个 x 出现两个 y 是无意义的。
    int addPoint(const CurvePoint &pt);

    // 移动第 index 个点。内部会做约束：
    //   · 端点只能上下移动，x 固定为 0 或 1
    //   · 中间点的 x 被限制在左右邻点之间（不允许越过邻居）
    // 返回是否发生了实际改变（用于决定要不要重绘）。
    bool movePoint(int index, const CurvePoint &pt);

    // 删除一个点。端点不可删除。返回是否成功。
    bool removePoint(int index);

    // 找到离指定位置最近的点，用于鼠标拖拽时的"吸附"。
    // 返回 -1 表示没有在阈值内的点。
    int findNearestPoint(const CurvePoint &pos, qreal maxDistance) const;

    // 是否为恒等曲线（未做调整）。
    // 用于决定"确定"按钮是否要真的生成一条 Command —— 用户打开曲线对话框
    // 什么都没动就点确定，不应该在历史记录里留下一条记录。
    bool isIdentity() const;

    // 重置为恒等曲线
    void reset();

    // ── 烘焙 ──
    // 按 Fritsch–Carlson 单调三次样条插值，生成 256 项 LUT。
    //
    // smooth 为 false 时退化为线性插值（直线的折线段）。
    // 提供这个选项是因为有些用户就想要"硬折线"的效果（例如做色阶分离），
    // 而且线性插值也是验证样条实现是否正确的参照 —— 两者的端点必然一致。
    CurveLut bake(bool smooth = true) const;

private:
    // 规范化：排序、合并同 x 的点、确保端点存在
    void normalize();

    QList<CurvePoint> m_points;
};

// ---------------------------------------------------------------------------
//  完整曲线设置（四条曲线）
// ---------------------------------------------------------------------------
class CurvesSettings
{
public:
    CurvesSettings();

    const Curve &curve(CurveChannel ch) const;
    Curve       &curveRef(CurveChannel ch);
    void setCurve(CurveChannel ch, const Curve &curve);

    // 全部通道是否都是恒等曲线
    bool isIdentity() const;

    void reset();

    // ── 烘焙成可应用的 LUT ──
    //
    // 返回三通道各自合成后的 LUT，可直接交给 applyLuts()。
    //
    // 合成顺序（与 Photoshop 一致）：
    //   Red   最终表 = compose(rgb主曲线, red通道曲线)
    //   Green 最终表 = compose(rgb主曲线, green通道曲线)
    //   Blue  最终表 = compose(rgb主曲线, blue通道曲线)
    ChannelLuts bakeLuts() const;

private:
    Curve m_curves[int(CurveChannel::Count)];
};

// ---------------------------------------------------------------------------
//  预设
// ---------------------------------------------------------------------------
//  提供一组常用曲线的预设，让用户一键套用再微调。这比从零拉曲线友好得多。
struct CurvePreset
{
    QString name;       // 显示名
    QList<CurvePoint> points;
};

// 返回全部内置预设
QList<CurvePreset> builtinPresets();

// ---------------------------------------------------------------------------
//  便捷入口：直接对图像应用曲线
// ---------------------------------------------------------------------------
QImage applyCurves(const QImage &src, const CurvesSettings &settings);

} // namespace zl
