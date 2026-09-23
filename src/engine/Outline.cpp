#include "engine/Outline.h"

#include <QVector>

namespace zl {
namespace outline {

QPainterPath maskOutline(const QImage &grayMask, const QRect &boundsHint,
                         int threshold)
{
    QPainterPath path;

    if (grayMask.isNull() || grayMask.format() != QImage::Format_Grayscale8)
        return path;

    // ── 扫描范围 ──
    //
    // 只在"非零区域"的包围盒上扫，向外扩 1 像素 —— 边界线位于"内/外交界"处，
    // 需要访问到外圈那一圈未选中的邻居才能判断出来。
    const QRect scanRect =
        (boundsHint.isEmpty() ? grayMask.rect() : boundsHint.adjusted(-1, -1, 1, 1))
            .intersected(grayMask.rect());
    if (scanRect.isEmpty())
        return path;

    const int W = scanRect.width();
    const int H = scanRect.height();
    const int ox = scanRect.x();
    const int oy = scanRect.y();

    // ── 先二值化，摊平成一维字节数组 ──
    //
    // 之所以不直接访问 grayMask.constScanLine()：下面要在**两个方向**上
    // 做同样的扫描（横向找上下边界、纵向找左右边界），用一维数组可以
    // 用同一个索引公式表达两个方向，于是那段扫描代码只需要写一遍。
    QVector<uchar> bits(W * H);
    for (int y = 0; y < H; ++y) {
        const uchar *src = grayMask.constScanLine(y + oy) + ox;
        uchar *dst = bits.data() + y * W;
        for (int x = 0; x < W; ++x)
            dst[x] = (src[x] >= threshold) ? uchar(1) : uchar(0);
    }

    // 两个方向统一取值的访问器。
    //   dir == 0：a0 是 x、a1 是 y
    //   dir == 1：a0 是 y、a1 是 x（相当于把图转置）
    // 越界一律视为 0（未选中），这样画布边缘处也能正确生成边界线。
    auto at = [&](int dir, int a0, int a1) -> bool {
        if (dir == 0) {
            if (a0 < 0 || a1 < 0 || a0 >= W || a1 >= H) return false;
            return bits[a1 * W + a0] != 0;
        }
        if (a0 < 0 || a1 < 0 || a0 >= H || a1 >= W) return false;
        return bits[a0 * W + a1] != 0;
    };

    for (int dir = 0; dir < 2; ++dir) {
        const int len0 = (dir == 0) ? W : H;   // 沿线方向长度
        const int len1 = (dir == 0) ? H : W;   // 垂直方向长度

        for (int a1 = 0; a1 < len1; ++a1) {
            // side == 0 → 上边界（或左边界），参考格在 a1-1
            // side == 1 → 下边界（或右边界），参考格在 a1+1
            for (int side = 0; side < 2; ++side) {
                const int refOffset = (side == 0) ? -1 : +1;
                const int lineCoord = (side == 0) ? a1 : a1 + 1;

                int runStart = -1;
                // 循环多跑一格（a0 <= len0），用于在末端把最后一段收尾。
                for (int a0 = 0; a0 <= len0; ++a0) {
                    bool isEdge = false;
                    if (a0 < len0) {
                        const bool inside    = at(dir, a0, a1);
                        const bool refInside = at(dir, a0, a1 + refOffset);
                        isEdge = inside && !refInside;
                    }

                    if (isEdge) {
                        if (runStart < 0)
                            runStart = a0;
                    } else if (runStart >= 0) {
                        // [runStart, a0) 是一段连续的边界，合并成一条线段。
                        // 理由见 Outline.h：虚线相位 + 性能。
                        const qreal f  = qreal(runStart + (dir == 0 ? ox : oy));
                        const qreal t  = qreal(a0       + (dir == 0 ? ox : oy));
                        const qreal lc = qreal(lineCoord + (dir == 0 ? oy : ox));

                        if (dir == 0) {
                            path.moveTo(f, lc);
                            path.lineTo(t, lc);
                        } else {
                            path.moveTo(lc, f);
                            path.lineTo(lc, t);
                        }

                        runStart = -1;
                    }
                }
            }
        }
    }

    return path;
}

} // namespace outline
} // namespace zl
