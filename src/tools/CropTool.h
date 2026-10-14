#pragma once

// ============================================================================
//  CropTool.h —— 裁剪工具
// ----------------------------------------------------------------------------
//  交互：在画布上框一个矩形，确认后把画布裁成那个矩形。
//
//  ★★ 四个关键设计决定（详见 docs/画布与文件操作设计.md 6.7）★★
//
//  ① **不做对话框**
//     图像大小 / 画布大小 / 任意角度旋转的操作数是**标量**，可以用输入框表达；
//     裁剪的操作数是**矩形** —— 那是空间信息，用数字表达要用户在脑子里做
//     两次坐标换算。所以裁剪的参数只剩 CropOptions 里的比例与网格两项。
//
//  ② **拖动期间完全不碰 Document**
//     与选区工具同一条理由：拖动每秒发 60~120 次事件，每次都改数据就要为
//     每次移动都记一条撤销。裁剪的"预览"只需要一个 QRectF 成员，
//     框外的半透明遮罩就是预览本身（告诉用户"这些会被丢掉"）。
//     附带好处：Esc / 切换工具时的取消是免费的 —— 丢掉那个矩形即可。
//
//  ③ **不自己压命令，发 cropRequested() 交回 MainWindow**
//     几何类命令（尺寸 / 旋转 / 翻转 / 裁剪）的内存预检只能有**一个入口**
//     —— 见 MainWindow.h 里 commitGeometryChange() 的注释。工具自己 new 一个
//     GeometryChangeCommand 就等于开了第二个入口，而"漏掉预检"的代价
//     （几百 MB 的快照占住内存）要到内存耗尽时才暴露。
//     方向上也必须如此：tools → ui 只能靠信号（同 requestForegroundColor /
//     requestZoom），反过来 include ui/MainWindow.h 是编译期就会报错的越界。
//
//  ④ **叠加层的屏幕尺寸恒定，但靠 painter 自己的变换取缩放**
//     角柄与尺寸标签必须保持固定的**屏幕**像素尺寸（约定 9）。而 tools/ 层
//     不允许 include ui/CanvasView.h（架构铁律 5），拿不到 imageToWidget()。
//     解法：paintOverlay() 里先取 painter.worldTransform()（此刻它就是画布
//     刚设好的 m_viewTransform），再 resetTransform 切到控件坐标，
//     用 t.map(...) 自行映射。口径与画布完全一致，且不新增任何接口。
//
//  ★ 依赖纪律 ★
//    只 include tools/ 与 core/ 的东西。Selection 的定义必须完整（要用
//    bounds() 播种初始框）；CropOptions 只做前置声明 —— 它的定义在
//    ToolOptions.h 里，那个头文件拖着整个 QtWidgets。
// ============================================================================

#include "tools/AbstractTool.h"
#include "tools/ToolParams.h"   // AspectPreset / CropGrid（★ 零 QtWidgets 依赖 ★）

#include <QRectF>
#include <QPointF>
#include <QSize>

namespace zl {

class CropOptions;

class CropTool : public AbstractTool
{
    Q_OBJECT

public:
    // ── 手柄枚举 ──
    //
    // ★ 为什么把枚举放在最前面 ★
    //   C++ 的类内名字查找规则：函数**体**里可以引用后面才声明的成员，
    //   但函数**签名**里的类型必须在声明点已经可见。resizeRect() 的参数
    //   要用 Handle，所以枚举必须先出现。
    enum class Handle {
        None = 0,
        Inside,        // 框内（不是手柄）→ 按下即整体平移
        TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left
    };

    explicit CropTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~CropTool() override;

    // ── 身份 ──
    QString id() const override { return QStringLiteral("crop"); }
    QString displayName() const override { return tr("Crop"); }
    int     shortcutKey() const override { return Qt::Key_C; }

    ToolOptions *options() const override;
    QCursor      cursor() const override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;
    ToolResult mouseDoubleClick(const ToolEvent &e) override;
    ToolResult keyPress(const ToolEvent &e) override;

    // ── 叠加层 ──
    // 框外遮罩 + 边框 + 构图网格（图像坐标）与角柄 / 尺寸标签（控件坐标）。
    // 两者都在这里画 —— paintScreenOverlay() 拿不到视图变换，见头文件顶部 ④。
    void paintOverlay(QPainter &painter, CanvasView *view) override;

    // ── 文档切换 ──
    void setDocument(Document *doc) override;

    // ── 生命周期 ──
    // 切换工具 / 关闭文档时调用：丢弃未确认的框。
    //   语义与 SelectionTool 一致 —— "没确认过的框本来就不算数"。
    //   注意：这里**不执行裁剪**。执行裁剪只有三个途径：双击、Enter、
    //   或者用户自己再点一次（不存在）。"拖到一半点了别的工具"不该悄悄裁一刀。
    void commit() override;

