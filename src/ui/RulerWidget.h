#pragma once

// ============================================================================
//  RulerWidget.h —— 标尺控件（上边 / 左边各一个实例）
// ----------------------------------------------------------------------------
//  职责只有两件：**画刻度** 与 **从尺子上拖出一根参考线**。
//  它不存参考线（参考线归 CanvasView，那是每页一份的视图状态），
//  也不改像素 —— 与"面板只读文档 + 只发信号"是同一条纪律。
//
//  ★★ 为什么刻度换算要"问画布"而不是自己记一份缩放 ★★
//
//    标尺与画布必须**严丝合缝**：尺子上写着 200 的那一格，必须正对着画布上
//    图像坐标 200 的那一列。做到这件事只有一个可靠办法 —— 用画布的
//    imageToWidget()/widgetToImage() 这一对**同一个变换**去换算，
//    而不是在标尺里按 zoom/offset 各算一遍（那就会有两份真相，
//    迟早出现"刻度整体偏几像素"这种没人能一眼看出的错）。
//
//  ★★ 为什么原点要靠**全局坐标**求，而不是假设"尺子左上角就是画布左上角" ★★
//
//    中央控件是"拐角 + 上标尺 / 左标尺 + QTabWidget"的网格，
//    而**标签栏在页面之上** —— 画布的真正 (0,0) 比标尺低了整整一个标签栏
//    的高度。写死偏移 = 换一种标签位置/加一圈边框就整体错位。
//    换算出来能把这个差值自动吃掉，不需要任何人去记它。
//
//    ★ 换算必须走 mapToGlobal + mapFromGlobal，**不能写 m_view->mapTo(this) ★**
//      标尺与画布是**兄弟**（都挂在中央网格宿主上，画布在标签容器里）。
//      而 QWidget::mapTo(parent, pos) 是从自己**沿父链往上找** parent ——
//      参数必须是自己**真正的祖先**。传一个兄弟进去，父链走到头变成
//      nullptr，紧接着 w->mapToParent(p) 直接解引用空指针 → 进程当场段错误。
//      症状很坑：MainWindow 构造完全正常，**一 show() 就崩**（第一次重绘才走到）。
//      走全局坐标中转与层级无关，兄弟、堂兄弟都成立。
//
//  ★★ 上边的尺子拖出来的是**水平**参考线 ★★
//
//    容易写反：从顶部尺子往下拖，得到的是"一条横线"，它的位置是 **y**。
//    也就是说 —— 参考线的方向 == 尺子的方向，但**位置取自垂直方向的位移**。
//    本类因此在拖拽时读的是"垂直于自己那一轴"的坐标（见 perpendicularImagePos）。
// ============================================================================

#include <QWidget>

#include "engine/RulerMetrics.h"

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

namespace zl {

class CanvasView;

class RulerWidget : public QWidget
{
    Q_OBJECT

public:
    // 尺子厚度（控件像素）。拐角小方块与另一把尺子都按它对齐。
    static constexpr int kThickness = 18;

    explicit RulerWidget(Qt::Orientation orientation, QWidget *parent = nullptr);

    // 重指当前画布。多标签切页时由 MainWindow 调用。
    // ★ 必须真的 disconnect 旧画布 ★ 否则切过的那一页缩放一下，
    //   已经不属于它的标尺跟着重绘（看不见，但白费，且可能读到旧矩阵）。
    void        setView(CanvasView *view);
    CanvasView *view() const { return m_view; }

    Qt::Orientation orientation() const { return m_orientation; }

    void setUnit(rulermetrics::Unit unit);
    rulermetrics::Unit unit() const { return m_unit; }

    // 物理单位（cm/mm/inch/pt）的换算基准。像素单位不受影响。
    void setDpi(int dpi);
    int  dpi() const { return m_dpi; }

    QSize sizeHint() const override;

signals:
    // 拖拽进行中。
    //   orientation —— **参考线的方向**（等于本尺子的方向）
    //   posImage    —— 参考线在图像坐标下的位置（水平线给 y，竖直线给 x）
    void guideDragMoved(Qt::Orientation orientation, int posImage);

    // 松手。commit=false 表示松在尺子自己身上（= 放弃这根线）。
    void guideDragFinished(Qt::Orientation orientation, bool commit);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // 画布原点在本控件坐标系里的偏移（水平尺只用到 .x()，垂直尺只用到 .y()）。
    QPoint  canvasOrigin() const;

    // 把"本控件坐标里垂直于自己那一轴的位置"换算成图像坐标。
    // 水平尺用 pos.y()（决定水平参考线落在图像的哪一行）。
    int     perpendicularImagePos(const QPoint &posInWidget) const;

    // 光标指示器（画布光标投在本尺子上的位置，图像坐标，沿尺子那一轴）。
    int     cursorImagePosAlongAxis() const;

    Qt::Orientation    m_orientation;
    CanvasView        *m_view = nullptr;
    rulermetrics::Unit m_unit = rulermetrics::Unit::Pixel;
    int                m_dpi  = 96;

    // 拖拽状态。只要按下就是拖拽中 —— 标尺上没有"可抓的东西"，
    // 按下必然是"拖出一根新参考线"（移动/删除都在画布侧，见 CanvasView）。
    bool m_dragging = false;
    int  m_lastPosImage = 0;

    // 画布光标位置（图像坐标）。view->cursorMoved 推过来。
    int  m_cursorImagePos = 0;
    bool m_hasCursor = false;
};

} // namespace zl
