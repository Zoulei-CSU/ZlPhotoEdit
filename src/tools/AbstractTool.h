#pragma once

// ============================================================================
//  AbstractTool.h —— 工具基类（策略模式）
// ----------------------------------------------------------------------------
//  架构铁律：
//    ★ 工具**不直接修改像素数据**，只产生 QUndoCommand。
//      唯一的例外是"拖动过程中的实时预览" —— 那会临时写图层位图，
//      但在 mouseRelease 时必须把所有改动包装成一个 DrawCommand 压栈
//      （见 DrawCommand 的 mergeWith 机制）。
//
//  为什么用策略模式：
//    画布的输入处理代码（mousePressEvent 等）只调用 m_activeTool->xxx()，
//    对具体是画笔还是裁剪一无所知。新增工具 = 新增一个子类 + 在
//    ToolBoxPanel 里注册一行，不需要碰 CanvasView。
//
//  ★ 生命周期与所有权 ★
//    工具对象由 MainWindow（或其管理的 ToolManager）持有，CanvasView 只是
//    借用指针（setActiveTool 不接管所有权）。这样做的原因：工具需要在
//    CanvasView 之外也能访问（选项条要拿它的 options()、快捷键要激活它），
//    如果让 CanvasView 持有会形成所有权纠缠。
//
//  ★ Qt5 / Qt6 兼容 ★
//    工具接收的是本工程自有的 ToolEvent，而不是 QMouseEvent。
//    所有版本相关的 API 差异都已经在 CanvasView 里通过 compat 层收敛掉，
//    工具层因此可以做到零 #if。
// ============================================================================

#include "tools/ToolEvent.h"
#include "core/ImageTypes.h"

#include <QObject>
#include <QString>
#include <QCursor>
#include <QColor>
#include <QFont>
#include <Qt>
#include <QPointF>
#include <QRectF>

class QPainter;
class QIcon;

namespace zl {

class Document;
class CommandStack;
class CanvasView;
class ToolOptions;

class AbstractTool : public QObject
{
    Q_OBJECT

public:
    // ── 构造 ──
    //
    // doc 与 stack 都不持有所有权，且**都允许为 nullptr**。
    // 为什么允许 nullptr：工具在文档关闭的瞬间仍然存在（面板上还挂着），
    // 此时若有人误触发它的方法，能安全地什么都不做，而不是崩溃。
    // 所有工具的入口函数都应先调用 isReady() 检查。
    AbstractTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~AbstractTool() override;

    // ── 身份 ──

    // 工具的内部标识（英文，用于 QSettings 键名、快捷键映射）。
    // 必须唯一，且一旦确定不要再改 —— 改了用户的"上次使用的工具"就丢了。
    virtual QString id() const = 0;

    // 显示名（中文），用于工具提示与状态栏
    virtual QString displayName() const = 0;

    // 工具图标。默认返回一个空的 QIcon（工具面板会退化为显示文字）。
    virtual QIcon icon() const;

    // 画布上的鼠标光标。默认是十字光标。
    // 注意：只有那些"没有自定义光标预览"的工具才需要它 ——
    // 画笔这类工具会返回 Qt::BlankCursor，改用 paintCursor 画出笔刷轮廓。
    virtual QCursor cursor() const;

    // 快捷键（Qt::Key 值）。返回 0 表示没有快捷键。
    virtual int shortcutKey() const { return 0; }

    // ── 参数体系 ──
    //
    // 返回本工具的参数对象；返回 nullptr 表示该工具没有可配置参数
    // （例如抓手、缩放工具）。ToolOptionsBar 在工具切换时取出它并重建选项条。
    // 返回的是借用指针，所有权仍在工具手上。
    virtual ToolOptions *options() const { return nullptr; }

    // ── 文档切换 ──
    //
    // ★ 这是遵守"约定 7"的关键接口 ★
    //   所有面板与工具一律提供 setDocument()，而不是在构造时绑定文档。
    //   否则多标签化时全部要返工。
    //
    // 实现约定：子类重写时必须调用基类版本（基类负责清理正在进行的操作）。
    virtual void setDocument(Document *doc);
    virtual void setCommandStack(CommandStack *stack);

    // ── 颜色同步 ──
    //
    // 画布在"活动工具被切换"与"用户改了前景/背景色"这两个时机调用它们，
    // 把最新颜色**推**给工具。
    //
    // ★ 为什么是"推"而不是让工具自己去查 ★
    //   工具拿不到 CanvasView 的指针 —— tools 层不允许依赖 ui 层。
    //   为了取一个颜色就给工具加个 CanvasView* 成员，会逼着 tools 层
    //   include ui/CanvasView.h，分层立刻被破坏，而且是编译期不报错的
    //   隐性破坏。
    //
    //   反过来由画布推，方向就是 ui → tools，与既定的分层方向一致。
    //
    // 默认实现什么都不做：多数工具（移动、选区、裁剪、抓手）与颜色无关，
    // 只有画笔、油漆桶、直线、形状、文字这几个需要重写。
    virtual void setForegroundColor(const QColor &) {}
    virtual void setBackgroundColor(const QColor &) {}

