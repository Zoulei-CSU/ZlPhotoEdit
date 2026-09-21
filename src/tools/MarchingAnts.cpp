#include "tools/MarchingAnts.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QColor>

namespace zl {

void drawMarchingAnts(QPainter &painter, const QPainterPath &path, int dashPhase)
{
    if (path.isEmpty())
        return;

    // ── 关抗锯齿 ──
    //
    // ★ 这一条不是"顺手设的"，而是必须的 ★
    //   选区的边界是"内/外交界"，天然落在半像素位置上。
    //   开抗锯齿后，1 像素宽的黑线会被摊成两条各 50% 灰度的线，
    //   白虚线也会跟着糊掉 —— 最终看到的是"一条灰糊糊的边"，
    //   黑白交替的对比完全消失，在复杂照片上就难以辨认了。
    //   像素级的界面装饰线本来就该是硬边。
    painter.setRenderHint(QPainter::Antialiasing, false);

    // 备份并清掉画刷，避免调用方留下的填充意外填出形状内部
    const QBrush oldBrush = painter.brush();
    painter.setBrush(Qt::NoBrush);

    // ── 第一层：实心黑线打底 ──
    QPen basePen;
    basePen.setColor(QColor(0, 0, 0));
    basePen.setWidth(1);
    basePen.setStyle(Qt::SolidLine);
    // cosmetic：线宽恒定于设备像素，不随 painter 的缩放变化
    basePen.setCosmetic(true);
    basePen.setJoinStyle(Qt::MiterJoin);
    painter.setPen(basePen);
    painter.drawPath(path);

    // ── 第二层：白色虚线盖在上面 ──
    QPen dashPen;
    dashPen.setColor(QColor(255, 255, 255));
    dashPen.setWidth(1);
    // ★ 用 CustomDashLine 而不是 Qt::DashLine ★
    //   Qt::DashLine 的具体图案在 Qt5 / Qt6 之间被调整过，
    //   自定义图案能让外观在两个大版本上完全一致。
    dashPen.setStyle(Qt::CustomDashLine);
    dashPen.setCosmetic(true);
    dashPen.setJoinStyle(Qt::MiterJoin);
    // 4 实 4 虚（设备像素）。这个比例是有讲究的：
    //   周期 8 像素 —— 太短看着像点线，太长又失去了"流动"的动感。
    dashPen.setDashPattern({4.0, 4.0});
    // 相位让虚线段沿路径流动起来（蚂蚁线的核心）
    dashPen.setDashOffset(qreal(dashPhase));
    painter.setPen(dashPen);
    painter.drawPath(path);

    painter.setBrush(oldBrush);
}

} // namespace zl
