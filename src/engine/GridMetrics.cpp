#include "engine/GridMetrics.h"

// ============================================================================
//  GridMetrics.cpp —— 网格吸附纯函数实现
// ----------------------------------------------------------------------------
//  全部是"输入 → 输出"，没有成员、没有状态。
//  这里唯一的技巧是 `qRound(v / g) * g`：先用整数除法找到**最近**的网格
//  序号（qRound 是四舍五入到最近的整数，正是"最近的网格线"），再乘回去。
//  写成 `int(v / g) * g`（截断）会变成"取左侧那条线"，
//  于是从右往左拖和从左往右拖的手感不一样 —— 是个很隐蔽的错。
// ============================================================================

#include <QtMath>

namespace zl {
namespace gridmetrics {

qreal nearestLine(qreal v, int gridSize)
{
    if (gridSize <= 0)
        return v;
    return qreal(qRound(v / qreal(gridSize))) * qreal(gridSize);
}

qreal snapValue(qreal v, int gridSize, qreal tolPx)
{
    if (gridSize <= 0)
        return v;

    // 负容差按 0 处理：语义是"只在已经贴合时才算吸附"（等于不动）。
    // 传负数通常意味着调用方算错了，静默当成 0 比"用负容差做出
    // 一个反向的判定"要安全。
    const qreal tol = (tolPx > 0.0) ? tolPx : 0.0;

    const qreal line = nearestLine(v, gridSize);
    return (qAbs(v - line) <= tol) ? line : v;
}

QPointF snapPoint(qreal x, qreal y, int gridSize, qreal tolPx)
{
    return QPointF(snapValue(x, gridSize, tolPx),
                   snapValue(y, gridSize, tolPx));
}

QPoint snapOffsetForRect(const QRect &rect, int gridSize, qreal tolPx)
{
    if (gridSize <= 0 || rect.isEmpty())
        return QPoint(0, 0);

    const qreal tol = (tolPx > 0.0) ? tolPx : 0.0;

    // ── 每轴：在两条边里挑"离网格更近"的那条 ──
    //
    //  ★ 为什么用 right()/bottom() 而不是 x()+width() ★
    //    QRect 的 right() 定义是 x + width - 1（闭区间），省得自己
    //    在"半开还是闭"上犯迷糊 —— 与绘制/命中测试的既有口径一致。
    //
    //  ★ 为什么要比"两边谁更近"★
    //    只吸左边的话，用户想让矩形的**右边**对齐时会发现怎么拖都对不上
    //    （左边被吸住，右边就由宽度决定，落在哪全看运气）。
    const qreal leftDist  = qAbs(qreal(rect.left())  - nearestLine(qreal(rect.left()),  gridSize));
    const qreal rightDist = qAbs(qreal(rect.right()) - nearestLine(qreal(rect.right()), gridSize));

    int dx = 0;
    if (rightDist < leftDist) {
        const qreal line = nearestLine(qreal(rect.right()), gridSize);
        if (rightDist <= tol)
            dx = int(qRound(line - qreal(rect.right())));
    } else {
        const qreal line = nearestLine(qreal(rect.left()), gridSize);
        if (leftDist <= tol)
            dx = int(qRound(line - qreal(rect.left())));
    }

    const qreal topDist    = qAbs(qreal(rect.top())    - nearestLine(qreal(rect.top()),    gridSize));
    const qreal bottomDist = qAbs(qreal(rect.bottom()) - nearestLine(qreal(rect.bottom()), gridSize));

    int dy = 0;
    if (bottomDist < topDist) {
        const qreal line = nearestLine(qreal(rect.bottom()), gridSize);
        if (bottomDist <= tol)
            dy = int(qRound(line - qreal(rect.bottom())));
    } else {
        const qreal line = nearestLine(qreal(rect.top()), gridSize);
        if (topDist <= tol)
            dy = int(qRound(line - qreal(rect.top())));
    }

    return QPoint(dx, dy);
}

} // namespace gridmetrics
} // namespace zl