#pragma once

// ============================================================================
//  ShapeTool.h —— 直线 / 矩形 / 椭圆
// ----------------------------------------------------------------------------
//  ★★ 为什么三个工具挤在一个类里 ★★
//
//  与 BrushTool（画笔 / 橡皮）同一个理由，而且是同一段理由的第二次出现：
//  三者的**交互骨架完全相同** ——
//
//      按下定锚点 → 拖动改几何（Shift 约束）→ 松手一次性合成进活动图层
//
//  差别只有两处：
//      · 几何：直线是两点，矩形/椭圆是"由两点拖出的矩形"；
//      · 光栅化：调 engine/Shape 的 rasterizeLine 还是 rasterizeShape。
//
//  拆成三个类，"落笔快照 / 选区快照 / 幂等重算 / 撤销打包 / 取消回滚"
//  这五块要复制三遍 —— 而它们正是最容易写错、也最容易改一处漏两处的部分
//  （BrushTool.h 顶部那段"为什么每帧从快照重算"的说明，同样适用于这里）。
//
//  ★★ 拖动期间：真实像素预览，且从快照重算 ★★
//
//  与画笔完全同构：拖动时**直接写图层位图**做预览，松手时把整段变化
//  打包成一条 DrawCommand。理由见 BrushTool.h（幂等、抗手抖、撤销粒度）。
//  这里补一条形状独有的：
//
//    形状的"预览"很难在叠加层里画准。若用 paintOverlay 画一个示意形状，
//    它与真实结果的差异至少有三处 —— **选区裁剪**（选区外的部分不会出现）、
//    **与下层像素的混合**（半透明形状压在别的内容上是另一种颜色）、
//    **抗锯齿边缘**（叠加层是画在已呈现的图像上，不是画进图层）。
//    用户会看到"松手之后形状变了"。所以宁可写像素，让预览就是结果本身。
//
//  ★★ 重算区域 = 上一帧包围盒 ∪ 本帧包围盒 ★★
//
//  形状不是"沿轨迹落笔"，而是"每一次几何变化都整体重画"，所以没有画笔那种
//  "本帧新增落笔点"的概念。要重算的是**两块的并集**：
//    · 本帧包围盒 —— 画上新的形状；
//    · 上一帧包围盒 —— 把上一帧的形状擦掉（从快照恢复）。
//  于是区域必然 ⊇ 本帧包围盒，rasterize 产出的局部蒙版要用 embedMask()
//  搬进区域坐标系（其余部分是 0，表示"没碰到、原样保留"）。
//
//  ★★ 一次点击（没拖动）不产生任何东西 ★★
//
//  按一下不动就松手，形状是零面积 / 零长度。此时**不压命令、不改像素**。
//  理由：压一条"什么都没变"的命令会让撤销栈里多出一条记录，
//  用户按 Ctrl+Z 时看起来像"撤销没反应"（油漆桶里同一个坑，见 BucketTool.cpp）。
//
//  ★ 依赖纪律 ★
//    只 include tools/ 与 core/ 的东西，不 include 任何 ui/ 的头
//    （屏幕坐标靠 ToolEvent::widgetPos / viewZoom，本工具其实用不到）。
//    ShapeOptions / LineOptions 只做前置声明 —— 它们的定义在 ToolOptions.h 里，
//    那个头文件拖着整个 QtWidgets。
// ============================================================================

#include "tools/AbstractTool.h"
#include "tools/ToolParams.h"
#include "engine/Shape.h"

#include <QImage>
#include <QRect>
#include <QPointF>
#include <QColor>

namespace zl {

// LineOptions / ShapeOptions 的定义在 ToolOptions.h（那个头文件拖着整个
// QtWidgets），这里只前置声明 —— 与本工程其它工具类的做法一致。
class LineOptions;
class ShapeOptions;

// ShapeMask 来自 engine/Shape.h，在本类里**按值**用作返回值与局部变量，
// 因此必须完整包含（前置声明不够）。它是纯 QtGui 的类型，不破坏分层。

class ShapeTool : public AbstractTool
{
    Q_OBJECT

public:
    // 与 BrushTool::Kind 同一个套路：一个类三个实例。
    // 枚举顺序 = ToolParams.h 里 ShapeKind 的顺序，转换时不用查表。
    enum class Kind { Line, Rectangle, Ellipse };

    ShapeTool(Kind kind, Document *doc, CommandStack *stack,
              QObject *parent = nullptr);
    ~ShapeTool() override;

    // ── 身份 ──
    Kind kind() const { return m_kind; }

    QString id() const override;
    QString displayName() const override;
    int     shortcutKey() const override;

    ToolOptions *options() const override;

