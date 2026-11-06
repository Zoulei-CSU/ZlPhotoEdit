#include "tools/PerspectiveTool.h"

#include "core/Document.h"
#include "engine/DocumentScan.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace zl {

namespace {

// 角柄的屏幕尺寸（像素）。与 CropTool 取同一档，手感才一致。
constexpr qreal kHandleScreenSize = 9.0;

// 命中容差在"半个柄"之外再放宽的屏幕像素。
// 手指/触控板拖动时不必精确压在 9px 的小方块上。
constexpr qreal kHandleHitSlack = 4.0;

// 四边形外区域的压暗透明度。比裁剪的 110 略轻 ——
// 展平时用户还要看清框外的纸边有没有对准，压太暗反而看不见。
constexpr int kMaskAlpha = 90;

} // namespace

PerspectiveTool::PerspectiveTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
{
}

PerspectiveTool::~PerspectiveTool() = default;

QCursor PerspectiveTool::cursor() const
{
    return QCursor(Qt::CrossCursor);
}

// ===========================================================================
//  几何辅助
// ===========================================================================

QRectF PerspectiveTool::canvasBounds() const
{
    if (!document())
        return QRectF();
    const QSize cs = document()->canvasSize();
    return QRectF(QPointF(0, 0), QSizeF(cs));
}

QPolygonF PerspectiveTool::seededQuad(const QRectF &bounds, qreal insetRatio)
{
    if (bounds.isEmpty())
        return QPolygonF();

    const qreal ratio = std::clamp(insetRatio, 0.0, 0.45);
    const qreal inset = std::min(bounds.width(), bounds.height()) * ratio;

    const qreal l = bounds.left() + inset;
    const qreal t = bounds.top() + inset;
    const qreal r = bounds.right() - inset;
    const qreal b = bounds.bottom() - inset;

    const QRectF inner(QPointF(l, t), QPointF(r, b));
    if (inner.width() < 2.0 || inner.height() < 2.0) {
        // 画布极小时不再内缩 —— 内缩会把四边形压成一条线，
        // 而退化的四边形会让展平直接失败。
        return QPolygonF() << bounds.topLeft() << bounds.topRight()
                           << bounds.bottomRight() << bounds.bottomLeft();
    }

    return QPolygonF() << inner.topLeft() << inner.topRight()
                       << inner.bottomRight() << inner.bottomLeft();
}

void PerspectiveTool::ensureSeeded()
{
    if (!isReady())
        return;

    const QSize cs = document()->canvasSize();
    if (cs.isEmpty())
        return;

    // 画布尺寸变了（刚展平过一次、或撤销了一次）→ 旧四角是旧画布的坐标，
    // 必须重新播种。这与 CropTool 的处理一致：这是唯一会让框"自己动"的时机。
    if (m_seeded && m_seedCanvasSize == cs)
        return;

    const QPolygonF seed = seededQuad(canvasBounds());
    if (seed.size() != 4)
        return;

    m_quad = seed;
    m_seedCanvasSize = cs;
    m_seeded = true;
}

QPointF PerspectiveTool::clampToCanvas(const QPointF &p) const
{
    const QRectF b = canvasBounds();
    return QPointF(std::clamp(p.x(), b.left(), b.right()),
                   std::clamp(p.y(), b.top(), b.bottom()));
}

QSize PerspectiveTool::currentOutputSize() const
{
    return scan::suggestOutputSize(m_quad);
}

