#include "ui/RulerWidget.h"

#include "ui/CanvasView.h"

// ★ CanvasView.h 里 Document 只是前置声明 ★
//   本文件真的要用 document()->canvasSize()（算出刻度要画到哪），
//   所以必须把完整定义引进来 —— 否则报 "使用了未定义类型 zl::Document"。
#include "core/Document.h"

#include "compat/QtCompat.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>

#include <cmath>

namespace zl {

RulerWidget::RulerWidget(Qt::Orientation orientation, QWidget *parent)
    : QWidget(parent), m_orientation(orientation)
{
    if (m_orientation == Qt::Horizontal)
        setFixedHeight(kThickness);
    else
        setFixedWidth(kThickness);

    // 刻度数字用一个小号字 —— 尺子是辅助，不该抢画布的视觉重量。
    QFont f = font();
    f.setPointSizeF(7.0);
    f.setBold(false);
    setFont(f);
}

QSize RulerWidget::sizeHint() const
{
    return (m_orientation == Qt::Horizontal) ? QSize(0, kThickness)
                                             : QSize(kThickness, 0);
}

void RulerWidget::setView(CanvasView *view)
{
    if (m_view == view)
        return;

    if (m_view)
        disconnect(m_view, nullptr, this, nullptr);

    m_view = view;
    m_hasCursor = false;
    m_dragging  = false;

    if (m_view) {
        // 缩放/平移/旋转 → 刻度整体重算。
        connect(m_view, &CanvasView::viewChanged, this, [this] { update(); });

        // 光标移动 → 尺子上那个"此刻在哪一格"的指示器。
        //   ★ 取的是**沿自己那一轴**的分量 ★ 水平尺看 x，垂直尺看 y ——
        //     两把尺子听同一个信号、各取各的分量，不需要画布发两种信号。
        connect(m_view, &CanvasView::cursorMoved, this,
                [this](const QPointF &imagePos) {
            m_cursorImagePos = qRound((m_orientation == Qt::Horizontal)
                                          ? imagePos.x()
                                          : imagePos.y());
            m_hasCursor = true;
            update();
        });

        // ★ 画布被销毁时把指针摘掉 ★
        //   标尺活得比某一块画布久（标签页关掉了、或者析构顺序如此），
        //   而它手里是**裸指针**。不接这一条，最坏情况下一次延迟重绘就会
        //   踩到已释放的画布 —— 与"销毁前先让裸指针消费者松手"同一条规律
        //   （见 docs/实现铁律.md 第十七章）。
        connect(m_view, &QObject::destroyed, this, [this] {
            m_view      = nullptr;
            m_hasCursor = false;
        });
    }

    update();
}

void RulerWidget::setUnit(rulermetrics::Unit unit)
{
    if (m_unit == unit)
        return;
    m_unit = unit;
    update();
}

void RulerWidget::setDpi(int dpi)
{
    const int d = (dpi > 0) ? dpi : 96;
    if (m_dpi == d)
        return;
    m_dpi = d;
    update();
}

// ---------------------------------------------------------------------------
//  坐标辅助
// ---------------------------------------------------------------------------

QPoint RulerWidget::canvasOrigin() const
{
    if (!m_view)
        return QPoint();
    // ★★ 必须经**全局坐标**中转，不能写 mapTo(this, ...) ★★
    //   标尺与画布是**兄弟**：两者都挂在中央网格宿主上（画布在标签容器里）。
    //   而 QWidget::mapTo(parent, pos) 是从自己**沿父链往上找** parent ——
    //   参数必须是自己的**祖先**。传一个兄弟进去，父链走到头就变成 nullptr，
    //   紧接着 w->mapToParent(p) 直接解引用空指针 → 进程当场段错误。
    //   （这一步在 show() 之后第一次重绘时触发，所以症状是"构造没事、一显示就崩"。）
    //   走 mapToGlobal + mapFromGlobal 则与层级无关，兄弟、堂兄弟都成立。
    return mapFromGlobal(m_view->mapToGlobal(QPoint(0, 0)));
}

int RulerWidget::perpendicularImagePos(const QPoint &posInWidget) const
{
    if (!m_view)
        return 0;

    const QPoint origin = canvasOrigin();

    if (m_orientation == Qt::Horizontal) {
        // 水平尺 → 水平参考线 → 位置是 **y**
        const QPointF img =
            m_view->widgetToImage(QPointF(0.0, qreal(posInWidget.y() - origin.y())));
        return qRound(img.y());
    }

    // 垂直尺 → 垂直参考线 → 位置是 **x**
    const QPointF img =
        m_view->widgetToImage(QPointF(qreal(posInWidget.x() - origin.x()), 0.0));
    return qRound(img.x());
}

int RulerWidget::cursorImagePosAlongAxis() const
{
    return m_hasCursor ? m_cursorImagePos : 0;
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------

void RulerWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter p(this);
    const bool horiz = (m_orientation == Qt::Horizontal);

    // ★ 深色主题适配（2026-10-15）★
    //   Qt6 会跟随系统深色主题把 QPalette 整体变暗，而这里原来是四处
    //   硬编码浅灰（背景 238 / 分界 178 / 次刻度 160 / 主刻度 90）——
    //   深色环境下标尺就是"一块白"，非常突兀。
    //   改为从 palette 派生，深浅两主题都自适应：
    //     背景         Window      （浅 ~240 / 深 ~53）
    //     分界线       Midlight    （浅 ~178）
    //     次刻度       Mid         （浅 ~160）
    //     主刻度+数字  WindowText  （浅 ~0 / 深 ~230）
    //   光标指示器的橙色保留 —— 它是"临时状态"色，两种主题下都醒目，
    //   而且要与刻度的灰、参考线的蓝区分开（设计意图，见下方注释）。
    const QPalette pal = palette();
    const QColor cBackground = pal.color(QPalette::Window);
    const QColor cDivider    = pal.color(QPalette::Midlight);
    const QColor cMinor      = pal.color(QPalette::Mid);
    const QColor cMajor      = pal.color(QPalette::WindowText);

    p.fillRect(rect(), cBackground);

    // 与画布之间的分界线（靠画布那一侧）
    p.setPen(cDivider);
    if (horiz)
        p.drawLine(0, height() - 1, width() - 1, height() - 1);
    else
        p.drawLine(width() - 1, 0, width() - 1, height() - 1);

    // ★ 没有文档时**不画刻度** ★
    //   空文档状态下画出"0 100 200…"会让人以为画布上有内容 ——
    //   尺子只在真的有图可量的时候才有意义。
    if (!m_view || !m_view->document())
        return;

    const QSize  canvasSize = m_view->document()->canvasSize();
    const QPoint origin     = canvasOrigin();
    const qreal  zoom       = m_view->zoom();

    // 主刻度之间至少要留出"4 个数字 + 间隔"的宽度，否则叠字。
    const qreal minSpacing =
        p.fontMetrics().horizontalAdvance(QStringLiteral("000.0")) + 12.0;

    const rulermetrics::Ticks ticks =
        rulermetrics::chooseTicks(zoom, m_unit, m_dpi, minSpacing);
    const qreal ppu      = rulermetrics::pixelsPerUnit(m_unit, m_dpi);
    const qreal majorImg = ticks.major * ppu;
    const qreal minorImg = ticks.minor * ppu;
    if (!(majorImg > 0.0) || !(minorImg > 0.0))
        return;

    const int   axisLen       = horiz ? width() : height();
    const int   axisOrigin    = horiz ? origin.x() : origin.y();
    const qreal canvasExtent  = horiz ? canvasSize.width() : canvasSize.height();

    // 本控件沿轴坐标 ⇄ 图像坐标（两条都走画布那**同一个**变换）
    const auto toWidget = [&](qreal imgPos) -> qreal {
        return horiz ? (m_view->imageToWidget(QPointF(imgPos, 0.0)).x() + axisOrigin)
                     : (m_view->imageToWidget(QPointF(0.0, imgPos)).y() + axisOrigin);
    };
    const auto toImage = [&](qreal widgetPos) -> qreal {
        return horiz
                   ? m_view->widgetToImage(QPointF(widgetPos - axisOrigin, 0.0)).x()
                   : m_view->widgetToImage(QPointF(0.0, widgetPos - axisOrigin)).y();
    };

    // 可见范围（图像坐标），与图像本身的范围求交（多一点余量，边缘刻度不至于半截）
    qreal lo = qMin(toImage(0.0), toImage(qreal(axisLen)));
    qreal hi = qMax(toImage(0.0), toImage(qreal(axisLen)));
    lo = qMax(lo, -majorImg);
    hi = qMin(hi, canvasExtent + majorImg);

    if (hi > lo) {
        // ── 次刻度（短）──
        //   屏幕上细到 3px 以下就不画了：那是"一片灰"，不是刻度。
        if (minorImg * zoom >= 3.0) {
            p.setPen(cMinor);
            const qreal i0 = std::floor(lo / minorImg);
            const qreal i1 = std::ceil(hi / minorImg);
            for (qreal i = i0; i <= i1; i += 1.0) {
                const qreal w = toWidget(i * minorImg);
                if (horiz)
                    p.drawLine(QPointF(w, height() - 4), QPointF(w, height() - 1));
                else
                    p.drawLine(QPointF(width() - 4, w), QPointF(width() - 1, w));
            }
        }

        // ── 主刻度（长）+ 数字 ──
        p.setPen(cMajor);
        const qreal j0 = std::floor(lo / majorImg);
        const qreal j1 = std::ceil(hi / majorImg);
        for (qreal j = j0; j <= j1; j += 1.0) {
            const qreal v = j * majorImg;
            const qreal w = toWidget(v);

            if (horiz)
                p.drawLine(QPointF(w, height() - 8), QPointF(w, height() - 1));
            else
                p.drawLine(QPointF(width() - 8, w), QPointF(width() - 1, w));

            const QString label =
                rulermetrics::formatLabel(v / ppu, m_unit, ticks.major);

            if (horiz) {
                // 最右一格如果数字放不下就贴右边界，别让它被裁掉半截。
                const int tw = p.fontMetrics().horizontalAdvance(label);
                qreal tx = w + 2.0;
                if (tx + tw > width())
                    tx = qMax(0.0, qreal(width() - tw - 1));
                p.drawText(QPointF(tx, height() - 2), label);
            } else {
                p.save();
                p.translate(width() - 3, w - 2);
                p.rotate(-90.0);   // 竖尺上的数字横躺着，从刻度往下读
                p.drawText(QPointF(0, 0), label);
                p.restore();
            }
        }
    }

    // ── 光标指示器 ──
    //   画布上的光标投在尺子上的那一格。橙色是刻意的：它是"临时状态"，
    //   要与刻度的深灰、参考线的蓝区分开。
    if (m_hasCursor) {
        const qreal w = toWidget(qreal(cursorImagePosAlongAxis()));
        p.setPen(QColor(240, 130, 20));
        if (horiz)
            p.drawLine(QPointF(w, 0), QPointF(w, height() - 1));
        else
            p.drawLine(QPointF(0, w), QPointF(width() - 1, w));
    }
}

void RulerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 可见刻度范围随长度变化 —— 主动重绘一次，不依赖"Qt 会自动重画"。
    update();
}

