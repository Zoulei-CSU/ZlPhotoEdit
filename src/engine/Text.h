#pragma once

// ============================================================================
//  Text.h —— 文字的排版与光栅化
// ----------------------------------------------------------------------------
//  一句话：把"一段文字 + 字体 + 对齐 + 锚点"变成**覆盖度蒙版**，
//  合成依旧交给 composeStroke()（engine/Brush.h）。本文件不碰图层、不碰文档、
//  不创建任何控件 —— 输入中文的浮层是 ui 层的事（见 TextTool / CanvasView）。
//
//  ★★ 为什么又是一张蒙版，而不是"画好的图像" ★★
//
//  理由与 Fill.h / Shape.h 完全一致，这里只补一条**只属于文字**的：
//
//    文字的颜色是**单一颜色**（没有渐变、没有多色），形状可以是非矩形，
//    还有抗锯齿 —— 这正好是 composeStroke(base, mask, color) 的全部输入。
//    如果这里直接返回"画好的彩色图"，工具就得自己再做一次 source-over，
//    那会成为全工程第四份合成实现，也就是最容易漏掉"选区边缘不泄漏"、
//    "颜色 alpha 即透明度"、"预乘空间运算"这三条纪律的那一份。
//
//  ★★ 为什么不用 QTextDocument / QTextEdit 的排版 ★★
//
//  两条硬理由：
//
//    ① **工具层与引擎层不能依赖 QtWidgets**。QTextEdit 是 QtWidgets 的类，
//       而 engine 只允许依赖 QtGui（这是分层的守门人，见 run_tests.sh 的
//       moc_headers_for / extra_libs_for：engine 的测试只链 Qt5Core + Qt5Gui）。
//       浮层必须由 ui 层创建，那么"排版"这件事就只能在 QtGui 范围内自己做 ——
//       即 QFont + QFontMetricsF + QPainter::drawText。
//
//    ② **排版必须可断点断言**。自己排版意味着"文字块的包围盒"是一个可以
//       被测量、被断言的量（measureTextBlock）。若交给 QTextDocument，
//       包围盒是它内部状态的派生物，自检就只能"看图"（见铁律第九章）。
//
//  代价：本文件**不支持自动换行** —— 只按显式的 '\n' 断行。
//  这是刻意的取舍：自动换行需要一个"折行宽度"，而点子文字（point text）
//  的锚点语义里并没有这个宽度。Photoshop 的点文字同样只按回车换行
//  （要自动换行得改成段落文字，本工程不做）。编辑浮层也用 NoWrap 对齐这一点，
//  否则浮层会折行、落到图层上却不折 —— 用户看到"确定之后行变长了"。
//
//  ★★ 覆盖度：直接画在 Grayscale8 上，值就是覆盖度 ★★
//
//  底填黑（0），用白色（255）画字。source-over 在"目标不透明"时退化成
//  线性插值：out = src*a + dst*(1-a) = 255*a —— 于是**像素值本身就是覆盖度**，
//  不需要再取 alpha 通道（Shape.h 那份"取 alpha 而不是 convertToFormat"
//  的说明针对的是 ARGB 绘制目标；这里压根没有 alpha 通道可以取错）。
//
//  若 QPainter 拒绝在 Grayscale8 上作画（painter.isActive() 为假），
//  本函数返回空结果 —— **不会静默画出一片空白**：调用方看到 bounds 为空，
//  自检也会立刻红。这是"宁可明确失败，也不要看起来成功"。
//
//  ★★ 锚点语义（全工程唯一一处定义）★★
//
//      锚点 = 文本块的**第一行顶部** + 水平方向的**对齐基准**
//
//        · 左对齐  → 文本块左边缘在 anchor.x
//        · 居中    → 文本块水平中心在 anchor.x
//        · 右对齐  → 文本块右边缘在 anchor.x
//        · 三者的顶边都在 anchor.y
//
//  为什么不学 Photoshop 把锚点放在"首行基线"：
//    基线的位置 = 顶部 + ascent，而 ascent 是**字体的属性**。同一个锚点、
//    换一个字体，文字块就会整体上下跳；用户想让两段不同字体的文字顶部对齐
//    时无法用同一个锚点值。用"首行顶部"则与字体无关，跨字体可对齐。
//    （代价：与 Photoshop 的锚点不可直接类比，故在此写明。）
//
//  ★★ letterSpacing 不在这里处理 ★★
//
//  QFont 自带字距（TextOptions::toFont 已经把"像素"换算成百分比写进字体）。
//  本文件**只管按行排布、对齐与行距**。若在这里再设一次字距，字距会被施加
//  两遍 —— 这是"同一件事有两处真相"的典型形态，而且是静默的（只是文字变宽）。
//  所以本函数的 font 参数就是**最终字体**，原样使用。
//
//  ★ 本文件属于 engine 层 ★
//    只依赖 QtGui，绝不 include 任何 QtWidgets 头文件。
//    允许 include tools/ToolParams.h —— 那是"零 QtWidgets 依赖"的共享枚举头。
// ============================================================================