    // ── 内嵌编辑框协议（文字工具用）──
    //
    //  ★ 为什么这条协议要放在基类，而不是让画布认识 TextTool ★
    //
    //    "在画布上打字"这件事天然分成两半：
    //      · **内容**（用户敲了什么）—— 属于模型，归工具；
    //      · **浮层**（一个能输入中文的 QTextEdit）—— 属于界面，归画布。
    //        QTextEdit 必须挂在画布上才能定位到"点击处"，而 tools 层
    //        不允许依赖 ui 层，所以浮层只能由画布自己创建。
    //
    //    两端由画布在中间牵线：画布拿到浮层的文本就推给活动工具。
    //    如果画布写成 `static_cast<TextTool*>(m_activeTool)`，ui → 具体工具
    //    的依赖就被焊死了 —— 将来多一个"矢量文字工具"，画布就得改。
    //    写成基类虚函数则回到既定的分层方向（ui → tools），
    //    与 setForegroundColor 的"推"是同一个套路。
    //
    //  默认实现什么都不做：绝大多数工具根本没有编辑框。
    virtual void setEditingText(const QString &text) { Q_UNUSED(text) }

    // 是否正在使用内嵌编辑框（画布据此决定 Esc / 换工具时要不要收浮层）。
    virtual bool hasInlineEditor() const { return false; }

    Document     *document() const { return m_doc; }
    CommandStack *commandStack() const { return m_stack; }

    // ── 输入事件 ──
    //
    // 返回 ToolResult 而不是 void：CanvasView 需要知道"这次事件工具消费了吗"。
    // 例如按住空格时 CanvasView 自己处理平移，工具不该收到事件；
    // 但有些工具（如移动工具在按住 Alt 时复制）会主动声明要处理。
    virtual ToolResult mousePress(const ToolEvent &e);
    virtual ToolResult mouseMove(const ToolEvent &e);
    virtual ToolResult mouseRelease(const ToolEvent &e);
    virtual ToolResult mouseDoubleClick(const ToolEvent &e);
    virtual ToolResult keyPress(const ToolEvent &e);
    virtual ToolResult keyRelease(const ToolEvent &e);

    // 悬停移动（没有按键按下）。默认忽略。
    // 只有需要"跟随鼠标画预览"的工具才重写它（画笔的轮廓圈、吸管的取样框）。
    virtual ToolResult hoverMove(const ToolEvent &e);

    // 视图变化（缩放/平移/旋转）。工具若缓存了"预览位置"需要在这里重算。
    virtual void viewChanged() {}

    // ── 绘制叠加层 ──
    //
    // ★ 调用时机是 CanvasView 呈现管线的第 8 步 ★
    //   此时 painter 已经 setTransform(m_viewTransform)，
    //   所以**直接使用图像坐标绘制**，不需要任何换算。
    //
    //   如果需要屏幕尺寸恒定的元素（如角柄），要在 resetTransform() 之后
    //   通过 paintScreenOverlay() 画 —— 这是约定 9。
    virtual void paintOverlay(QPainter &painter, CanvasView *view);

    // 屏幕坐标叠加层（在 painter.resetTransform() 之后调用）。
    // 适合画"角柄""尺寸标签""笔刷轮廓圆"这些需要固定像素尺寸的东西。
    virtual void paintScreenOverlay(QPainter &painter, CanvasView *view);

    // ── 操作生命周期 ──
    //
    // 工具被切换走 / 文档被关闭时调用。
    //   commit() —— 把未完成的操作提交掉（例如画笔正在画一笔，把它压栈）
    //   cancel() —— 丢弃未完成的操作并回滚到操作前的状态
    //
    // 两者的区别只在"用户按 Esc"与"用户点了别的工具"这两种场景下体现。
    virtual void commit();
    virtual void cancel();

    // 是否有正在进行的操作（用于判断"关闭文档时是否需要提示"）
    virtual bool isBusy() const { return m_busy; }

protected:
    // ── 给子类用的小工具 ──

    // 是否具备执行条件（文档与撤销栈都就绪）。
    // 子类的事件处理函数应当第一行就检查它。
    bool isReady() const;

    // 当前活动图层是否可写（存在、可见、未锁定）。
    // 不可写时应当拒绝操作并向状态栏发消息，而不是静默失败。
    bool canWriteToActiveLayer() const;

    // 发一条状态栏消息，并自动在 2 秒后清空。
    void showStatus(const QString &message, int timeoutMs = 2000);

    // 请求重绘整个画布（等价于 view->update()）。
    // 只修改叠加层状态时用。
    void requestRepaint();

