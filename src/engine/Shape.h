#pragma once

// ============================================================================
//  Shape.h —— 直线 / 矩形 / 椭圆的光栅化
// ----------------------------------------------------------------------------
//  一句话：把"一条直线"或"一个矩形 / 椭圆"变成**覆盖度蒙版**，
//  合成交给 composeStroke()（engine/Brush.h）。本文件不碰图层、不碰文档。
//
//  ★★ 为什么又是一张蒙版，而不是"画好的图像" ★★
//
//  理由与 Fill.h 完全一致（见那里「为什么不返回填好的图像」），这里再补
//  一条**只属于形状**的：
//
//    形状工具有"描边色"与"填充色"两个颜色，而它们**必须分两次合成**
//    （描边压在填充之上）。如果引擎直接返回画好的图像，就得在引擎里
//    自己实现一遍 source-over —— 那会成为全工程**第三份**合成实现
//    （画笔一份、油漆桶一份），也是最容易漏掉"选区边缘不泄漏"的那一份。
//    分成两张蒙版之后，"两次 composeStroke"就是全部，一行合成代码都不用新写。
//
//  ★★ 覆盖度取自 alpha 通道，而不是 convertToFormat(Grayscale8) ★★
//
//  绘制目标用 ARGB32_Premultiplied（raster engine 的原生格式，抗锯齿质量
//  最好），再取它的 alpha 通道当覆盖度。**刻意不走 convertToFormat**：
//
//    ARGB32_Premultiplied → Grayscale8 的转换，Qt 内部是否先做
//    unpremultiply（解预乘）取决于具体格式转换路径 —— 若它解了预乘，
//    抗锯齿边缘那圈"半透明的白"会被还原成**不透明的白**（灰度 255），
//    于是抗锯齿静默失效：形状边缘重新变成硬台阶，而代码一个字都没错。
//
//    取 alpha 通道则语义唯一且明确："这个像素被形状覆盖了多少" ——
//    正是 composeStroke 里 mask 的定义（mask × color.alpha() = 源 alpha）。
//    顺带还有一个好处：即使绘制时用的是灰色而不是白色，覆盖度也不会被
//    颜色污染（我们只看 alpha）。
//
//  ★★ 为什么蒙版的坐标系是"局部"的（不是整幅画布）★★
//
//  一张 4000×3000 的画布，Grayscale8 蒙版就是 12MB。拖动时每帧生成一张
//  整画布蒙版、再拷来拷去，内存与带宽都白白浪费（形状往往只占几十像素）。
//  所以：
//
//    · rasterizeLine / rasterizeShape **自己算出形状的包围盒**，
//      只在这个包围盒上绘制，返回的蒙版尺寸 == bounds.size()；
//    · 调用方若需要"整块重算区域 ⊇ 包围盒"（形状工具拖动时正是如此：
//      要把上一帧的预览擦掉），用 embedMask() 把局部蒙版嵌进区域坐标系。
//
//  ★ 本文件属于 engine 层 ★
//    只依赖 QtGui，绝不 include 任何 QtWidgets 头文件。
//    允许 include tools/ToolParams.h —— 那是"零 QtWidgets 依赖"的共享枚举头，
//    头文件里明确写了 engine/core/commands 都可以放心用（见该文件顶部说明）。
// ============================================================================

#include "tools/ToolParams.h"   // ShapeKind / ShapeMode（★ 零 QtWidgets ★）

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QPointF>
#include <QSize>
#include <Qt>

namespace zl {

// ---------------------------------------------------------------------------
//  结果：局部坐标系的覆盖度蒙版
// ---------------------------------------------------------------------------
struct ShapeMask
{
    // 描边覆盖度（Grayscale8，尺寸 == bounds.size()）。
    // 只有"描边"相关的模式才会生成；ShapeMode::Fill 时为空。
    QImage strokeMask;

    // 填充覆盖度（同上）。只有 ShapeMode::Fill / Both 才会生成。
    QImage fillMask;

    // 蒙版的包围盒（图像坐标，已与画布求交）。
    // ★ 蒙版像素 (0,0) 对应图像坐标 bounds.topLeft() ★
    // 空矩形表示"形状完全在画布外"，调用方应当什么都不做。
    QRect bounds;

