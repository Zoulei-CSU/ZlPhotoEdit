#include "engine/Text.h"

#include <QPainter>
#include <QFontMetricsF>
#include <QStringList>
#include <QColor>
#include <QtMath>

namespace zl {

// ===========================================================================
//  内部辅助
// ===========================================================================
namespace {

// 统一换行符。为什么要单独做：
//   QTextEdit::toPlainText() 在 Windows 上给的是 "\r\n"，而 Qt 内部
//   其它地方（QLabel 等）常见的是 "\n"，粘贴进来的文本可能混着两种。
//   不归一化的话，"一段从剪贴板粘进来的两行文字"会被 split('\n') 切出
//   "第一行\r" 与 "第二行"，而那个残留的 '\r' 在 drawText 里是**不可见**的
//   （它不占宽度、不画任何东西）—— 于是排版看起来完全正常，
//   而字距/宽度测量会莫名其妙地差一点。这是"看不见的字符"的典型陷阱。
QStringList splitLines(const QString &text)
{
    QString t = text;
    t.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return t.split(QLatin1Char('\n'));
}

// 蒙版里有没有"墨"。全是 0 表示这段文本实际什么都没画出来
//   —— 例如整段都是空格 / 制表符（有 advance、有包围盒，但没有字形），
//   或者字体缺失导致一个字形都取不到。
//
// ★ 为什么必须单独判它 ★
//   ① 不判的话，一个空格会走完整条"落像素 + 压一条撤销命令"的流程，
//      而 composeStroke 面对全 0 蒙版是**原样返回**（像素一个字节都没变）
//      —— 于是撤销栈里多出一条"按了 Ctrl+Z 什么也没发生"的记录。
//      这类空命令是所有绘制工具的共同敌人（油漆桶、形状都处理过）。
//   ② "非空 bounds ⟺ 确实有墨" 是一条能写进断言的**不变量**，
//      比"包围盒算对了"强得多：它同时覆盖排版、绘制与裁剪三条路径。
bool maskHasInk(const QImage &mask)
{
    if (mask.isNull() || mask.format() != QImage::Format_Grayscale8)
        return false;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (line[x] != 0)
                return true;
        }
    }
    return false;
}

// 行与行之间的基线步长（像素）。行距是**倍数**语义：
//   lineHeight = 1.0 → 行紧挨着（步长 = 字体行高）
//   lineHeight = 2.0 → 行间距等于一行的高度
// ★ 下限钳到 0.1 ★ —— 参数来自界面（滑块/数字框），0 或负值会让所有行
//   叠在一起，那不叫"行距为零"而叫"文本块高度算成 0 → 包围盒为空 →
//   一个字都不画"，用户会以为工具坏了。
qreal lineStep(const QFontMetricsF &fm, qreal lineHeight)
{
    return fm.height() * qMax(0.1, lineHeight);
}

// 按对齐方式算出文本块的左边缘。
qreal blockLeft(TextAlign align, qreal anchorX, qreal blockWidth)
{
    switch (align) {
    case TextAlign::Center: return anchorX - blockWidth / 2.0;
    case TextAlign::Right:  return anchorX - blockWidth;
    case TextAlign::Left:
    default:                return anchorX;
    }
}

} // namespace

// ===========================================================================
//  纯几何：包围盒
// ===========================================================================
QRectF measureTextBlock(const QString &text, const QFont &font,
                        TextAlign align, qreal lineHeight,
                        const QPointF &anchor)
{
    if (text.isEmpty())
        return QRectF();

    const QFontMetricsF fm(font);
    const QStringList lines = splitLines(text);

    qreal width = 0.0;
    for (const QString &line : lines)
        width = qMax(width, fm.horizontalAdvance(line));

    // 高度：(n-1) 个行距 + 最后一行自己的行高。
    // ★ 不是 n * 行距 ★ —— 那样最后一行下面会多出一整行空白，
    //   包围盒（以及随后的 DrawCommand 脏矩形）凭空变大一圈。
    const qreal height = (lines.size() - 1) * lineStep(fm, lineHeight) + fm.height();

    return QRectF(blockLeft(align, anchor.x(), width), anchor.y(), width, height);
}