int PerspectiveTool::hitCorner(const QPointF &imagePos, qreal zoom) const
{
    if (m_quad.size() != 4)
        return -1;

    // 屏幕上的可点半径 → 图像坐标。zoom 取保护值，
    // 否则极端缩放下容差会变成 0（永远点不中）或大到覆盖整张图。
    const qreal z = std::max(0.01, std::abs(zoom));
    const qreal tol = (kHandleScreenSize / 2.0 + kHandleHitSlack) / z;

    int best = -1;
    qreal bestDist = tol;

    for (int i = 0; i < 4; ++i) {
        const QPointF d = m_quad[i] - imagePos;
        const qreal dist = std::hypot(d.x(), d.y());
        // 严格小于：距离相同时保留序号小的，行为可预期（与遍历顺序一致）
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

// ===========================================================================
//  输入事件
// ===========================================================================

ToolResult PerspectiveTool::mousePress(const ToolEvent &e)
{
    if (!isReady() || e.button != Qt::LeftButton)
        return ToolResult::ignored();

    ensureSeeded();

    const int corner = hitCorner(e.imagePos, e.viewZoom);
    if (corner < 0) {
        // 没点中角柄：不猜用户想拖哪个角（猜错会把对角也带歪），
        // 只提示一句。这比"自动选最近角"安全 —— 后者在四个角挨得近时
        // 会让用户拖 A 角却动了 B 角。
        if (e.viewZoom < 1.0)
            showStatus(tr("Drag a corner onto the paper corner - zoom in for precision"), 3000);
        else
            showStatus(tr("Drag a corner onto the paper corner"), 2000);
        return ToolResult::accepted();
    }

    m_dragCorner = corner;
    setBusy(true);
    return ToolResult::repaint();
}

ToolResult PerspectiveTool::mouseMove(const ToolEvent &e)
{
    if (!isReady() || m_dragCorner < 0)
        return ToolResult::ignored();

    if (!e.isDragging())
        return ToolResult::ignored();

    m_quad[m_dragCorner] = clampToCanvas(e.imagePos);
    return ToolResult::repaint();
}

ToolResult PerspectiveTool::mouseRelease(const ToolEvent &e)
{
    Q_UNUSED(e)
    if (m_dragCorner < 0)
        return ToolResult::ignored();

    m_dragCorner = -1;
    setBusy(false);
    return ToolResult::repaint();
}

ToolResult PerspectiveTool::mouseDoubleClick(const ToolEvent &e)
{
    Q_UNUSED(e)
    if (!isReady())
        return ToolResult::ignored();

    ensureSeeded();
    emitRequest();
    return ToolResult::accepted();
}

ToolResult PerspectiveTool::keyPress(const ToolEvent &e)
{
    if (!isReady())
        return ToolResult::ignored();

    if (e.key == Qt::Key_Return || e.key == Qt::Key_Enter) {
        ensureSeeded();
        emitRequest();
        return ToolResult::accepted();
    }

    return ToolResult::ignored();
}

void PerspectiveTool::emitRequest()
{
    if (m_quad.size() != 4)
        return;

    if (!scan::isQuadUsable(m_quad)) {
        // 常见成因：用户把某个角拖到了对边或对角上，四点共线 / 凹进去了。
        // 这种情况给明确提示，而不是让展平失败后弹"操作失败"。
        showStatus(tr("The four corners are not a valid quadrilateral - drag them apart"), 3500);
        return;
    }

    const QSize out = scan::suggestOutputSize(m_quad);
    if (out.isEmpty())
        return;

    emit perspectiveRequested(m_quad, out);
}

// ===========================================================================
//  叠加层
// ===========================================================================

void PerspectiveTool::paintOverlay(QPainter &painter, CanvasView *view)
{
    Q_UNUSED(view)

    if (!isReady())
        return;

    ensureSeeded();
    if (m_quad.size() != 4)
        return;

    const QRectF bounds = canvasBounds();
    if (bounds.isEmpty())
        return;

    // ── ① 四边形之外压暗 ──
    //
    // 用"两个子路径 + 奇偶填充"而不是四条边各画一个矩形：
    // 后者在角上会互相重叠，半透明色叠两次，四个角明显更黑。
    QPainterPath mask;
    mask.setFillRule(Qt::OddEvenFill);
    mask.addRect(bounds);
    mask.addPolygon(m_quad);

    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, kMaskAlpha));
    painter.drawPath(mask);
    painter.restore();

    // ── ② 视图缩放 ──
    //   与 CropTool 同一个口径：painter 此刻的变换就是画布的 m_viewTransform，
    //   从矩阵里读缩放，tools 层就不必依赖 ui 层。
    const QTransform viewTransform = painter.worldTransform();
    const qreal zoom = std::max(0.01, std::abs(viewTransform.m11()));

    // ── ③ 四边（图像坐标，线宽折算成"屏幕上 1 像素"）──
    painter.save();
    QPen edge(QColor(255, 255, 255, 235));
    edge.setWidthF(1.0 / zoom);
    edge.setStyle(Qt::DashLine);
    // ★ 虚线间隔的单位是"笔宽"★（Qt 的约定），笔宽已经是 1/zoom 图像像素，
    //   所以 {6,4} 正好等于屏幕上 6px 实线 + 4px 空隙 —— 不随缩放变化。
    edge.setDashPattern({6.0, 4.0});
    painter.setPen(edge);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(m_quad);
    painter.restore();

    // ── ④ 角柄与尺寸标签（控件坐标）──
    //
    // ★ 必须 resetTransform ★
    //   角柄要保持固定的**屏幕**尺寸（架构铁律 9）。在图像坐标下画，
    //   9px 的柄在 25% 缩放下只有 2px（点不中），在 800% 下是 72px（糊住画面）。
    painter.save();
    painter.resetTransform();

    const qreal hs = kHandleScreenSize / 2.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF p = viewTransform.map(m_quad[i]);

        // 正在拖的那个角用高亮色，用户能看出"刚才抓的是这个"
        const bool active = (i == m_dragCorner);
        painter.setPen(QPen(QColor(40, 40, 40, 220), 1.0));
        painter.setBrush(active ? QColor(90, 170, 255, 245)
                                : QColor(255, 255, 255, 235));
        painter.drawRect(QRectF(p.x() - hs, p.y() - hs, hs * 2.0, hs * 2.0));

        // 角序号（1~4）：让"从左上角开始按顺时针"这件事在界面上可见，
        // 用户报问题时能说"我拖的是 3 号角"。
        if (zoom >= 0.4) {
            painter.setPen(QColor(70, 70, 70, 220));
            QFont f = painter.font();
            f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.0));
            painter.setFont(f);
            painter.drawText(QRectF(p.x() - hs, p.y() - hs - 16.0, 26.0, 14.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QString::number(i + 1));
        }
    }

    // 结果尺寸标签：用户不用点确定就知道能不能保住清晰度
    const QSize out = scan::suggestOutputSize(m_quad);
    if (out.isValid() && !out.isEmpty()) {
        const QString text = tr("%1 × %2 px").arg(out.width()).arg(out.height());

        QFont f = painter.font();
        f.setPointSizeF(std::max(9.0, f.pointSizeF()));
        painter.setFont(f);

        const QFontMetrics fm(f);
        const QRect textRect = fm.boundingRect(text).adjusted(-6, -3, 6, 3);

        // 标签跟着左上角走，与角柄留一点距离
        const QPointF anchor = viewTransform.map(m_quad[TopLeft]);
        qreal lx = anchor.x() + hs + 6.0;
        qreal ly = anchor.y() - textRect.height() - 6.0;
        if (ly < 2.0)
            ly = anchor.y() + hs + 6.0;
        if (painter.device())
            lx = std::min(lx, qreal(painter.device()->width()) - textRect.width() - 4.0);
        lx = std::max(lx, 2.0);

        const QRect bgRect(int(lx), int(ly), textRect.width(), textRect.height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));
        painter.drawRoundedRect(bgRect, 3, 3);

        painter.setPen(QColor(255, 255, 255));
        painter.drawText(bgRect, Qt::AlignCenter, text);
    }

    painter.restore();
}

// ===========================================================================
//  生命周期
// ===========================================================================

void PerspectiveTool::setDocument(Document *doc)
{
    if (document() == doc)
        return;

    // 换文档 → 旧四角是旧画布的坐标，重新播种之前用不得
    m_quad = QPolygonF();
    m_seeded = false;
    m_dragCorner = -1;

    AbstractTool::setDocument(doc);
}

void PerspectiveTool::commit()
{
    // ★ 这里绝不能执行展平 ★
    //   ToolManager::setActiveTool 在"用户点了别的工具"时也会调 commit()，
    //   而那时用户并没有确认过。未确认的四边形直接丢掉 —— 与 CropTool 同一条语义。
    m_quad = QPolygonF();
    m_seeded = false;
    m_dragCorner = -1;

    AbstractTool::cancel();
    emit overlayChanged();
}

void PerspectiveTool::cancel()
{
    // Esc：文档从头到尾没被改过，所以"取消"只是把四角按当前画布重新播种。
    m_dragCorner = -1;
    AbstractTool::cancel();

    if (!isReady()) {
        emit overlayChanged();
        return;
    }

    m_seeded = false;
    ensureSeeded();
    emit overlayChanged();
}

} // namespace zl
