#include "ui/NavigatorPanel.h"

#include "core/Document.h"
#include "ui/CanvasView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

namespace zl {

namespace {

const int    kThrottleMs = 300;
const qreal  kMargin     = 8.0;
const qreal  kMinSide    = 1.0;   // 小于这个尺寸的矩形不画也不换算

} // namespace

namespace navigator {

QRectF thumbnailRect(const QSizeF &docSize, const QRectF &panelRect,
                     qreal margin)
{
    if (docSize.width() <= 0.0 || docSize.height() <= 0.0)
        return QRectF();

    const QRectF inner = panelRect.adjusted(margin, margin, -margin, -margin);
    if (inner.width() < kMinSide || inner.height() < kMinSide)
        return QRectF();

    const qreal scale = qMin(inner.width() / docSize.width(),
                             inner.height() / docSize.height());
    const QSizeF size(docSize.width() * scale, docSize.height() * scale);

    // 以 inner 的中心对齐，而不是左上角：面板宽高比与文档不一致时，
    // 缩略图才能落在面板正中。
    const QPointF center = inner.center();
    return QRectF(center.x() - size.width() / 2.0,
                  center.y() - size.height() / 2.0,
                  size.width(), size.height());
}

QRectF imageRectToThumb(const QRectF &imageRect, const QRectF &thumbRect,
                        const QSizeF &docSize)
{
    if (docSize.width() <= 0.0 || docSize.height() <= 0.0)
        return QRectF();
    if (thumbRect.width() < kMinSide || thumbRect.height() < kMinSide)
        return QRectF();

    const qreal sx = thumbRect.width() / docSize.width();
    const qreal sy = thumbRect.height() / docSize.height();

    return QRectF(thumbRect.left() + imageRect.left() * sx,
                  thumbRect.top() + imageRect.top() * sy,
                  imageRect.width() * sx,
                  imageRect.height() * sy);
}

QPointF thumbToImage(const QPointF &thumbPt, const QRectF &thumbRect,
                     const QSizeF &docSize)
{
    if (docSize.width() <= 0.0 || docSize.height() <= 0.0)
        return QPointF();
    if (thumbRect.width() < kMinSide || thumbRect.height() < kMinSide)
        return QPointF();

    const qreal x = (thumbPt.x() - thumbRect.left()) / thumbRect.width()
                    * docSize.width();
    const qreal y = (thumbPt.y() - thumbRect.top()) / thumbRect.height()
                    * docSize.height();

    return QPointF(qBound(0.0, x, docSize.width()),
                   qBound(0.0, y, docSize.height()));
}

} // namespace navigator

NavigatorPanel::NavigatorPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("navigatorPanel"));
    setMinimumSize(120, 90);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    m_thumbTimer->setInterval(kThrottleMs);
    connect(m_thumbTimer, &QTimer::timeout,
            this, &NavigatorPanel::rebuildThumbnail);
}

void NavigatorPanel::setDocument(Document *doc)
{
    if (m_doc == doc)
        return;

    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);

    m_doc = doc;
    m_thumb = QImage();

    if (m_doc) {
        connect(m_doc, &Document::contentChanged,
                this, &NavigatorPanel::scheduleThumbRebuild);
        connect(m_doc, &Document::structureChanged,
                this, &NavigatorPanel::scheduleThumbRebuild);
        connect(m_doc, &Document::canvasSizeChanged,
                this, &NavigatorPanel::scheduleThumbRebuild);
    }

    // 切页要立刻看到新文档，不等节流。
    rebuildThumbnail();
}

void NavigatorPanel::setView(CanvasView *view)
{
    if (m_view == view)
        return;

    if (m_view)
        disconnect(m_view, nullptr, this, nullptr);

    m_view = view;

    if (m_view) {
        connect(m_view, &CanvasView::viewChanged, this, [this]() { update(); });
    }

    update();
}

int NavigatorPanel::thumbnailThrottleMs() const
{
    return m_thumbTimer ? m_thumbTimer->interval() : -1;
}

void NavigatorPanel::setThumbnailThrottleMs(int ms)
{
    if (!m_thumbTimer)
        return;

    // 与图层缩略图同样的处理：运行中改 interval 不影响已排定的触发，先停再设。
    m_thumbTimer->stop();
    m_thumbTimer->setInterval(qMax(0, ms));
}