// ---------------------------------------------------------------------------
//  交互：从尺子上拖出一根参考线
// ---------------------------------------------------------------------------
//
//  ★ 标尺上没有"可抓的东西" ★
//    参考线都躺在画布范围内（尺子只占 18px），所以尺子上的按下**永远是新建**；
//    移动与删除都在画布侧（见 CanvasView 的参考线拖拽）。
//    这也是 Photoshop 的行为：新建从尺子拖出，移动/删除在画布上做。
//
//  ★ 松手位置决定"落线"还是"放弃" ★
//    松在尺子自己身上（rect() 内）= 放弃；松到画布那边 = 落线。
//    Qt 在按下时会做**隐式鼠标抓取**，所以即使光标已经移到画布上，
//    本控件仍然收到 move/release —— 这正是这套判定能成立的前提。

void RulerWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_view || !m_view->document()) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_dragging      = true;
    m_lastPosImage  = perpendicularImagePos(
        zl::eventPos(event).toPoint());
    emit guideDragMoved(m_orientation, m_lastPosImage);
    event->accept();
}

void RulerWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const int posImage =
        perpendicularImagePos(zl::eventPos(event).toPoint());
    if (posImage != m_lastPosImage) {
        m_lastPosImage = posImage;
        emit guideDragMoved(m_orientation, posImage);
    }
    event->accept();
}

void RulerWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    m_dragging = false;
    const bool commit = !rect().contains(zl::eventPos(event).toPoint());
    emit guideDragFinished(m_orientation, commit);
    event->accept();
}

} // namespace zl
