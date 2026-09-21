#include "engine/Shape.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QColor>
#include <QtMath>

#include <cstring>   // memcpy

namespace zl {

// ===========================================================================
//  内部辅助
// ===========================================================================
namespace {

// 把"预乘的绘制结果"变成覆盖度蒙版：**直接取 alpha 通道**。
//
// ★ 为什么不 convertToFormat(Format_Grayscale8) ★
//   见 Shape.h 顶部那段说明。一句话：ARGB32_Premultiplied → Grayscale8 的
//   转换是否先解预乘（unpremultiply）由 Qt 内部的转换路径决定，一旦它解了，
//   抗锯齿边缘那圈"半透明的白"会被还原成不透明的白（255），
//   抗锯齿静默失效、边缘变回硬台阶，而代码里一个字都没写错。
//
//   取 alpha 则语义唯一：alpha 就是"这个像素被覆盖了多少"，
//   正是 composeStroke 对 mask 的定义。
QImage alphaToCoverage(const QImage &premul)
{
    if (premul.isNull())
        return QImage();

    QImage out(premul.size(), QImage::Format_Grayscale8);
    if (out.isNull())
        return QImage();

    for (int y = 0; y < premul.height(); ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(premul.constScanLine(y));
        uchar *dst = out.scanLine(y);
        for (int x = 0; x < premul.width(); ++x)
            dst[x] = uchar(qAlpha(src[x]));
    }

    return out;
}

// 建一张"只用来接 alpha"的临时绘制面。
//
// 透明度不能省：底不透明的话，画在它上面的东西的 alpha 永远是 255，
// 覆盖度就全变成"满"，抗锯齿与线宽的信息全部丢失。
QImage makeScratch(const QSize &size)
{
    QImage scratch(size, QImage::Format_ARGB32_Premultiplied);
    if (scratch.isNull())
        return scratch;      // 内存不足：调用方会退化成"空结果"，不崩
    scratch.fill(Qt::transparent);
    return scratch;
}

// 图像坐标的矩形 → 蒙版包围盒（取整、外扩、与画布求交）
//
// ★ 一律"向外取整"★
//   toAlignedRect() 取的是完全包含该浮点矩形的最小整数矩形（左上 floor、
//   右下 ceil）。宁可多算一行像素（改完再撤销时多存一行），也不能少算 ——
//   少算的后果是形状边缘缺一块，而且撤销无法完整还原。
QRect maskBox(const QRectF &rect, qreal pad, const QSize &canvasSize)
{
    QRect box = rect.normalized().adjusted(-pad, -pad, pad, pad).toAlignedRect();
    box &= QRect(QPoint(0, 0), canvasSize);
    return box;
}

} // namespace

// ===========================================================================
//  光栅化：直线
// ===========================================================================
ShapeMask rasterizeLine(const QSize &canvasSize,
                        const QPointF &from, const QPointF &to,
                        int width, Qt::PenStyle style, bool antialias)
{
    ShapeMask result;

    if (canvasSize.isEmpty())
        return result;

    // ★ 两点重合（用户点了一下没拖）→ 什么都不画 ★
    //
    //   这是与矩形/椭圆的"零面积不画"**同一条规则**：没有拖出长度就不是一个
    //   形状。看起来像小事，但圆头笔（RoundCap）在两点重合时确实会画出一个小
    //   圆点 —— 于是"点了一下"变成"画布上凭空多了个痕迹，还得撤销它"。
    //   "误点产生垃圾"是绘图软件里最恼人的一类小毛病。
    //
    //   阈值 0.5 像素：它低于任何**有意**的拖动。注意高倍缩放下（800% 时
    //   一个屏幕像素 = 0.125 图像像素）用户手抖也可能落在阈值内 —— 但那本来
    //   就不是在画线；想点像素应当用画笔。
    //
    //   ★ 断言落点 ★
    //     tests/shape_tool_test.cpp 的 [13] 直接钉住"点一下不产生任何改动、
    //     也不压撤销命令"；[2] 另有一条"零长度线段产出空蒙版"。
    const QPointF delta = to - from;
    if (qAbs(delta.x()) < 0.5 && qAbs(delta.y()) < 0.5)
        return result;

    // 线宽下限 1：0 或负数在 QPainter 里会被理解成"CosmeticPen"（1 像素宽），
    // 结果看似正确，但语义就错了（用户设的 0 变成了别的意思）。这里显式钳制。
    const qreal penWidth = qMax(1, width);

    // 包围盒 = 两端点的外接矩形 + 半个线宽（圆头两端各外扩 penWidth/2）
    //          + 2 像素余量（抗锯齿过渡带 + 浮点取整）
    const QRect box = maskBox(QRectF(from, to), penWidth / 2.0 + 2.0, canvasSize);
    if (box.isEmpty())
        return result;      // 整条线都在画布外

    QImage scratch = makeScratch(box.size());
    if (scratch.isNull())
        return result;

    {
        QPainter painter(&scratch);
        if (!painter.isActive())
            return result;

        painter.setRenderHint(QPainter::Antialiasing, antialias);

        // ★ 平移而不是换算：把图像坐标系搬进"蒙版局部坐标系" ★
        //   之后的绘制代码完全按图像坐标写，与整画布绘制时的代码一模一样 ——
        //   这样"局部光栅化"不会引入任何与坐标相关的分支。
        painter.translate(-box.x(), -box.y());

        // 白色无所谓（我们只取 alpha），但保持不透明白是"不引入额外变量"的做法：
        // 透明色会让 alpha 恒为 0，肉眼调试时看不出任何东西。
        const QPen pen(QColor(255, 255, 255, 255), penWidth, style,
                       Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawLine(from, to);
        painter.end();
    }

    result.bounds = box;
    result.strokeMask = alphaToCoverage(scratch);
    return result;
}

// ===========================================================================
//  光栅化：矩形 / 椭圆
// ===========================================================================
ShapeMask rasterizeShape(const QSize &canvasSize, const QRectF &rect,
                         ShapeKind kind, ShapeMode mode,
                         int strokeWidth, int cornerRadius, bool antialias)
{
    ShapeMask result;

    if (canvasSize.isEmpty())
        return result;

    const QRectF r = rect.normalized();
    if (r.isEmpty())
        return result;      // 零面积（用户点了一下没拖）→ 什么都不画

    const bool wantStroke = (mode != ShapeMode::Fill);
    const bool wantFill   = (mode != ShapeMode::Stroke);

    // ── 路径 ──
    //
    // 用 QPainterPath 而不是分别调 fillRect / drawEllipse：
    // 填充与描边**必须落在同一条路径上**，否则"圆角矩形的填充"与
    // "圆角矩形的描边"会是两个形状，在圆角处对不齐（露出一圈毛边）。
    QPainterPath path;
    if (kind == ShapeKind::Ellipse) {
        path.addEllipse(r);
    } else {
        // ★ 圆角半径必须钳到半边以内 ★
        //   半径超过半宽/半高时 Qt 会画出畸形（自交的圆弧），
        //   而用户完全可能在选项条里把半径拖到比矩形还大。
        const qreal maxRadius = qMin(r.width(), r.height()) / 2.0;
        const qreal cr = qBound(0.0, qreal(cornerRadius), maxRadius);
        if (cr > 0.0)
            path.addRoundedRect(r, cr, cr);
        else
            path.addRect(r);
    }

    // ── 包围盒 ──
    //   描边会从路径本身向外扩张 penWidth/2，填充不会。
    const qreal pad = (wantStroke ? qMax(1, strokeWidth) / 2.0 : 0.0) + 2.0;
    const QRect box = maskBox(r, pad, canvasSize);
    if (box.isEmpty())
        return result;

    // 绘制一遍，可指定"只填"或"只描"，返回该遍的覆盖度蒙版。
    //
    // ★ 填充与描边为什么要画两遍 ★
    //   因为它们要落在**两张蒙版**上（两个不同颜色、分两次合成）。
    //   画在一张上再取 alpha 的话，重叠区域的 alpha 会被 max 成一个值，
    //   描边色就再也分不出来了。
    auto render = [&](bool fill, bool stroke) -> QImage {
        QImage scratch = makeScratch(box.size());
        if (scratch.isNull())
            return QImage();

        QPainter painter(&scratch);
        if (!painter.isActive())
            return QImage();

        painter.setRenderHint(QPainter::Antialiasing, antialias);
        painter.translate(-box.x(), -box.y());

        if (fill)
            painter.fillPath(path, QColor(255, 255, 255, 255));

        if (stroke) {
            // 闭合路径上 cap 类型不影响结果（起点与终点重合），
            // Join 用 MiterJoin：矩形/椭圆的笔画交接处保持尖锐，
            // 与 Photoshop 的形状描边一致。
            const QPen pen(QColor(255, 255, 255, 255), qMax(1, strokeWidth),
                           Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
            painter.strokePath(path, pen);
        }

        painter.end();
        return alphaToCoverage(scratch);
    };

    result.bounds = box;
    if (wantFill)
        result.fillMask = render(true, false);
    if (wantStroke)
        result.strokeMask = render(false, true);

    return result;
}

// ===========================================================================
//  位置搬运
// ===========================================================================
QImage embedMask(const QImage &mask, const QRect &maskBounds, const QRect &region)
{
    if (mask.isNull() || maskBounds.isEmpty() || region.isEmpty())
        return QImage();

    // region 必须完整包含 maskBounds。不满足说明调用方算错了区域 ——
    // 这时**必须返回空图**（= 不做任何合成），而不是硬拷一部分：
    // 硬拷会把蒙版放错位置，在画面上表现为"形状突然跳了一下"。
    if (!region.contains(maskBounds))
        return QImage();

    QImage out(region.size(), QImage::Format_Grayscale8);
    if (out.isNull())
        return QImage();

    // ★ 必须清成 0（而不是留未初始化的内存）★
    //   0 在 composeStroke 里的含义是"这个像素没被形状碰到，原样保留"。
    //   若这里留着垃圾值，区域里就会凭空出现噪点；若留着上一帧的形状，
    //   形状拖动时会拖一条擦不掉的尾巴。
    out.fill(0);

    const int dx = maskBounds.x() - region.x();
    const int dy = maskBounds.y() - region.y();

    // Grayscale8 每像素 1 字节，但**行可能有对齐填充**（bytesPerLine >= width）。
    // 所以只能逐行按 width 拷，不能把整块内存一次 memcpy 过去。
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *src = mask.constScanLine(y);
        uchar *dst = out.scanLine(y + dy);
        std::memcpy(dst + dx, src, size_t(mask.width()));
    }

    return out;
}

// ===========================================================================
//  纯几何：Shift 约束
// ===========================================================================
QPointF constrainTo8Directions(const QPointF &start, const QPointF &current)
{
    const qreal dx = current.x() - start.x();
    const qreal dy = current.y() - start.y();
    const qreal adx = qAbs(dx);
    const qreal ady = qAbs(dy);

    // 两个点重合：没有方向可言，原样返回
    if (adx < 0.5 && ady < 0.5)
        return current;

    // tan(22.5°) ≈ 0.4142：这是"水平/垂直扇区"与"对角线扇区"的分界角。
    // 用比例判定而不是 qAtan2 算角度，省一次三角函数，且没有象限分支。
    const qreal kTan22_5 = 0.4142135623730950;

    if (ady <= adx * kTan22_5) {
        // 水平：y 锁到起点
        return QPointF(current.x(), start.y());
    }
    if (adx <= ady * kTan22_5) {
        // 垂直：x 锁到起点
        return QPointF(start.x(), current.y());
    }

    // 对角线：取当前点在 45° 方向上的**投影长度**，再按符号摊到两个轴上。
    //
    //   单位方向向量是 (±1, ±1)/√2，投影长度 = (dx ± dy)/√2，
    //   于是 x 分量 = (dx+dy)/2（或 (dx-dy)/2）。
    //   取 |dx| 与 |dy| 的**平均**，等价于对"近似 45° 的手抖"取中值：
    //   用户拖到 (100, 92) 时，得到的是 (96, 96) 而不是 (100, 100) ——
    //   线长与原拖动长度接近，不会"越拖越长"。
    const qreal d = (adx + ady) / 2.0;
    return QPointF(start.x() + (dx < 0.0 ? -d : d),
                   start.y() + (dy < 0.0 ? -d : d));
}

QRectF rectFromDrag(const QPointF &start, const QPointF &current, bool square)
{
    qreal dx = current.x() - start.x();
    qreal dy = current.y() - start.y();

    if (square) {
        // 边长取较大的那一边 —— 与 Photoshop 一致：
        // 用户按住 Shift 时希望"至少包住我拖过的范围"，而不是被较小的一边卡住。
        const qreal d = qMax(qAbs(dx), qAbs(dy));

        // ★ 符号跟随拖动方向 ★
        //   往左上拖（dx<0）必须得到向左上展开的正方形。
        //   如果统一取正号，正方形会突然翻到相反象限 —— 用户会觉得"按住 Shift
        //   之后图形跑到鼠标另一边去了"。
        //   dx 恰好为 0 时（纯垂直拖动）取正号，行为固定可预期。
        dx = (dx < 0.0) ? -d : d;
        dy = (dy < 0.0) ? -d : d;
    }

    return QRectF(start, QPointF(start.x() + dx, start.y() + dy)).normalized();
}

} // namespace zl
