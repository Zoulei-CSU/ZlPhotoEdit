#pragma once

// ============================================================================
//  ColorPickerTool.h —— 吸管（取色）
// ----------------------------------------------------------------------------
//  工程里最简单的工具之一：不写任何像素、不进撤销栈，只做一件事 ——
//  把鼠标位置的颜色读出来，发信号告诉上层"前景色变成这个了"。
//
//  ★ 为什么取色要取"合成图"而不是"活动图层" ★
//
//    用户看到的是合成结果。在活动图层上取色时，如果那一点是透明的，
//    取到的是 (0,0,0,0) —— 用户看到画面上明明是红色（下面那层），
//    却被设成了透明。这种"取到的颜色和看到的不一样"没有任何报错，
//    只是"手感不对"，而且极难解释。
//
//    所以默认取 Document::composite()，做到"所见即所得"。
//
//  ★ 为什么工具不直接改前景色 ★
//
//    tools/ 层不允许依赖 ui/ 层（那是架构铁律 5，编译期就会报错）。
//    前景色由 MainWindow 持有，所以工具只能发
//    AbstractTool::requestForegroundColor() 信号出去，
//    由 MainWindow 决定怎么处理（改颜色、同步给画笔与油漆桶的选项…）。
//
//  ★ 3×3 / 5×5 平均为什么在预乘空间求和 ★
//
//    位图是 ARGB32_Premultiplied，存的是 (R·A/255, G·A/255, B·A/255, A)。
//    要在"颜色的平均值"上得到正确结果，应当先把预乘分量求和（这等价于
//    按 alpha 加权），最后再除一次总 alpha 还原出颜色：
//
//        预乘分量的平均 = avg(R·A)/255
//        还原颜色 R = 预乘平均 × 255 / avg(A)
//
//    如果在非预乘空间逐像素平均，就必须对每个像素单独做一次除法
//    （而 A=0 时那个除法是无意义的，得额外特判）——
//    不但慢，还要写一堆边界分支。预乘空间的做法天然没有这个特例。
// ============================================================================

#include "tools/AbstractTool.h"

#include <QColor>
#include <QPointF>

namespace zl {

class PickerOptions;

class ColorPickerTool : public AbstractTool
{
    Q_OBJECT

public:
    ColorPickerTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~ColorPickerTool() override;

    // ── 身份 ──
    QString id() const override;
    QString displayName() const override;
    int     shortcutKey() const override;

    ToolOptions *options() const override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;
    ToolResult hoverMove(const ToolEvent &e) override;
    ToolResult keyPress(const ToolEvent &e) override;

    // ── 叠加层：取样范围的方框 ──
    // 与画笔轮廓圆同理，走 paintScreenOverlay（控件坐标），
    // 这样 3×3 的方框在 800% 缩放下不会变成一堵墙。
    void paintScreenOverlay(QPainter &painter, CanvasView *view) override;

    void viewChanged() override;

    // ── 给自检用的观测口 ──
    /// 最近一次取到的颜色（未取过时 isValid() 为 false）
    QColor lastPickedColor() const { return m_lastColor; }
    /// 最近一次取到的位置（图像坐标）
    QPoint lastPickPos() const { return m_lastPos; }
    /// 取色成功的次数（自检用它确认"拖动时会连续取色"）
    int pickCount() const { return m_pickCount; }

    /// 直接按图像坐标取一次色（不依赖事件）—— 自检与将来的"颜色范围"用
    QColor pickAt(const QPoint &imagePos) const;

private:
    void doPick(const ToolEvent &e);

    PickerOptions *m_options = nullptr;

    QColor  m_lastColor;
    QPoint  m_lastPos{ -1, -1 };
    int     m_pickCount = 0;
    bool    m_pressed   = false;

    // 悬停位置（控件坐标 + 缩放比），画取样框用。
    // 与 BrushTool 一样：工具层拿不到 CanvasView，只能靠事件里带的值。
    QPointF m_hoverWidgetPos;
    qreal   m_hoverViewZoom = 1.0;
    bool    m_hoverValid    = false;
    bool    m_hoverStale    = true;
};

} // namespace zl
