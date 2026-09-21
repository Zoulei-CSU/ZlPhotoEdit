#pragma once

// ============================================================================
//  CurveEditor.h —— 曲线控件（控制点的编辑与绘制）
// ----------------------------------------------------------------------------
//  设计依据：docs/曲线调整设计.md 第 4 节。
//
//  ★ 铁律：绘制曲线必须复用 engine 的烘焙 ★
//
//    本控件画的不是"自己算出来的另一条曲线"，而是把当前控制点交给
//    `Curve::bake()` 得到的那张 256 项 LUT，逐点连线。
//    只要绘制与实际映射共用同一份烘焙代码，"看到的曲线"和"实际效果"
//    就永远不可能不一致 —— 而这类不一致（画出来是 S 形、实际是直线）
//    恰恰是曲线功能最难排查的 bug。
//
//  ★ 内部坐标：整数 0..255 ★
//
//    engine 的 CurvePoint 用 qreal 且范围 [0,1]，因为它是"数学上的曲线定义"。
//    本控件内部一律用**整数曲线值 0..255**，因为：
//      · "相邻控制点 x 间隔 ≥1" 是整数语义的约束（防止插值除零）
//      · 命中检测的容差是像素，比较也应是整数
//    两套坐标只在 setCurve() / curve() 这一个边界上转换，
//    避免把"像素"这个概念泄漏进 engine。
//
//  ★ 关于 y 值的边界 ★
//
//    设计文档原本设想"允许把点拖出绘图区、烘焙时再 clamp"，
//    从而画出"压到底"（输出恒为 0）的曲线段。
//    本实现**把 y 限制在 0..255**，因为一旦允许超出，控件里画的曲线
//    与实际映射就会分叉，违反上面那条铁律。
//    需要"压到底"的平段时，把相邻两点都拖到底部即可 ——
//    Fritsch–Carlson 单调样条在两点等值之间不会过冲，正好得到平段，
//    这也是当初选它而不是 Catmull-Rom 的直接收益。
// ============================================================================

#include "engine/Curves.h"

#include <QColor>
#include <QPoint>
#include <QVector>
#include <QWidget>

namespace zl {

// 控件内的控制点（整数曲线值）
struct CurveNode
{
    int x = 0;
    int y = 0;

    CurveNode() = default;
    CurveNode(int ix, int iy) : x(ix), y(iy) {}
};

class CurveEditor : public QWidget
{
    Q_OBJECT

public:
    explicit CurveEditor(QWidget *parent = nullptr);

    // ── 曲线数据（与 engine 互转）──
    void  setCurve(const Curve &curve);
    Curve curve() const;

    // ── 显示选项 ──
    // 256 项的背景直方图，可为空（色阶/曲线共用同一套统计）。
    // ★ 调用方只需要在打开对话框时设一次 ★
    //   直方图要对整层遍历一遍（1200 万像素约 30ms），
    //   绝不能放在拖动路径上重算。
    void setHistogram(const QVector<int> &hist);

    // 平滑（单调三次样条）还是直折线。
    // 直折线不是"性能降级"，而是有些用户就想要硬折线的效果（做色阶分离），
    // 同时它也是验证样条实现的参照 —— 两者在控制点上必然重合。
    void setSmooth(bool on);
    bool isSmooth() const { return m_smooth; }

    void setCurveColor(const QColor &c);

    // ── 状态查询 ──
    bool isIdentity() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // 控制点变化（拖动过程中实时发出）→ 主窗口据此重算预览
    void curveChanged();

    // 一次编辑结束（松开鼠标 / 键盘微调结束）
    void editingFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // ── 坐标换算 ──
    // 注意 y 轴方向：控件坐标 y 向下增长，而亮度语义上"上 = 亮"，
    // 所以两个方向的换算必须各写一处 —— 不要在别处手写这个翻转，
    // 那是出现"曲线上下颠倒"这类 bug 的常见来源。
    QRect  plotRect() const;
    QPoint nodeToWidget(int x, int y) const;
    QPoint widgetToNode(const QPoint &w) const;   // 结果已 clamp 到 0..255

    int hitTest(const QPoint &w, int tolerancePx = 7) const;

    // ── 编辑操作 ──
    void ensureEndpoints();          // 保证存在 x=0 与 x=255 两个端点
    int  insertNode(const CurveNode &n);   // 返回新点索引；同 x 则替换 y
    bool removeNode(int index);
    bool moveNode(int index, const CurveNode &n);  // 内部做相邻约束
    void rebake();                   // 重算 m_bakedLut

    // ── 绘制 ──
    void drawGrid(QPainter &p);
    void drawHistogram(QPainter &p);
    void drawCurve(QPainter &p);
    void drawNodes(QPainter &p);
    void drawInfoBox(QPainter &p);

    // ── 状态 ──
    // 至少两个点（两端点），x 严格递增
    QVector<CurveNode> m_nodes;

    // 当前控制点烘焙出的 LUT。绘制与数值框都读它，
    // 保证"画出来的"与"查表用的"是同一张表。
    CurveLut m_bakedLut;

    QVector<int> m_histogram;        // 256 项，可为空

    int    m_selected   = -1;        // 键盘微调的目标（-1 = 无）
    bool   m_dragging   = false;
    int    m_hoverIndex = -1;
    bool   m_smooth     = true;
    QColor m_curveColor = QColor(40, 90, 200);

    QPoint m_cursorPos;
    bool   m_hasCursor = false;
};

} // namespace zl