    // ── 前景 / 背景色同步 ──
    //
    //  前景色 → 直线色 / 形状描边色（画布在"用户改了前景色"与
    //           "本工具被激活"两个时机推过来）
    //  背景色 → 形状填充色
    //
    // ★ 为什么填充色接背景色 ★
    //  "前景色画线条、背景色做填充"是绘图软件的通行约定（工具箱底部那对
    //  黑白方块就是这个意思）。不接的话，ShapeOptions 里那个填充色会
    //  永远停在默认的白色，用户改背景色时毫无反应 —— 看起来像坏了。
    void setForegroundColor(const QColor &color) override;
    void setBackgroundColor(const QColor &color) override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;

    // ── 生命周期 ──
    void commit() override;     // 切换工具 / 关文档：把正在拖的形状收尾压栈
    void cancel() override;     // Esc：回滚到落笔前，撤销栈里不留痕
    bool isBusy() const override;

    // ── 自检入口：不构造 ToolEvent 也能走完"落笔 → 拖动 → 松手" ──
    //
    //  返回是否真的产生了像素改动。
    //  ★ 之所以要这个入口 ★
    //    "拖动时每帧从快照重算"这条纪律，最好的检验方式是**连续调用它两次**
    //    并比较结果 —— 第二次必须与第一次完全相同（幂等）。
    //    而构造一串 ToolEvent 来测同一件事，会把"事件构造错了"混进失败原因里。
    bool drawShape(const QPointF &anchor, const QPointF &current,
                   bool constrain = false);

private:
    // ── 形状的生命周期 ──

    // 落笔：取整层快照、选区快照，记下活动图层。
    // 返回 false 表示当前不可写（图层锁定/隐藏/不存在），此时不进入绘制状态。
    bool beginShapeAt(const QPointF &imagePos);

    // 按"锚点 + 当前点（含 Shift 约束）"生成蒙版并写进图层。
    void updatePreview(const QPointF &anchor, const QPointF &current, bool constrain);

    // 松手：把整段变化打包成一条 DrawCommand 压栈
    void finishShape();

    // 取消：把图层恢复成落笔前的样子，不产生撤销记录
    void discardShape();

    // ── 内部工具 ──

    // 当前几何 + 当前选项 → 覆盖度蒙版（图像坐标，含 Shift 约束）
    ShapeMask buildMask(const QPointF &anchor, const QPointF &current,
                        bool constrain) const;

    // 在 region 上用"落笔前快照 + 蒙版"重算一遍并写回图层。
    // ★ region 必须完整包含 mask.bounds ★
    void applyRegion(const QRect &region, const ShapeMask &mask);

    // 可写的活动图层位图；返回 nullptr 表示当前不可写
    QImage *writableLayerImage();

    // 实际用于描边 / 填充的颜色。
    // 不透明度就是颜色自带的 alpha（本工具的选项里没有单独的不透明度滑块），
    // 所以这里不需要像画笔那样再做一次乘法。
    QColor strokeColor() const;
    QColor fillColor() const;

    // ── 状态 ──

    Kind m_kind;

    // 按 kind 只创建其中一个（另一个保持 nullptr）。
    // 用两个具体指针而不是 ToolOptions*：options() 要返回能给
    // ToolOptionsBar 用的东西，而两种参数对象的信号也不一样
    // （LineOptions::colorChanged / ShapeOptions::strokeColorChanged）。
    LineOptions  *m_lineOptions  = nullptr;
    ShapeOptions *m_shapeOptions = nullptr;

    // ── 拖动进行中的状态 ──

    // 落笔前的整层快照（浅拷贝，写图层时才真正 detach）
    QImage m_originalImage;

    // 选区快照（Grayscale8，画布尺寸）。空表示"全选"。
    QImage m_selectionMask;

    // 按下时的锚点（图像坐标）。★ 拖动全程不变 ★
    // 它就是"线的起点"或"矩形的第一个角"，Shift 约束也是以它为基准。
    QPointF m_anchor;

    // 本帧之前的形状包围盒。下一帧要用它把旧预览擦掉。
    QRect m_prevBounds;

    // 整段操作已触及区域的并集 —— 松手时它就是 DrawCommand 的脏矩形。
    QRect m_contentBounds;

    int m_layerIndex = -1;      // -1 表示当前没有形状在进行中

    // 用户是否真的拖动过（用于区分"点一下就松手"与"拖到画布外"）
    bool m_dragged = false;

    // ── 拖动的节流状态 ──
    //
    // ★ 为什么需要它 ★
    //   形状每帧要重算"上一帧 ∪ 本帧"整块区域。有些平台会在鼠标没动时
    //   持续发 move 事件，这时每帧白算一次大区域能把 CPU 吃满，
    //   而画面纹丝不动。位置与 Shift 都没变就跳过（见 mouseMove）。
    bool    m_hasMove = false;
    QPointF m_lastMovePos;
    bool    m_lastShift = false;
};

} // namespace zl