Document *NavigatorPanel::document() const
{
    return m_doc.data();
}

CanvasView *NavigatorPanel::view() const
{
    return m_view.data();
}

QRectF NavigatorPanel::thumbnailRect() const
{
    if (!m_doc)
        return QRectF();

    return navigator::thumbnailRect(QSizeF(m_doc->canvasSize()),
                                    QRectF(rect()), kMargin);
}

QSize NavigatorPanel::sizeHint() const
{
    return QSize(240, 160);
}

QSize NavigatorPanel::minimumSizeHint() const
{
    return QSize(120, 90);
}

void NavigatorPanel::scheduleThumbRebuild()
{
    if (!m_thumbTimer)
        return;

    // 只在空闲时启动，不能改成 restart：连续落笔时 restart 会让计时器
    // 一直不到期，缩略图在停笔前完全不刷新。
    if (!m_thumbTimer->isActive())
        m_thumbTimer->start();
}

void NavigatorPanel::rebuildThumbnail()
{
    m_thumb = QImage();

    if (m_doc) {
        const QRectF target = thumbnailRect();
        if (target.width() >= kMinSide && target.height() >= kMinSide) {
            const QImage composite = m_doc->composite();
            if (!composite.isNull()) {
                m_thumb = composite.scaled(
                    QSize(qRound(target.width()), qRound(target.height())),
                    Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
    }

    update();
}

void NavigatorPanel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));

    if (!m_doc) {
        painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        painter.drawText(rect(), Qt::AlignCenter, tr("No document"));
        return;
    }

    const QRectF thumbBox = thumbnailRect();
    if (thumbBox.isEmpty())
        return;

    if (!m_thumb.isNull())
        painter.drawImage(thumbBox, m_thumb);
    else
        painter.fillRect(thumbBox, QColor(0, 0, 0, 24));

    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.drawRect(thumbBox);

    if (m_view) {
        const QSizeF docSize(m_doc->canvasSize());
        const QRectF box = navigator::imageRectToThumb(
            m_view->visibleImageRect(), thumbBox, docSize);
        // 视图可能看到文档以外（缩小后四周留白），框要裁到缩略图范围内，
        // 否则会画到面板边缘。
        const QRectF visible = box.intersected(thumbBox);

        if (visible.width() >= kMinSide && visible.height() >= kMinSide) {
            painter.fillRect(visible, QColor(255, 255, 255, 40));
            painter.setPen(QPen(QColor(255, 255, 255, 210), 1));
            painter.drawRect(visible);
        }
    }
}

void NavigatorPanel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_doc || !m_view) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QRectF thumbBox = thumbnailRect();
    if (!thumbBox.contains(QPointF(event->pos()))) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPointF imagePt = navigator::thumbToImage(
        QPointF(event->pos()), thumbBox, QSizeF(m_doc->canvasSize()));
    const QRectF visible = m_view->visibleImageRect();

    // 点在可视框内：保留"按下点相对框中心"的偏移，拖动时框跟手走，
    // 不会一按下就把画面吸到正中间。点在框外：直接跳过去。
    m_grabOffset = visible.contains(imagePt) ? imagePt - visible.center()
                                             : QPointF();
    m_dragging = true;
    centerOnImagePoint(imagePt - m_grabOffset);
}

void NavigatorPanel::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !m_doc || !m_view) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QRectF thumbBox = thumbnailRect();
    if (thumbBox.isEmpty())
        return;

    // 拖出缩略图范围时坐标会被夹到文档内，相当于拖到边缘就停住。
    const QPointF imagePt = navigator::thumbToImage(
        QPointF(event->pos()), thumbBox, QSizeF(m_doc->canvasSize()));
    centerOnImagePoint(imagePt - m_grabOffset);
}

void NavigatorPanel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        m_dragging = false;

    QWidget::mouseReleaseEvent(event);
}

void NavigatorPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    // 面板尺寸变了，缩略图要按新尺寸重算。
    scheduleThumbRebuild();
    update();
}

void NavigatorPanel::centerOnImagePoint(const QPointF &imagePt)
{
    if (!m_view)
        return;

    // 平移量在控件坐标里算，旋转/缩放都不用特殊处理。
    const QPointF current = m_view->imageToWidget(imagePt);
    const QPointF center(m_view->width() / 2.0, m_view->height() / 2.0);
    m_view->panBy(center - current);
}

} // namespace zl
