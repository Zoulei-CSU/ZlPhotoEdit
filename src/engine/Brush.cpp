#include "engine/Brush.h"

#include <QtGlobal>
#include <QtMath>

#include <cmath>

namespace zl {

// ===========================================================================
//  笔尖生成
// ===========================================================================
//
//  衰减曲线的选择：
//
//    归一化半径 r ∈ [0, 1]（0 = 笔尖中心，1 = 理论边缘），
//    硬度 h ∈ [0, 1]，过渡带起点为 h。
//
//      r ≤ h          → 覆盖度 1（完全不透明的"实心"部分）
//      r > h          → 在 [h, 1] 区间内从 1 衰减到 0
//
//    衰减形状用 smoothstep  t²(3-2t) 而不是线性 t。
//    理由：线性衰减在圆心附近的斜率是突变的（从 0 直接跳到一个固定斜率），
//    画出来能看出一个"台阶"，尤其在低硬度大笔刷时明显。
//    smoothstep 两端导数为 0，过渡自然，更接近真实软笔刷的形态。
// ===========================================================================
BrushTip makeBrushTip(int diameter, int hardness)
{
    BrushTip tip;

    // 钳制参数。直径上限 2000 是本工程可接受的极端值
    // （再大的笔刷在 4000×3000 的画布上也只够落两笔）。
    diameter = qBound(1, diameter, 2000);
    hardness = qBound(0, hardness, 100);

    tip.diameter = diameter;
    tip.radius   = diameter / 2;

    // ── 退化情形：直径 1~2 像素 ──
    // 这个尺寸下"中心"与"边缘"最多只差 1 个像素，衰减公式没有发挥空间，
    // 而且 (diameter - 1) / 2 会算出 0 或 0.5 这种让半径归一化失真的值。
    // 直接给一个实心点，这也是像素画场景下用户期望的行为。
    if (diameter <= 2) {
        tip.mask = QImage(diameter, diameter, QImage::Format_Grayscale8);
        tip.mask.fill(255);
        tip.falloffEdge = 1.0;
        return tip;
    }

    tip.mask = QImage(diameter, diameter, QImage::Format_Grayscale8);
    if (tip.mask.isNull())
        return BrushTip();     // 内存不足，返回空笔尖（调用方需检查）

    // ★ 用像素中心而不是整数直径中点 ★
    //   直径 4 时，(diameter - 1) / 2 = 1.5 —— 笔尖对称地落在
    //   像素 1 与像素 2 之间的缝上，上下左右四个像素等距。
    //   若用 diameter / 2 = 2，则笔尖偏向右下，画出的圆点不对称。
    const qreal center = (diameter - 1) / 2.0;
    const qreal radius = diameter / 2.0;

    const qreal h = hardness / 100.0;

    // ── 过渡带宽度（归一化到半径）──
    //
    //  硬度 100% → 过渡带 0   → 理论上纯硬边
    //  硬度   0% → 过渡带 1   → 从中心一路衰减到边缘
    //
    //  ★ 下限 1.0 / radius 是这个函数的关键 ★
    //    它保证过渡带至少有 1 个像素宽。没有这个下限时，
    //    硬度 100% 的笔刷边缘会是"非 0 即 255"的硬跳变，
    //    在像素网格上呈现为明显的阶梯锯齿。
    //    加上之后，最硬的笔刷也自带 1 像素抗锯齿。
    const qreal edge = qMax(1.0 - h, 1.0 / radius);
    tip.falloffEdge = edge;

    for (int y = 0; y < diameter; ++y) {
        uchar *line = tip.mask.scanLine(y);

        for (int x = 0; x < diameter; ++x) {
            const qreal dx = x - center;
            const qreal dy = y - center;

            // sqrt 在这里的开销可以忽略：笔尖只在参数变化时生成一次
            // （按直径缓存），不是每个像素都算一遍。
            const qreal r = std::sqrt(dx * dx + dy * dy) / radius;

            // 映射到 [0, 1]：r = 1-edge 时为 1，r = 1 时为 0
            qreal a = (1.0 - r) / edge;
            a = qBound(0.0, a, 1.0);

            // smoothstep 平滑过渡
            a = a * a * (3.0 - 2.0 * a);

            line[x] = uchar(qRound(a * 255.0));
        }
    }

    return tip;
}

// ===========================================================================
//  笔画补点
// ===========================================================================
QVector<QPointF> interpolateStamps(const QPointF &from, const QPointF &to,
                                   qreal spacing)
{
    QVector<QPointF> out;

    // ★ 钳制下限 0.5 ★
    //   调用方若传 0（例如"间距 = 直径 × 0%" 且直径算出来很小），
    //   下面 dist / spacing 会变成 inf，ceil 之后转 int 是未定义行为。
    spacing = qMax(0.5, spacing);

    const qreal dx = to.x() - from.x();
    const qreal dy = to.y() - from.y();
    const qreal dist = std::sqrt(dx * dx + dy * dy);

    // 原地落笔（鼠标没移动）。此时也必须落一笔 ——
    // 用户单击一下应该留下一个点，而不是什么都不发生。
    if (dist < 0.0001) {
        out.append(to);
        return out;
    }

    // 步数取 ceil(dist / spacing)，于是实际间距 = dist / steps ≤ spacing。
    // 用 ceil 而不是 floor 是为了保证"只密不疏"：宁可多落几笔，
    // 也不能出现间距大于 spacing 的空隙（那会出现算盘珠断点）。
    int steps = int(std::ceil(dist / spacing));
    steps = qMax(1, steps);

    out.reserve(steps);

    // ★ 从 i = 1 开始，不含 from ★
    //   from 是上一段的终点，已经在上一段落过笔。重复落笔不会让颜色
    //   变深（合成是取最大），但会白白多算一次笔尖贴图。
    for (int i = 1; i <= steps; ++i) {
        const qreal t = qreal(i) / qreal(steps);
        out.append(QPointF(from.x() + dx * t, from.y() + dy * t));
    }

    return out;
}

// ===========================================================================
//  压入落笔点
// ===========================================================================
void stampTipInPlace(QImage &strokeMask, const BrushTip &tip,
                     const QPointF &center, qreal strength)
{
    if (tip.mask.isNull() || strokeMask.isNull())
        return;

    // 累积蒙版必须是 Grayscale8。用别的格式会静默写出错误结果
    // （例如误传 ARGB32 时 scanLine 的步长与灰度蒙版不同），
    // 所以这里直接拒绝而不是"尽力而为"。
    if (strokeMask.format() != QImage::Format_Grayscale8)
        return;

    strength = qBound(0.0, strength, 1.0);
    if (strength <= 0.0)
        return;

    const int mw = strokeMask.width();
    const int mh = strokeMask.height();
    const int d  = tip.diameter;
    const int r  = tip.radius;

    // 笔尖左上角在蒙版坐标系中的位置。
    //
    // ★ 用 floor 而不是 qRound ★
    //   center 是浮点落笔位置。位移量应该是"笔尖矩形按实际浮点位置摆放"，
    //   用 floor 得到的偏差 ∈ [0, 1) 且恒为同向；用四舍五入则偏差 ∈ (-0.5, 0.5]
    //   且随位置来回跳，在缓慢拖动时表现为整条线的轻微抖动。
    //   画笔的亚像素精度就体现在这里 —— 累积蒙版本身是整数网格，
    //   落笔位置取整的抖动会被直接看到。
    const int ox = int(std::floor(center.x())) - r;
    const int oy = int(std::floor(center.y())) - r;

    // ── 与蒙版求交 ──
    // 笔尖有一半在画布外时，只画可见的那部分。
    // 这是画笔"可以贴着边缘画"的关键 —— 不允许越界写会直接段错误。
    const int x0 = qMax(0, ox);
    const int y0 = qMax(0, oy);
    const int x1 = qMin(mw, ox + d);
    const int y1 = qMin(mh, oy + d);

    if (x0 >= x1 || y0 >= y1)
        return;     // 完全在画布外

    // strength 预先算成 0..255 的整数系数，避免在内层循环里反复做浮点乘。
    // +0.5 是为了四舍五入（截断会让弱流量下整片区域变成 0，
    // 表现为"流量调到 30% 就画不出东西"）。
    const int strength255 = int(strength * 255.0 + 0.5);

    for (int y = y0; y < y1; ++y) {
        const uchar *tipLine = tip.mask.constScanLine(y - oy);
        uchar *dstLine = strokeMask.scanLine(y);

        for (int x = x0; x < x1; ++x) {
            const int tipVal = tipLine[x - ox];

            // ── 核心：取最大，而不是累加 ──
            // 同一笔内重叠的区域，覆盖度只取决于"笔尖在该点的最大值"，
            // 与落笔次数无关。这正是让一条线颜色均匀的原因。
            const int v = tipVal * strength255 / 255;

            if (v > dstLine[x])
                dstLine[x] = uchar(v);
        }
    }
}

QRect stampBoundingRect(const BrushTip &tip, const QPointF &center)
{
    const int r = tip.radius;
    const int ox = int(std::floor(center.x())) - r;
    const int oy = int(std::floor(center.y())) - r;

    // 上下左右各多留 1 像素，容纳抗锯齿过渡带与落笔位置取整的偏差。
    // 宁可脏矩形略大（多算几行像素），也不能略小 ——
    // 略小的后果是线条边缘缺一块，而且撤销时无法完全还原。
    return QRect(ox - 1, oy - 1, tip.diameter + 2, tip.diameter + 2);
}

// ===========================================================================
//  合成到图层
// ===========================================================================
QImage composeStroke(const QImage &base, const QImage &strokeMask,
                     const QColor &color, bool erase,
                     const QImage &selectionMask)
{
    if (base.isNull() || strokeMask.isNull())
        return base;

    // ── 准备可写的预乘位图 ──
    //
    // ★ 这里的 copy() 是必须的，不是多余的开销 ★
    //
    //   QImage 是隐式共享的。`QImage work = base;` 只是加了一次引用计数，
    //   此时 work 与 base 指向**同一块**像素数据。随后我们对 work 写入，
    //   Qt 会在第一次写时自动 detach（深拷贝）——
    //   但那个 detach 只保证"写 work 不会污染 base"，
    //   它发生在 scanLine() 调用时，而我们在同一个循环里既读又写同一行。
    //
    //   更麻烦的是格式转换：convertToFormat 一定会返回新的独立位图，
    //   所以"格式不对"的路径天然安全，反而是"格式已经正确"的路径
    //   容易忘记 copy，造成"画笔把原始快照也改了"这类难查的 bug。
    //   统一在这里 copy 一次，两条路径行为一致。
    QImage work;
    if (base.format() == QImage::Format_ARGB32_Premultiplied)
        work = base.copy();
    else
        work = base.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if (work.isNull())
        return base;

    // 只处理两者的公共区域。理论上尺寸相同（都由画布尺寸生成），
    // 但取 min 可以避免任何一方的尺寸异常导致越界读。
    const int w = qMin(work.width(), strokeMask.width());
    const int h = qMin(work.height(), strokeMask.height());

    // ── 选区蒙版的有效性判断 ──
    //
    // 三个条件缺一不可：
    //   · 非空
    //   · 尺寸与笔画蒙版一致（不一致说明选区还是旧画布尺寸的，
    //     这种"看起来有选区但坐标对不上"的情况必须当作全选处理，
    //     否则会擦错位置）
    //   · 格式为 Grayscale8
    const bool useSelection = !selectionMask.isNull()
                              && selectionMask.format() == QImage::Format_Grayscale8
                              && selectionMask.width() == strokeMask.width()
                              && selectionMask.height() == strokeMask.height();

    const int cr = color.red();
    const int cg = color.green();
    const int cb = color.blue();
    const int ca = color.alpha();

    for (int y = 0; y < h; ++y) {
        const uchar *maskLine = strokeMask.constScanLine(y);
        const uchar *selLine = useSelection ? selectionMask.constScanLine(y) : nullptr;
        QRgb *dstLine = reinterpret_cast<QRgb *>(work.scanLine(y));

        for (int x = 0; x < w; ++x) {
            int m = maskLine[x];
            if (m == 0)
                continue;       // 该点没被笔刷碰到，原样保留（绝大多数像素走这条路）

            // ── 选区蒙版乘进覆盖度 ──
            //
            // ★ 必须在颜色预乘之前做 ★
            //   见头文件说明：反过来会让选区外的像素变成
            //   "rgb 被完整预乘、alpha 却是 0"的不一致状态。
            //
            //   加权平均而不是直接取小值，是为了让羽化过的选区边缘
            //   得到正确的半透明过渡（直接取 min 会让羽化完全失效）。
            if (selLine) {
                m = (m * selLine[x] + 127) / 255;   // +127 是四舍五入
                if (m == 0)
                    continue;
            }

            const QRgb px = dstLine[x];

            if (erase) {
                // ── 橡皮：擦除到透明 ──
                //
                // 在预乘空间里，"降低 alpha"恰好等于"四个通道同乘一个系数"：
                //   预乘存储的是 (r·a, g·a, b·a, a)
                //   乘以 k 得到 (r·a·k, g·a·k, b·a·k, a·k)
                //   这正好是"alpha 变成 a·k"的预乘表示，颜色分量自动跟着缩。
                //
                // 这正是预乘格式在合成上的便利 —— 不需要解出原始颜色、
                // 乘完再重新预乘。也正因如此，这里**不能**做格式往返转换。
                const int keep = 255 - m;
                dstLine[x] = qRgba(qRed(px)   * keep / 255,
                                   qGreen(px) * keep / 255,
                                   qBlue(px)  * keep / 255,
                                   qAlpha(px) * keep / 255);
            } else {
                // ── 画笔：source-over（源覆盖）──
                //
                // 结果 = 源色 × srcA + 目标 × (1 - srcA)
                //
                // 其中"源色 × srcA"就是源色的预乘形式。
                // 所有运算在 0..255 的整数范围内完成，不需要浮点。
                const int srcA = (ca * m + 127) / 255;
                if (srcA == 0)
                    continue;

                const int inv = 255 - srcA;

                // 把画笔颜色按 srcA 预乘
                const int sr = (cr * srcA + 127) / 255;
                const int sg = (cg * srcA + 127) / 255;
                const int sb = (cb * srcA + 127) / 255;

                // ★ 这里不会溢出 255 ★
                //   sr ≤ srcA（因为 cr ≤ 255），且 qRed(px) ≤ qAlpha(px)（预乘性质），
                //   所以 sr + r·inv/255 ≤ srcA + a·inv/255 ≤ srcA + inv = 255。
                dstLine[x] = qRgba(sr + qRed(px)   * inv / 255,
                                   sg + qGreen(px) * inv / 255,
                                   sb + qBlue(px)  * inv / 255,
                                   srcA + qAlpha(px) * inv / 255);
            }
        }
    }

    return work;
}

} // namespace zl
