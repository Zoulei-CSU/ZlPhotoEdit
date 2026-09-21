#pragma once

// ============================================================================
//  ToolOptionsBar.h —— 工具选项条
// ----------------------------------------------------------------------------
//  位置：主工具栏下方，横贯窗口宽度的一条。
//  内容：由当前工具的 ToolOptions::createWidget() 生成。
//
//  ★ 为什么不用 QDockWidget ★
//
//    选项条需要"横贯整个窗口宽度"且"不能浮动、不能关闭"。
//    QDockWidget 天生支持浮动与关闭，要禁用它们需要写一堆属性；
//    而且 Dock 的尺寸行为与"一条 30 像素高的条"不匹配 ——
//    Dock 的最小高度会把主窗口的中央区域挤变形。
//
//    QToolBar 才是对的：它天然是"一条"，可以 addWidget()，
//    且不浮动。本类被放进一个专用的 QToolBar 里。
//
//  ★ 为什么本类不是 QToolBar 的子类 ★
//
//    如果继承 QToolBar，那么"选项条的容器"与"选项条的布局"
//    就耦合在一起了。测试时要构造一个完整的 QToolBar 才能验证
//    "切换工具是否正确重建内容"。
//    做成普通 QWidget 后，它可以被放进 QToolBar、QDockWidget、
//    甚至对话框里，容器选择完全自由。
// ============================================================================

#include <QWidget>
#include <QString>

class QHBoxLayout;

namespace zl {

class AbstractTool;

class ToolOptionsBar : public QWidget
{
    Q_OBJECT

public:
    explicit ToolOptionsBar(QWidget *parent = nullptr);
    ~ToolOptionsBar() override;

public slots:
    // 切换当前工具，重建选项条内容。
    // 传 nullptr 表示没有活动工具（显示一句提示而不是留空）。
    //
    // ★ 本函数可以被反复调用（每次工具切换都会调）★
    //   实现里必须彻底清理旧内容，否则新旧控件的信号会同时生效 ——
    //   表现为"改了画笔的大小，形状工具的线宽也跟着变"。
    void setTool(AbstractTool *tool);

private:
    QHBoxLayout *m_layout = nullptr;
    AbstractTool *m_currentTool = nullptr;
};

} // namespace zl
