#pragma once

// ============================================================================
//  ToolEvent.h —— 工具事件的统一表示
// ----------------------------------------------------------------------------
//  为什么不让工具直接接收 QMouseEvent？
//
//  三个理由：
//
//  1. **坐标系解耦**
//     工具需要的是**图像坐标**，而 Qt 的事件给的是控件坐标。
//     如果每个工具自己调 canvas->widgetToImage()，那么：
//       · 每个工具的 mouseMove 里都要写一遍换算
//       · 视图矩阵化改造时（zoom+offset → QTransform）要改所有工具
//     统一在这里换算好，工具只拿到图像坐标，对视图实现完全无感。
//
//  2. **Qt5/Qt6 差异的收敛点**
//     QMouseEvent::localPos() 在 Qt6 被删除了。如果工具直接用 QMouseEvent，
//     那么每个工具的每个事件处理函数里都要写 #if QT_VERSION。
//     在这个文件里换算一次，所有工具都干净了。
//
//  3. **可测试性**
//     工具的交互逻辑可以脱离 QWidget 测试 —— 构造一个 ToolEvent
//     直接喂给工具，不需要创建真实的窗口与模拟事件。
//     画笔的"两点间补点"逻辑、选区的"Shift 约束正方形"逻辑
//     都能用这种方式做单元测试。
//
//  ★ 事件里同时提供控件坐标与图像坐标 ★
//     大部分工具用图像坐标，但有些需要控件坐标 ——
//     例如"笔刷轮廓的预览圈"要在屏幕上保持固定尺寸，
//     计算它的大小时需要知道当前缩放比。所以两者都给。
// ============================================================================

#include <QPointF>
#include <QPoint>
#include <Qt>
#include <QString>

namespace zl {

// ---------------------------------------------------------------------------
//  事件类型
// ---------------------------------------------------------------------------
enum class ToolEventType {
    Press,          // 鼠标按下
    Move,           // 鼠标移动（含拖动）
    Release,        // 鼠标释放
    DoubleClick,    // 双击
    // 键盘事件也走同一套机制，因为文字工具、选区工具都需要它
    KeyPress,
    KeyRelease,
    // 视口变化（缩放/平移）—— 工具可能需要重新计算预览位置
    ViewChanged
};

// ---------------------------------------------------------------------------
//  修饰键（从 Qt 的枚举里剥离出来，避免工具直接依赖 Qt::KeyboardModifiers）
// ---------------------------------------------------------------------------
struct ToolModifiers
{
    bool shift = false;
    bool ctrl  = false;
    bool alt   = false;
    bool meta  = false;

    static ToolModifiers fromQt(Qt::KeyboardModifiers mods)
    {
        ToolModifiers m;
        m.shift = mods.testFlag(Qt::ShiftModifier);
        m.ctrl  = mods.testFlag(Qt::ControlModifier);
        m.alt   = mods.testFlag(Qt::AltModifier);
        m.meta  = mods.testFlag(Qt::MetaModifier);
        return m;
    }
};

// ---------------------------------------------------------------------------
//  工具事件
// ---------------------------------------------------------------------------
struct ToolEvent
{
    ToolEventType type = ToolEventType::Move;

    // ── 坐标 ──
    //
    // widgetPos —— 控件坐标（浮点，保留亚像素精度）
    // imagePos  —— 图像坐标（浮点）
    //
    // ★ 为什么用浮点而不是整数 ★
    //   在 25% 缩放下手动移动 1 个控件像素，对应图像坐标里的 4 个像素。
    //   如果一开始就取整，画笔的轨迹会有明显的台阶。用浮点保留精度，
    //   在真正落笔时才取整，线条会平滑得多。
    //
    //   另一个场景是图像放大到 800% 时：一个图像像素等于 8 个控件像素，
    //   此时控件坐标的取整误差会直接变成"点不准"。
    QPointF widgetPos;
    QPointF imagePos;

    // 上一次的位置（用于计算增量）。工具在做"相对位移"操作时需要它。
    QPointF lastImagePos;

    // ── 按钮与修饰键 ──
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    ToolModifiers modifiers;

    // ── 键盘事件（type 为 KeyPress/KeyRelease 时有效）──
    int     key = 0;              // Qt::Key_xxx
    QString text;                 // 输入的文本（文字工具用）

    // ── 滚轮（可选）──
    // 大部分工具忽略它（滚轮用于缩放），但形状工具可能用它调整线宽。
    int     wheelDelta = 0;

    // ── 视口信息 ──
    // 工具计算"屏幕尺寸恒定的预览"时需要（例如笔刷轮廓圈的半径）
    qreal   viewZoom = 1.0;

    // ── 便捷判断 ──
    bool isPress()   const { return type == ToolEventType::Press; }
    bool isMove()    const { return type == ToolEventType::Move; }
    bool isRelease() const { return type == ToolEventType::Release; }
    bool isDoubleClick() const { return type == ToolEventType::DoubleClick; }

    // 是否为"拖动中"的移动（有按钮按下）
    bool isDragging() const
    {
        return type == ToolEventType::Move && buttons != Qt::NoButton;
    }

    // 图像坐标取整后的整数版本。工具真正写像素时用这个。
    QPoint imagePoint() const
    {
        return QPoint(int(qRound(imagePos.x())), int(qRound(imagePos.y())));
    }

    QPoint lastImagePoint() const
    {
        return QPoint(int(qRound(lastImagePos.x())), int(qRound(lastImagePos.y())));
    }
};

// ---------------------------------------------------------------------------
//  工具的处理结果
// ---------------------------------------------------------------------------
//  工具通过返回值告诉 CanvasView "这次事件我处理了吗"，
//  以及"要不要重绘"。
//
//  为什么需要返回"是否处理"：CanvasView 在工具不处理某个事件时
//  可以执行默认行为（例如空格+拖动由 CanvasView 自己处理平移）。
struct ToolResult
{
    bool handled = true;         // 事件是否被工具消费
    bool needsRepaint = false;   // 是否需要重绘画布（工具修改了预览状态）
    bool needsCommit = false;    // 是否已向撤销栈压入了命令

    static ToolResult accepted()   { return { true, false, false }; }
    static ToolResult ignored()    { return { false, false, false }; }
    static ToolResult repaint()    { return { true, true, false }; }
    static ToolResult committed()  { return { true, true, true }; }
};

} // namespace zl
