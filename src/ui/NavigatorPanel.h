#pragma once

// NavigatorPanel.h —— 导航器面板：文档缩略图 + 当前可视区域框。

#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSizeF>
#include <QWidget>

class QTimer;

namespace zl {

class CanvasView;
class Document;

// 缩略图布局与坐标换算。不依赖控件，便于直接断言。
namespace navigator {

// 文档在面板内的缩略图矩形：等比缩放、居中、四周留 margin。
QRectF thumbnailRect(const QSizeF &docSize, const QRectF &panelRect,
                     qreal margin);

// 图像矩形换算到缩略图坐标（画可视框用）。不做裁剪。
QRectF imageRectToThumb(const QRectF &imageRect, const QRectF &thumbRect,
                        const QSizeF &docSize);

// 缩略图坐标换算回图像坐标（拖动定位用），结果夹在文档范围内。
QPointF thumbToImage(const QPointF &thumbPt, const QRectF &thumbRect,
                     const QSizeF &docSize);

} // namespace navigator

class NavigatorPanel : public QWidget
{
    Q_OBJECT

public:
    explicit NavigatorPanel(QWidget *parent = nullptr);

    // 文档决定缩略图内容，画布决定可视框与拖动，两者分开设。
    void setDocument(Document *doc);
    void setView(CanvasView *view);

    // 定义放在 .cpp：这里只有前向声明，而 QPointer 转成裸指针要求完整类型。
    Document   *document() const;
    CanvasView *view() const;

    // 缩略图刷新节流。内容变化很频繁（画笔每段都发），不节流会一直重合成。
    int  thumbnailThrottleMs() const;
    void setThumbnailThrottleMs(int ms);

    const QImage &thumbnail() const { return m_thumb; }
    QRectF thumbnailRect() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void scheduleThumbRebuild();
    void rebuildThumbnail();
    void centerOnImagePoint(const QPointF &imagePt);

    // 用 QPointer：关标签时画布与文档都是**直接 delete**，裸指针会悬空，
    // 而本面板的 paintEvent / 缩略图定时器还会碰到它们。
    QPointer<Document>   m_doc;
    QPointer<CanvasView> m_view;

    QImage  m_thumb;
    QTimer *m_thumbTimer = nullptr;

    bool    m_dragging = false;
    QPointF m_grabOffset;   // 按下点到可视框中心的偏移（图像坐标）
};

} // namespace zl