#include "tools/ToolParams.h"   // TextAlign（★ 零 QtWidgets ★）

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QPointF>
#include <QFont>
#include <QString>
#include <QSize>

namespace zl {

// ---------------------------------------------------------------------------
//  结果：局部坐标系的覆盖度蒙版
// ---------------------------------------------------------------------------
struct TextMask
{
    // 覆盖度（Grayscale8，尺寸 == bounds.size()）。
    QImage mask;

    // 蒙版的包围盒（图像坐标，已与画布求交）。
    // ★ 蒙版像素 (0,0) 对应图像坐标 bounds.topLeft() ★
    // 空矩形表示"没有内容"或"文字完全在画布外"，调用方应当什么都不做。
    QRect bounds;

    bool isEmpty() const { return bounds.isEmpty() || mask.isNull(); }
};

// ---------------------------------------------------------------------------
//  纯几何：文字块的包围盒（不碰任何图像数据）
// ---------------------------------------------------------------------------
//  与 rasterizeText() 共用同一套度量，所以"算出来的框"与"画出来的墨"一致。
//  单独拿出来是为了让自检能直接量它 —— 对齐算错是文字工具最隐蔽的错误
//  （居中偏半个字、右对齐差一个字宽），在界面上一眼看不出来。
//
//  text        文本（按 '\n' 断行；'\r\n' 与 '\r' 都按 '\n' 处理）
//  font        最终字体（字距/粗斜体/字号都应当已经设好，见文件头）
//  align       水平对齐（相对 anchor.x）
//  lineHeight  行距**倍数**（相对 fontMetrics 的 height()，1.0 = 紧排）
//  anchor      锚点（图像坐标，浮点；见文件头的锚点语义）
//
//  返回        文本块的包围盒（浮点，图像坐标）。
//              空文本返回**零宽**矩形 —— 调用方据此判断"什么都不用做"。
QRectF measureTextBlock(const QString &text, const QFont &font,
                        TextAlign align, qreal lineHeight,
                        const QPointF &anchor);

// ---------------------------------------------------------------------------
//  排版 + 光栅化
// ---------------------------------------------------------------------------
//  canvasSize  画布尺寸。包围盒会与它求交 —— 画布外的部分不生成像素。
//
//  ★ 返回的 mask 是**局部**的（尺寸 == bounds.size()）★
//    理由与 Shape.h 相同：4000×3000 的画布，整张 Grayscale8 蒙版就是 12MB，
//    而一段文字往往只占几百像素。调用方若需要"在更大的区域里重算"，
//    用 embedMask() 把它嵌进去（engine/Shape.h）。
TextMask rasterizeText(const QString &text, const QFont &font,
                       TextAlign align, qreal lineHeight,
                       const QPointF &anchor, const QSize &canvasSize);

} // namespace zl