    // Esc（由 CanvasView::keyPressEvent 拦下后调用）：框复位到整幅画布。
    //   文档从头到尾没被改过，所以"取消"只需要把矩形改回去。
    void cancel() override;

    // ── 供自检使用的只读查询 ──
    //
    // ★ 为什么要暴露 m_rect ★
    //   crop_test 要断言的正是"拖动之后框落在哪"。没有这个入口，
    //   测试就只能靠"能不能编过"来间接验证，而几何算错恰恰是能编过的。
    QRectF currentRectF() const { return m_rect; }
    QRect  currentRect() const;          // 取整到像素的版本（就是提交时用的那个）
    bool   isSeeded() const { return m_seeded; }

    // ── 纯几何helper：全部 static，便于自检直接调 ──
    //
    // 三个函数都不读成员、不碰文档，输入输出全部是矩形与点 ——
    // 这样"比例约束算错了"这类问题可以在没有画布、没有窗口的情况下定位。
    // 单位为图像坐标的浮点像素。

    // 由"起点 + 当前点"拖出矩形。ratio <= 0 表示不约束比例。
    // square 为 true 时按 1:1 处理（Shift 的临时约束，覆盖 ratio）。
    // 结果保证落在 bounds 内；比例约束下会**等比收缩**而不是截断，
    // 否则拖到画布边缘时框的宽高比会突然变掉。
    static QRectF dragNewRect(const QRectF &bounds, const QPointF &anchor,
                              const QPointF &current, qreal ratio, bool square);

    // 拖手柄调整：固定对角（角柄）或对边（边柄）。边柄在比例约束下
    // 会**在垂直方向上居中**扩张，与 Photoshop 一致。
    static QRectF resizeRect(const QRectF &bounds, const QRectF &start, Handle handle,
                             const QPointF &current, qreal ratio, bool square);

    // 整体平移：整个框钳制在 bounds 内（不会被推出画布，否则框会消失）。
    static QRectF moveRect(const QRectF &bounds, const QRectF &start,
                           const QPointF &delta);

    // 命中测试。zoom 用于把"手柄的屏幕尺寸"换算成图像坐标尺寸。
    // 返回 Inside 表示落在框内（不带手柄），None 表示落在框外。
    Handle hitTest(const QPointF &imagePos, qreal zoom) const;

signals:
    // 请求主窗口执行裁剪。★ 这是本工具唯一的"动手"出口 ★
    void cropRequested(const QRect &rect);

private:
    // 两种内部状态：正在拖框本身，还是正在拖某个手柄
    enum class Mode { Idle, NewRect, Move, Resize };

    // 画布矩形（图像坐标）。QRectF 的口径：right = x + width。
    QRectF canvasBounds() const;

    // 首次需要画框时播种：有选区用选区包围盒，否则整幅画布。
    // 画布尺寸变化后会自动重新播种（旧框已经不可能还在画布内）。
    void ensureSeeded();

    // 提交裁剪：把 m_rect 取整，做退化检查（框 = 整幅画布 / 空框）后发信号。
    void emitCrop();

    // 当前设置下的目标宽高比。0 表示自由。
    qreal targetRatio() const;

    // 把已经存在的框"内切"到当前比例预设上（锚点是框中心，只缩不放）。
    // 由 CropOptions::changed 触发 —— 用户在选项条里选了 16:9，
    // 框却纹丝不动会让人以为那个下拉框是坏的。
    void applyRatioToCurrentRect();

    // ── 状态 ──

    CropOptions *m_options = nullptr;

    // 裁剪框（图像坐标，浮点）。★ 这是本工具的全部"数据" ★
    QRectF m_rect;
    bool   m_seeded = false;

    // 播种时的画布尺寸。与当前不一致说明画布被改过（刚裁完一次、
    // 或撤销了一次裁剪），此时旧框无意义，必须重新播种。
    QSize  m_seedCanvasSize;

    Mode   m_mode = Mode::Idle;
    Handle m_activeHandle = Handle::None;

    // 按下瞬间的位置与原框。拖动期间一律"从按下时的框重算"，
    // 而不是在上一次结果上累加 —— 这是本项目反复出现的同一个主题
    // （画笔、移动、选区都是如此），累加会让"拖出去再拖回来"产生漂移。
    QPointF m_dragStart;
    QRectF  m_dragStartRect;

    // 按下瞬间从选项条读到的比例。拖动期间不再读 ——
    // 否则用户在拖动时改了选项，框会在半空中跳一下。
    qreal m_dragRatio = 0.0;
};

} // namespace zl
