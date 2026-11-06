#pragma once

// ============================================================================
//  PerspectiveTool.h —— 透视校正（四角展平）工具
// ----------------------------------------------------------------------------
//  用途：手机斜着拍的 A4 纸。用户在画布上把四个角拖到纸的四角，
//  Enter（或双击）后把这块四边形拉正成一张新画布。
//
//  ★ 为什么是工具而不是对话框 ★
//    与 CropTool 同一条理由（docs/画布与文件操作设计.md 6.7 ①）：
//    操作数是**空间信息**（四个点），用数字输入框表达等于让用户在脑子里
//    做四次坐标换算。而且"把角拖准"这件事必须有**放大**能力 ——
//    1200 万像素的照片在适应窗口时只有 25%，一个角的误差在屏幕上
//    是 1 像素、在原图上就是 4 像素。对话框里的缩略预览给不了这个精度，
//    画布天生就有。
//
//  ★ 不自己压命令，发信号交回 MainWindow ★
//    展平会换画布尺寸，所有图层与选区都要一起换 —— 与裁剪/旋转走的是
//    同一条收尾（commitGeometryChange：快照 → 内存预检 → 压栈）。
//    这条收尾**只能有一个入口**，否则内存预检必然被漏掉一处。
//
//  ★ 拖动期间完全不碰 Document ★
//    四个角只存在 m_quad 里。好处与裁剪一样：Esc / 切工具取消是免费的，
//    也不需要为每次鼠标移动记一条撤销。
// ============================================================================

#include "tools/AbstractTool.h"

#include <QPointF>
#include <QPolygonF>
#include <QSize>

namespace zl {

class PerspectiveTool : public AbstractTool
{
    Q_OBJECT

public:
    // ★ 顶点顺序是硬约定 ★
    //   与 engine::scan::unwarp 的源四边形一一对应（左上 → 右上 → 右下 → 左下）。
    //   顺序错了展平出来会是镜像或交叉的图，而且 quadToQuad 不一定报错。
    enum Corner {
        TopLeft     = 0,
        TopRight    = 1,
        BottomRight = 2,
        BottomLeft  = 3
    };

    explicit PerspectiveTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~PerspectiveTool() override;

    // ── 身份 ──
    QString id() const override { return QStringLiteral("perspective"); }
    QString displayName() const override { return tr("Perspective Correction"); }
    int     shortcutKey() const override { return Qt::Key_P; }
    QCursor cursor() const override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;
    ToolResult mouseDoubleClick(const ToolEvent &e) override;
    ToolResult keyPress(const ToolEvent &e) override;

    // 四边形内部压暗（图像坐标）+ 边框线；角柄与尺寸标签（控件坐标）。
    void paintOverlay(QPainter &painter, CanvasView *view) override;

    // ── 文档切换 / 生命周期 ──
    void setDocument(Document *doc) override;
    void commit() override;
    void cancel() override;

    // ── 供自检使用的只读查询 ──
    QPolygonF currentQuad() const { return m_quad; }
    QSize     currentOutputSize() const;
    bool      isSeeded() const { return m_seeded; }

    // ── 纯函数（static，可在没有画布的环境下直接测）──
    //
    // 由画布矩形按比例内缩得到初始四角。
    //
    // ★ 为什么默认内缩 6% 而不是铺满画布 ★
    //   初始四角正好落在画布边缘时，四个角柄有一半在画布外，
    //   用户第一眼看到的是"四个抓不到的半边方块"。内缩一点，
    //   四个柄完整可见，也暗示"这四条边是可以拖的"。
    //
    // ★ 为什么内缩按**短边**算而不是按宽高各自算 ★
    //   按宽高各自算，宽图上左右内缩很多、上下内缩很少，
    //   初始四边形会是歪的（不是相似缩放），看起来像已经斜了。
    static QPolygonF seededQuad(const QRectF &bounds, qreal insetRatio = 0.06);

signals:
    // 请求主窗口执行展平。★ 本工具唯一的"动手"出口 ★
    void perspectiveRequested(const QPolygonF &quad, const QSize &outputSize);

private:
    QRectF canvasBounds() const;
    void   ensureSeeded();

    // 命中测试：返回角序号，没命中返回 -1。
    // zoom 用于把"角柄的屏幕尺寸"换算成图像坐标容差。
    int hitCorner(const QPointF &imagePos, qreal zoom) const;

    // 把角点钳制在画布内。★ 必须钳制 ★
    //   拖到画布外不会报错，但展平时源四边形有一部分在图像之外，
    //   那块区域会被采样成透明 —— 用户会以为"纸角被吃了"。
    QPointF clampToCanvas(const QPointF &p) const;

    void emitRequest();

    QPolygonF m_quad;
    bool      m_seeded = false;
    QSize     m_seedCanvasSize;
    int       m_dragCorner = -1;   // -1 表示没有在拖
};

} // namespace zl