// ===========================================================================
//  光栅化
// ===========================================================================
TextMask rasterizeText(const QString &text, const QFont &font,
                       TextAlign align, qreal lineHeight,
                       const QPointF &anchor, const QSize &canvasSize)
{
    TextMask out;

    if (canvasSize.isEmpty())
        return out;

    const QRectF block = measureTextBlock(text, font, align, lineHeight, anchor);
    // 空文本 / 零宽（全是空白行）→ 没有可画的东西。
    // ★ 返回空 bounds 而不是"一张空蒙版" ★：调用方（与自检）据此判断
    //   "这次操作什么都没产生"，从而不去压一条什么都不改的撤销命令。
    if (block.isEmpty())
        return out;

    const QFontMetricsF fm(font);
    const QStringList lines = splitLines(text);

    // ── 包围盒：向外留余量 ──
    //
    //  三条理由，缺一条都会让字形被切掉一点边：
    //    ① 抗锯齿：字形边缘会溢出 advance 一个像素左右（斜体尤其明显，
    //       因为斜体的右上角"探"出字身框）；
    //    ② 下划线：画在基线**以下**，而 fm.height() 只覆盖字形本身；
    //    ③ 取整：left/top 向下取整、宽高向上取整，各让出一格。
    //
    //  余量取 3 像素是实测值（见自检里"墨迹必须完整落在包围盒内"那条）。
    const int pad = 3;
    const QRect box(int(qFloor(block.left())) - pad,
                    int(qFloor(block.top()))  - pad,
                    int(qCeil(block.width()))  + 2 * pad + 1,
                    int(qCeil(block.height())) + 2 * pad + 1);

    // 与画布求交 —— 画布外的部分不生成像素（省内存，也让"文字全在画布外"
    // 这类情况自然退化成空结果）。
    out.bounds = box & QRect(QPoint(0, 0), canvasSize);
    if (out.bounds.isEmpty())
        return out;

    QImage img(out.bounds.size(), QImage::Format_Grayscale8);
    if (img.isNull()) {
        out.bounds = QRect();
        return out;
    }

    // 底填黑（0）。白色文字以 source-over 画上去之后，
    // 像素值 = 255 * 覆盖度 —— 值本身就是覆盖度（见 Text.h 顶部）。
    img.fill(0);

    {
        QPainter painter(&img);
        if (!painter.isActive()) {
            // 见 Text.h：「宁可明确失败，也不要看起来成功」。
            out.bounds = QRect();
            return out;
        }

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setPen(QColor(255, 255, 255));
        painter.setFont(font);

        // 把"图像坐标"平移成"蒙版局部坐标"。
        // ★ 用 translate 而不是逐点减偏移 ★
        //   后者要在每个 drawText 的坐标里手算一次减法，将来加"逐字抖动"
        //   之类的东西时极易漏掉一处 —— 和自绘控件里"模型坐标没映射"
        //   是同一类错误（见铁律 §4.4）。
        painter.translate(-out.bounds.left(), -out.bounds.top());

        const qreal step = lineStep(fm, lineHeight);
        const qreal left = block.left();
        const qreal top  = block.top();

        for (int i = 0; i < lines.size(); ++i) {
            const QString &line = lines.at(i);
            if (line.isEmpty())
                continue;                 // 空行不画（但**占**一行的高度）

            // ★ 基线 = 本行顶部 + ascent ★
            //   QPainter::drawText(QPointF, QString) 的 y 是**基线**，
            //   不是顶部 —— 传顶部的话文字会整体下沉一个 ascent。
            const qreal baseline = top + i * step + fm.ascent();
            painter.drawText(QPointF(left, baseline), line);
        }

        painter.end();
    }

    // ── 没有墨就不算有内容 ──
    //   见 maskHasInk 的说明：这挡住的是"整段空白字符"与"字体缺失"两种
    //   静默空操作 —— 它们都会让调用方以为"写成功了"。
    if (!maskHasInk(img)) {
        out.bounds = QRect();
        return out;
    }

    out.mask = img;
    return out;
}

} // namespace zl