    // 设置"正在操作"标志。子类在 mousePress 时置 true，release/cancel 时置 false。
    void setBusy(bool busy) { m_busy = busy; }

    // 上一次事件的图像坐标。多个工具需要它计算增量。
    QPointF lastImagePos() const { return m_lastImagePos; }
    void    setLastImagePos(const QPointF &p) { m_lastImagePos = p; }

    // 记录一次"拖动起点"，供 Shift 约束等使用
    QPointF dragStartPos() const { return m_dragStartPos; }
    void    setDragStartPos(const QPointF &p) { m_dragStartPos = p; }

    // 拖动累计的脏矩形（图像坐标，浮点）。
    // 用一个累加矩形而不是每步都查询，是为了能一次算出整笔的影响范围。
    QRectF  dragRect() const { return m_dragRect; }
    void    expandDragRect(const QRectF &r);
    void    resetDragRect();

private:
    Document     *m_doc   = nullptr;
    CommandStack *m_stack = nullptr;

    bool    m_busy = false;
    QPointF m_lastImagePos;
    QPointF m_dragStartPos;
    QRectF  m_dragRect;

signals:
    // 送到状态栏的消息
    void statusMessage(const QString &message, int timeoutMs);

    // 工具的"临时状态"发生变化，需要重绘画布（叠加层变化，数据未变）
    void overlayChanged();

    // 工具请求执行一个动作（例如吸管取色后要更新前景色）。
    // 用信号而不是直接调用 MainWindow，避免工具层反向依赖 ui 层。
    void requestForegroundColor(const QColor &color);
    void requestBackgroundColor(const QColor &color);

    // 工具需要画布做一个动作（例如缩放工具请求以某点为锚点缩放）。
    // CanvasView 连接它，把"视图操作"的控制权留在 ui 层。
    //
    // ★ 锚点是**控件坐标**，不是图像坐标 ★
    //   直觉上"以某个图像点为锚点缩放"更像几何语言，但真正必须保持不动的
    //   是**屏幕上的那一点**：缩放后用户还得在同一个屏幕位置看到同一个
    //   图像点。工具手里正好有按下时的 widgetPos（ToolEvent 同时给了控件与
    //   图像两套坐标），直接传控件坐标即可。传图像坐标的话画布还得再
    //   imageToWidget() 换算回去，绕一圈回到原点，中间任何一次精度损失都会
    //   让"连续点击缩放"的画面缓慢漂移。
    //   （这个参数原先叫 imageAnchor，与实现相反 —— 声明不诚实比没有声明更糟。）
    void requestZoom(qreal factor, const QPointF &anchorWidget);

    // 平移请求。delta 同样走**控件坐标**：m_offset 就是"图像原点的控件坐标"，
    // 与它同空间才能直接相加。所以抓手不会因为缩放比变化而"拖一点跑很远"。
    void requestPan(const QPointF &delta);

    // 工具请求"缩放到某个图像矩形"（缩放工具的框选缩放）。
    // 与 requestZoom 一样属于视图操作，由 CanvasView 实现
    // （它已有 zoomToRect()：把这个矩形铺满视口并留出边距）。
    //
    // ★ 这一个反而用**图像坐标** ★
    //   因为"要放大的那块内容"是文档里的一个区域，与它此刻显示在屏幕的
    //   哪里无关。用户换个缩放比再框同一块，应该得到同样的结果。
    void requestZoomToRect(const QRectF &imageRect);

    // 文字工具请求"在画布的某处打开输入浮层"。
    //
    // ★ 实参全是**内容**，没有一个是"控件怎么摆" ★
    //   imageAnchor 是锚点（图像坐标）—— 由画布用 imageToWidget() 换算成
    //   控件坐标，再决定浮层放哪、字号按当前缩放放大多少倍。
    //   工具不该知道"浮层有边框、字号要乘 zoom"这些界面细节，
    //   就像它不该知道 panBy 是怎么实现的。
    //
    //   font 已经是**最终字体**（像素字号 + 粗斜体 + 字距都设好了，
    //   见 TextOptions::toFont）。画布要用它显示"所见即所得"的输入效果。
    //
    //   lineHeight 是行距**倍数**。它必须一起传，不能只靠 font ——
    //   QFont 里没有行距这个概念，浮层不拿到它就只能用 100% 行距显示，
    //   于是"浮层里两行挨着、确定之后行距变成两倍"。凡是"浮层要与最终
    //   结果一致"的量，都得在请求里传全（漏一个就静默不一致）。
    void requestTextEditor(const QPointF &imageAnchor, const QFont &font,
                           const QColor &color, int align, qreal lineHeight);

public:
    // 让 ColorPickerTool 之类能发出取色信号（信号是 protected 的，
    // 子类可以 emit，但跨层调用需要公开一点）
    void emitStatusMessage(const QString &message, int timeoutMs = 2000)
    {
        emit statusMessage(message, timeoutMs);
    }
};

} // namespace zl