    bool isEmpty() const { return bounds.isEmpty(); }
};

// ---------------------------------------------------------------------------
//  光栅化：直线
// ---------------------------------------------------------------------------
//  from / to   端点（图像坐标，浮点）
//  width       线宽（像素）。小于 1 时按 1 处理。
//  style       Qt::SolidLine / DashLine / DotLine …
//  antialias   是否抗锯齿
//
//  端点用 RoundCap（圆头）。理由：方头（FlatCap）在斜线上会让线**短一截**
//  （端点被切掉半个线宽），用户拖到哪里线就到哪里，圆头最符合直觉。
//
//  ★ 两点重合时返回空结果（什么都不画）★
//    这是与矩形/椭圆的"零面积不画"同一条规则：没有拖出长度就不是一个形状。
//    圆头笔在两点重合时会画出一个实实在在的小圆点，于是"用户点了一下画布"
//    就变成"凭空多了个还得撤销的痕迹"。
//
//  ★ 线宽是**图像坐标**下的宽度，不是屏幕像素 ★
//    所以放大到 800% 看，线会跟着变粗 —— 这与画笔的行为一致
//    （画笔的"20 像素"也是图像像素，不是屏幕像素）。
ShapeMask rasterizeLine(const QSize &canvasSize,
                        const QPointF &from, const QPointF &to,
                        int width, Qt::PenStyle style, bool antialias);

// ---------------------------------------------------------------------------
//  光栅化：矩形 / 椭圆
// ---------------------------------------------------------------------------
//  rect          形状的外接矩形（图像坐标，浮点，无需 normalized）
//  kind          Rectangle（可带圆角）/ Ellipse
//  mode          Stroke / Fill / Both
//  strokeWidth   描边线宽（mode 含描边时有效）
//  cornerRadius  圆角半径（仅 Rectangle 且 > 0 时生效；Ellipse 忽略）
//  antialias     是否抗锯齿
//
//  ★ Both 的两张蒙版是**独立**的 ★
//    重叠区域的"描边压在填充之上"由调用方按顺序合成两次来体现
//    （先 fillMask 后 strokeMask），而不是在蒙版里预先做减法。
//    这样描边色半透明时，重叠处会像 Photoshop 一样叠得更实。
ShapeMask rasterizeShape(const QSize &canvasSize, const QRectF &rect,
                         ShapeKind kind, ShapeMode mode,
                         int strokeWidth, int cornerRadius, bool antialias);

// ---------------------------------------------------------------------------
//  位置搬运：把局部蒙版嵌进一块更大的区域里
// ---------------------------------------------------------------------------
//  形状工具拖动时，每一帧要重算的区域是"上一帧包围盒 ∪ 本帧包围盒"——
//  比本帧的 bounds 大。而 rasterizeXxx 只产出本帧 bounds 大小的蒙版，
//  于是需要在区域坐标系里补出"其余部分是 0"的那一圈。
//
//  ★ 其余部分必须是 0（而不是"不填充"或"沿用原值"）★
//    0 在 composeStroke 里表示"这个像素没被碰到、原样保留"。若把上一帧的
//    残留盖在这里，形状就会拖着一条擦不掉的尾巴。
//
//  mask        局部蒙版（Grayscale8），可为空（返回空 QImage）
//  maskBounds  局部蒙版在图像坐标里的位置（即 rasterizeXxx 返回的 bounds）
//  region      目标区域（图像坐标）。必须包含 maskBounds，否则返回空。
//  返回         Grayscale8，尺寸 == region.size()
QImage embedMask(const QImage &mask, const QRect &maskBounds, const QRect &region);

// ---------------------------------------------------------------------------
//  纯几何：Shift 约束
// ---------------------------------------------------------------------------
//  这两个函数不碰任何图像数据，纯粹是"修饰键改变了拖动的几何"，
//  单独拿出来是为了让自检能直接调它们 —— 约束算错是形状工具最容易出的
//  问题（"按住 Shift 拖出来的不是正方形"），而这类错误在界面上一眼看不出来
//  （差几个像素），只有断言抓得住。

// 把"起点 → 当前点"约束到 8 个方向之一（水平 / 垂直 / 两个对角线）。
// 返回**当前点在约束方向上的投影**（保留用户拖出的长度）。
//
// ★ 投影而不是"把角度吸到 45° 倍数上"★
//   后者会让终点跳到离鼠标很远的斜角上（长度也被改了），手感像鼠标失灵。
//   投影则是"方向听约束的、长度听用户的"，这是 Photoshop / GIMP 的一致做法。
QPointF constrainTo8Directions(const QPointF &start, const QPointF &current);

// 由拖动的两个点构造矩形。
// square 为 true 时约束成正方形（边长取 |dx| 与 |dy| 的较大者，
// 方向跟随拖动方向 —— 往哪个象限拖就在哪个象限生成，不会突然翻边）。
QRectF rectFromDrag(const QPointF &start, const QPointF &current, bool square);

} // namespace zl
