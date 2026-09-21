#pragma once

// ============================================================================
//  ToolParams.h —— 工具参数枚举（★ 零 QtWidgets 依赖 ★）
// ----------------------------------------------------------------------------
//  为什么把枚举从 ToolOptions.h 里分出来，单独放一个头文件：
//
//  ToolOptions.h 必须 include <QtWidgets>（createWidget 返回 QWidget*），
//  而 AbstractTool.h 要 include ToolOptions.h。于是：
//
//     任何 include 了 AbstractTool.h 的文件
//          → 间接拿到全部 QtWidgets 头
//              → 编译变慢（QtWidgets 是最大的模块之一，约 1~2 秒）
//              → 架构分层被隐性破坏（engine 层不再"只依赖 QtGui"）
//
//  参数枚举恰恰是 engine 层也需要的 —— 例如"矩形工具的画圆角半径"
//  会作为参数传给 engine 的绘制函数，"选区的运算方式"要传给
//  core/Selection::Op。如果不分开，engine 层就只有两条路：
//    ① include 一个拖着 QtWidgets 的头（破坏铁律）
//    ② 自己再定义一遍同样的枚举（两处定义必然漂移）
//
//  分开之后：engine / core / commands 都可以放心 include 本文件。
//
//  ★ 纪律：本文件只允许 include QtCore 级别的头（QString、Qt 命名空间），
//    永远不要 include <QWidget>、<QPushButton> 之类的东西。
// ============================================================================

#include <QString>
#include <Qt>
#include <QMetaType>

namespace zl {

// ---------------------------------------------------------------------------
//  吸管取样范围
// ---------------------------------------------------------------------------
enum class PickRadius : int {
    Point  = 0,   // 单点
    Average3x3 = 1,   // 3×3 平均（嘈杂图像更稳）
    Average5x5 = 2    // 5×5 平均
};

QString pickRadiusName(PickRadius r);

// 取样范围的边长（像素）。Point 返回 1，其余返回 3 / 5。
int pickRadiusSize(PickRadius r);

// ---------------------------------------------------------------------------
//  形状工具的类型
// ---------------------------------------------------------------------------
enum class ShapeKind : int {
    Rectangle = 0,
    Ellipse   = 1
};

// ---------------------------------------------------------------------------
//  形状的绘制模式
// ---------------------------------------------------------------------------
enum class ShapeMode : int {
    Stroke = 0,   // 只描边
    Fill   = 1,   // 只填充
    Both   = 2    // 描边 + 填充
};

QString shapeModeName(ShapeMode m);

// ---------------------------------------------------------------------------
//  文字对齐
// ---------------------------------------------------------------------------
enum class TextAlign : int {
    Left   = 0,
    Center = 1,
    Right  = 2
};

QString textAlignName(TextAlign a);

// ---------------------------------------------------------------------------
//  选区模式（对应选项条上的四个按钮）
// ---------------------------------------------------------------------------
//  注意：这里的值与 core/Selection.h 的 Selection::Op 是**一一对应**的
//  （都是 Replace/Add/Subtract/Intersect 的 0/1/2/3 顺序）。
//  之所以不直接复用 Selection::Op，是为了不让 tools 层被 core 层的
//  某个具体数据结构的枚举绑死 —— 将来若 Selection 重构，
//  tools 层只需要改一个转换函数的实现。
//
//  转换函数：SelectionOptions::toSelectionOp()
// ---------------------------------------------------------------------------
enum class SelectionMode : int {
    Replace   = 0,   // 新建
    Add       = 1,   // 添加
    Subtract  = 2,   // 减去
    Intersect = 3    // 交集
};

QString selectionModeName(SelectionMode m);

// ---------------------------------------------------------------------------
//  裁剪比例预设
// ---------------------------------------------------------------------------
enum class AspectPreset : int {
    Free       = 0,
    Square     = 1,    // 1:1
    FourThree  = 2,    // 4:3
    ThreeTwo   = 3,    // 3:2
    SixteenNine= 4,    // 16:9
    NineSixteen= 5,    // 9:16（竖屏）
    TwoThree   = 6,    // 2:3
    ThreeFour  = 7,    // 3:4
    Custom     = 8     // 自定义（由用户在界面上输入）
};

QString aspectPresetName(AspectPreset p);

// ---------------------------------------------------------------------------
//  裁剪网格叠加
// ---------------------------------------------------------------------------
enum class CropGrid : int {
    None      = 0,   // 不显示
    Thirds    = 1,   // 三分法
    Grid      = 2,   // 密网格
    Diagonal  = 3,   // 对角线
    GoldenRatio = 4  // 黄金比例
};

QString cropGridName(CropGrid g);

// ---------------------------------------------------------------------------
//  缩放工具的行为
// ---------------------------------------------------------------------------
//  这个枚举随"滚轮是否缩放"这个设置一起用，放在这里方便统一持久化。
enum class ZoomMode : int {
    ZoomIn  = 0,
    ZoomOut = 1
};

// ---------------------------------------------------------------------------
//  缩放步长
// ---------------------------------------------------------------------------
//  ★ 这一个数字全工程只有这一处定义 ★
//
//    "点一下缩放工具"（tools/ZoomTool）与"按 Ctrl+= / 菜单里的放大"
//    （ui/CanvasView::zoomIn）必须走**同一档**步长 —— 否则用户会认为
//    其中一个是坏的（"为什么点工具放得比按快捷键多？"）。
//
//    两边分属 tools/ 与 ui/，共享常量只有放在这里才不会漂移：
//    ui → tools 本来就是允许的依赖方向（见架构铁律 5），
//    而 tools/ 绝不能反过来 include ui/。
//
//  为什么是 1.25 而不是 2.0：
//    连续按放大键时 2 倍会很快撞到上限（64×），而 1.25 的手感更细腻。
//    Photoshop 用的也是相近的比例。
constexpr qreal kZoomStepFactor = 1.25;

} // namespace zl

Q_DECLARE_METATYPE(zl::PickRadius)
Q_DECLARE_METATYPE(zl::ShapeKind)
Q_DECLARE_METATYPE(zl::ShapeMode)
Q_DECLARE_METATYPE(zl::TextAlign)
Q_DECLARE_METATYPE(zl::SelectionMode)
Q_DECLARE_METATYPE(zl::AspectPreset)
Q_DECLARE_METATYPE(zl::CropGrid)
