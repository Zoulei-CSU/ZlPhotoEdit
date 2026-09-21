#pragma once

// ============================================================================
//  LevelsWidget.h —— 色阶控件（输入直方图 + 五枚可拖动手柄）
// ----------------------------------------------------------------------------
//  界面自上而下分四层：
//
//     ┌──────────────────────────────────────────┐
//     │                                          │
//     │        输入直方图（对数缩放）              │  ← inputPlotRect()
//     │                                          │
//     ├──▲───────────▲──────────────────▲────────┤  ← inHandleRow()
//     │ 黑场       灰点(γ)             白场       │
//     ├──────────────────────────────────────────┤
//     │  ███████████ 输出预览渐变条 ███████████    │  ← outBarRect()
//     ├──▲──────────────────────────────▲────────┤  ← outHandleRow()
//     │ 输出黑场                     输出白场      │
//     └──────────────────────────────────────────┘
//
//  ★ 铁律：灰点手柄的位置由 engine 的烘焙结果反查，不另算一遍 ★
//
//    灰点（gamma）手柄的横坐标不是"照着公式另算一次"，而是把当前参数
//    交给 `adjust::levelsLut()` 烘焙出 256 项表，再查"输出首次达到 128
//    的那个输入值"。这样"手柄画在哪"与"像素实际怎样映射"永远出自同一份
//    代码 —— 与 CurveEditor 用 m_bakedLut 画曲线是同一条铁律。
//    一旦在 UI 里自己推公式，就会出现"界面上看着是 A、实际结果是 B"。
//
//  ★ 灰点手柄拖不到最右端，这是正确行为 ★
//
//    gamma ∈ [0.1, 10] 对应灰点落在黑白场之间的比例约 [0.1%, 93.3%] 处。
//    拖到 93% 就停住不是 bug —— 再往右需要 gamma < 0.1，超出引擎钳制范围。
//    用户若真要"把暗部整段压到底"，应该拖黑场/白场，那才是语义正确的操作。
//
//  ★ 手柄的 x 取值范围与直方图严格一致 ★
//
//    两者共用同一对 levelToX()/xToLevel()。若各处手写映射，
//    拖到黑场时手柄与直方图的 0 位置对不上，用户会以为参数不准。
// ============================================================================

#include "engine/Adjust.h"

#include <QVector>
#include <QWidget>

namespace zl {

class LevelsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LevelsWidget(QWidget *parent = nullptr);

    // ── 参数 ──
    // 直接用 engine 的 LevelsParams，不另立一套结构 ——
    // 两套结构迟早会因为某个字段的默认值不同而对不上。
    void setParams(const adjust::LevelsParams &p);
    adjust::LevelsParams params() const { return m_params; }

    // 256 项输入直方图，可为空（空时只画空框，不画柱形）。
    // ★ 调用方负责只算一次 ★ 对整层遍历一遍约 30ms（1200 万像素），
    // 绝不能放在拖动路径上重算。
    void setHistogram(const QVector<int> &hist);

    // 参数是否等于"不变"（输入 0/255、gamma 1、输出 0/255）
    bool isIdentity() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // 拖动中实时发出 → 对话框据此请求重算预览
    void levelsChanged();

    // 一次编辑结束（松开鼠标 / 键盘微调结束）
    void editingFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    // 五枚手柄。None 用于"没命中任何一枚"。
    enum class Handle { None, InBlack, InGray, InWhite, OutBlack, OutWhite };

    // ── 几何 ──
    // 四行的高度按固定比例分配（见 metrics()），
    // 这样控件被拉高拉矮时都不会有某一行跑出边界。
    QRect inputPlotRect() const;
    QRect inHandleRow()  const;
    QRect outBarRect()   const;
    QRect outHandleRow() const;

    QRect handleRect(Handle h) const;

    // 色阶值(0..255) ⇄ 控件 x。两个方向各写一处 ——
    // 在别处手写是"手柄与直方图对不齐"这类问题的常见来源。
    int levelToX(int level, const QRect &row) const;
    int xToLevel(int x, const QRect &row) const;

    // 灰点手柄的横坐标（由烘焙表反查，见文件头说明）
    int  grayHandleX() const;

    // 依据目标横坐标反解 gamma 并写回参数
    void setGrayFromX(int x);

    // 把某枚手柄移到鼠标所指的位置（按下与拖动共用这一段）。
    // 会按手柄类型自动处理黑白场的先后约束。
    void moveHandleTo(Handle h, const QPoint &pos);

    Handle hitTest(const QPoint &pos) const;

    // ── 绘制 ──
    void drawHistogram(QPainter &p);
    void drawOutputBar(QPainter &p) const;
    void drawHandle(QPainter &p, Handle h) const;
    void drawSeparator(QPainter &p) const;

    // ── 参数写入的统一出口 ──
    // 所有改动都经这里，保证 update() 与信号只发一次，
    // 也保证 m_grayDragLevel 在参数被外部改动后一定失效。
    void applyParams(const adjust::LevelsParams &p);

    // ── 状态 ──
    adjust::LevelsParams m_params;
    QVector<int>         m_histogram;

    Handle m_hover    = Handle::None;
    Handle m_dragging = Handle::None;
    // 键盘微调的目标（最后一次点中的手柄）
    Handle m_selected = Handle::None;

    // 拖动灰点时的目标色阶值（0..255），-1 表示"不在拖动灰点"。
    // ★ 为什么需要它 ★
    //   反解出的 gamma 会被钳制到 [0.1, 10]，若绘制时再"由 gamma 正查
    //   位置"，钳制边缘会让手柄往回跳一格，手感像是卡住不动。
    //   拖动期间直接用目标值定位，松手后再切回烘焙反查。
    int m_grayDragLevel = -1;
};

} // namespace zl
