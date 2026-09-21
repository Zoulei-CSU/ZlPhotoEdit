#pragma once

// ============================================================================
//  HandTool.h —— 抓手（平移画布）
// ----------------------------------------------------------------------------
//  这是整个工程里最不像"工具"的工具：不碰一个像素、不进撤销栈、
//  连文档都可以没有。它唯一做的事，是把鼠标拖动换成画布平移。
//
//  ★ 为什么它仍然必须是一个工具，而不是 CanvasView 里的一段 if ★
//
//    "按住空格临时平移"已经在 CanvasView 里实现了。但两者是两种**进入方式**，
//    用户期望的是同一套手感。做成工具之后：
//      · 工具箱里有它的按钮、快捷键 H、光标是张开的手；
//      · 平移的**实现仍然只有 CanvasView::panBy() 一份** —— 工具只发位移。
//    如果直接在 CanvasView 里再写一段平移逻辑，两处的边界处理
//    （自动滚动、光标切换、要不要碰 m_userAdjustedView）迟早会分叉。
//
//  ★★ 位移是"控件坐标"，不是图像坐标 ★★
//
//    看着反直觉：用户拖的是图像内容，为什么不按图像坐标算？
//    因为要保证的是"鼠标走多远、画面就跟多远"。在 800% 缩放下，
//    1 个控件像素只对应 1/8 个图像像素 —— 按图像坐标算的话，
//    画面只挪鼠标位移的八分之一，手感立刻变成"怎么拖都不动"。
//
//    m_offset 本身就是"图像原点的**控件**坐标"，与控件位移同空间，
//    直接相加就是对的（见 CanvasView::panBy 的说明）。
//    所以抓手既不需要知道缩放比，也不会因为缩放而变化手感。
//
//  ★ 增量必须取自"上一次位置"，不是"按下位置" ★
//
//    取按下位置的话，每一次 mouseMove 都会把**整段**位移重复加一遍 ——
//    拖动会以平方级加速冲出屏幕。这是手写平移最经典的一个 bug，
//    而且症状（"一开始正常，越拖越快"）很容易被当成"性能问题"。
//    所以工具自己记 m_lastWidgetPos，每移动一次就把它更新掉。
//
//  ★ 不按 isReady() 拒绝 ★
//
//    AbstractTool::isReady() 只检查"有没有文档"。抓手不碰像素，
//    没有文档时拖动也只是让空白画布平移，没有任何副作用。
//    硬要拒绝反而会出现"文档全关掉之后抓手突然不能用了"的怪现象，
//    而且用户会以为程序卡死了。
// ============================================================================

#include "tools/AbstractTool.h"

#include <QPointF>

namespace zl {

class HandTool : public AbstractTool
{
    Q_OBJECT

public:
    HandTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~HandTool() override;

    // ── 身份 ──
    QString id() const override;
    QString displayName() const override;
    int     shortcutKey() const override;

    // 张开的手（空闲）／握紧的手（拖动中）。
    // ★ 拖动中的光标能不能生效，取决于 CanvasView 有没有在按下之后
    //   重新问一次 —— 见 CanvasView::mousePressEvent 里那句 refreshCursor()。
    QCursor cursor() const override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;

    // 切工具 / 关文档时调用：把拖动状态清干净，绝不留下"半途抓住"的状态。
    void cancel() override;

    // ── 给自检用的观测口 ──
    /// 是否正在拖动
    bool isDragging() const { return m_dragging; }
    /// 最近一次发出的平移位移（控件坐标）
    QPointF lastPanDelta() const { return m_lastDelta; }
    /// 一共发出过多少次非零平移（自检用它确认"每个 move 都会平移一次"）
    int panCount() const { return m_panCount; }

private:
    bool    m_dragging = false;
    QPointF m_lastWidgetPos;      // 上一次的控件坐标（进增量用）
    QPointF m_lastDelta;          // 观测用
    int     m_panCount = 0;
};

} // namespace zl
