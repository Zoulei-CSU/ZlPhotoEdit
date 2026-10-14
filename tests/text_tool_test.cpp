// ============================================================================
//  text_tool_test.cpp —— 文字工具的离线自检（浮层协议 / 落像素 / 撤销粒度）
// ----------------------------------------------------------------------------
//  本组与 text_test 的分工：
//
//    · text_test        —— 引擎（engine/Text）：一段文字排出来**长什么样**。
//                          全部押在"与字体无关的不变量"上（墨迹不贴边、
//                          bounds ⊆ 画布、三种对齐只有位移不同……）。
//    · text_tool_test   —— 工具（tools/TextTool）+ 参数对象（TextOptions）：
//                          文字**怎么被用户敲出来、怎么落到图层上、撤销粒度多大**。
//
//  所以这里**不去重复量字形**（那是 text_test 的事），只盯"接线的两端"：
//
//    A. **浮层协议**（最重要的一条）
//       文字工具是 14 个工具里唯一"有第二份界面"的 —— 输入浮层（QTextEdit）
//       归画布，内容归工具，两端靠 AbstractTool::requestTextEditor /
//       setEditingText 牵线。这条线是**单向且弱类型**的（信号实参一堆），
//       接错了编译器一声不吭：
//         · 忘了传 lineHeight   → 浮层按 100% 行距显示，确定之后行距突然变成两倍
//         · 传了屏幕 DPI 算的字体 → 换台机器字号就不一样
//         · 对齐传成常量而不是选项 → 选项条上点"居中"没反应
//       本组把 requestTextEditor 的**每一个实参**都断出来。
//
//    B. **落像素与撤销粒度**
//       一次输入 = 一条 DrawCommand::standalone()。撞号会被 QUndoStack
//       静默 mergeWith（"写两段，撤一次两段都没了"，见 DrawCommand.h）。
//       断言方式与 shape_tool_test 一致：看 count() 的增量，不写死编号值。
//
//    C. **拒绝路径必须是"拒绝"，不能是"写了一半"**
//       空文本 / 纯空白 / 画布外 / 无撤销栈 / 图层锁定隐藏 / 无文档。
//       其中"空文本"这条尤其重要：开了浮层一个字没敲就点别处，
//       压一条"按 Ctrl+Z 什么也没发生"的记录会毁掉 Ctrl+Z 的语义。
//       注意"纯空白"与"空"是两条不同的路径 —— 前者 engine 返回空蒙版
//       （有 advance、无字形），后者在工具层就被拦下。
//
//    D. **参数 → 引擎的链子**
//       对齐 / 行距 / 字距 / 颜色这四样都在 TextOptions 里，要真的走到
//       engine/Text 的实参上才算通。其中"字距"最阴 —— 它由**字体携带**
//       （TextOptions::toFont 把像素换算成百分比写进 QFont），
//       引擎里若再加一遍，字距会被施加两次，文字静静变宽（见 Text.h 顶部）。
//
//  ★ 关于字体与画布 ★
//    固定 Arial + 像素字号 48（文档 DPI 默认 72 → 磅值 == 像素值）。
//    画布给足 240×200，让 48px 的文本块（含两行、2.4 倍行距）不会碰到边界 ——
//    否则"画布裁剪"会混进对齐/行距的断言里，一处红看不出是谁的问题。
//
//  ★ 为什么用 kClear（全透明）做底 ★
//    本组大量用"哪里有墨"来定位文字（alpha != 0）。底色不透明的话
//    每个像素 alpha 都是 255，就没法用这个判据了。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 texttool 分支。链 Qt5Widgets（ToolOptions 要造控件），
//    用 offscreen 平台、并显式指 QT_QPA_FONTDIR（Qt 5.15 起不带字体，
//    offscreen 默认一个字体都找不到 —— 症状是"所有蒙版都是空的"而不是报错）。
// ============================================================================

#include "tools/TextTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolEvent.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "commands/CommandStack.h"
#include "engine/Text.h"          // measureTextBlock：断言"墨落在它该在的框里"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <Qt>
#include <QtGlobal>
#include <QUndoStack>

#include <cmath>
#include <cstdio>

using namespace zl;

// ---------------------------------------------------------------------------
//  断言辅助（与其余工具自检保持同一套写法）
// ---------------------------------------------------------------------------
static int g_failed = 0;
static int g_passed = 0;
static int g_group  = 0;

static void beginGroup(const char *name)
{
    ++g_group;
    std::printf("[%d] %s\n", g_group, name);
    std::fflush(stdout);
}

static void check(bool ok, const char *what)
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n", what);
    }
}

static void checkEqInt(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

static void checkNear(qreal actual, qreal expected, qreal tol, const char *what)
{
    if (std::fabs(actual - expected) > tol) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %.2f（容差 %.2f），实际 %.2f\n",
                    what, expected, tol, actual);
    } else {
        ++g_passed;
    }
}

static void checkRgb(QRgb actual, QRgb expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 a=%d r=%d g=%d b=%d，"
                    "实际 a=%d r=%d g=%d b=%d\n", what,
                    qAlpha(expected), qRed(expected), qGreen(expected), qBlue(expected),
                    qAlpha(actual),   qRed(actual),   qGreen(actual),   qBlue(actual));
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  像素读写（一律走扫描线；ARGB32_Premultiplied 下地址不一定是 x*4）
// ---------------------------------------------------------------------------
static QRgb rawPixel(const QImage &img, int x, int y)
{
    return reinterpret_cast<const QRgb *>(img.constScanLine(y))[x];
}

static void rawFillRect(QImage &img, const QRect &r, QRgb c)
{
    for (int y = r.top(); y <= r.bottom(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = r.left(); x <= r.right(); ++x)
            line[x] = c;
    }
}

static QRgb layerPixel(const Document *doc, int layerIndex, int x, int y)
{
    const Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return 0;
    return rawPixel(l->image(), x, y);
}

// 区域内的"墨迹"包围盒（**图像坐标**）。空矩形 = 一点墨都没有。
//
// ★ 判据是 alpha != 0 而不是"颜色不同" ★
//   底色是全透明（kClear），所以"有墨"就是"这里被打上了东西"。
//   用颜色比较的话，"白色文字画在透明底上"与"底色本来就是白"分不开。
static QRect layerInkBounds(const Document *doc, int layerIndex, const QRect &region)
{
    const Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return QRect();

    const QRect clip = region & QRect(QPoint(0, 0), l->image().size());
    if (clip.isEmpty())
        return QRect();

    int minX = clip.right() + 1, minY = clip.bottom() + 1;
    int maxX = clip.left() - 1,  maxY = clip.top() - 1;

    for (int y = clip.top(); y <= clip.bottom(); ++y) {
        for (int x = clip.left(); x <= clip.right(); ++x) {
            if (qAlpha(rawPixel(l->image(), x, y)) != 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < minX || maxY < minY)
        return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

static QRect wholeCanvasInk(const Document *doc, int layerIndex = 0)
{
    const Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return QRect();
    return layerInkBounds(doc, layerIndex, QRect(QPoint(0, 0), l->image().size()));
}

// 区域内非透明像素的个数（用于"选区之外一个墨点都没有"这类断言）
static int countInk(const Document *doc, int layerIndex, const QRect &region)
{
    const Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return 0;

    const QRect clip = region & QRect(QPoint(0, 0), l->image().size());
    int n = 0;
    for (int y = clip.top(); y <= clip.bottom(); ++y)
        for (int x = clip.left(); x <= clip.right(); ++x)
            if (qAlpha(rawPixel(l->image(), x, y)) != 0)
                ++n;
    return n;
}

static const QRgb kWhite = qRgba(255, 255, 255, 255);
static const QRgb kRed   = qRgba(255, 0, 0, 255);
static const QRgb kBlue  = qRgba(0, 0, 255, 255);
static const QRgb kClear = qRgba(0, 0, 0, 0);

// 画布要装得下 48px 的字（含两行 + 2.4 倍行距 ≈ 190px 高）还不碰边
static const int kW = 240;
static const int kH = 200;

static const int kPointSize = 48;      // 文档 DPI 72 → 像素字号也是 48

// 单图层文档。fill 直接写预乘字节（理由见 tool_paint_test）：
//   kClear 用于"靠 alpha 找墨"的那些断言；
//   kWhite 用于"只改活动图层"那种整层比对的断言。
static Document *makeDoc(QRgb fill = kClear)
{
    auto *doc = new Document(QSize(kW, kH), Qt::transparent);
    Layer *l = doc->activeLayer();
    if (l) {
        QImage img(kW, kH, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        rawFillRect(img, QRect(0, 0, kW, kH), fill);
        l->setImage(img);
        doc->markDirty();
    }
    return doc;
}

// ---------------------------------------------------------------------------
//  工具配置：固定 Arial + 像素字号 48，对齐/行距各自按需设
// ---------------------------------------------------------------------------
static TextOptions *prepareText(TextTool *tool, const QColor &color)
{
    auto *opts = qobject_cast<TextOptions *>(tool->options());
    if (!opts)
        return nullptr;
    opts->setFamily(QStringLiteral("Arial"));
    opts->setPointSize(kPointSize);
    opts->setColor(color);
    opts->setAlign(TextAlign::Left);
    return opts;
}

// ---------------------------------------------------------------------------
//  事件构造
// ---------------------------------------------------------------------------
//  ★ button 必须是左键 ★ （工具的 mousePress 第一件事就是挡掉非左键）
static ToolEvent mouseEvent(ToolEventType type, int x, int y)
{
    ToolEvent e;
    e.type = type;
    e.button  = Qt::LeftButton;
    e.buttons = (type == ToolEventType::Release) ? Qt::NoButton : Qt::LeftButton;
    e.imagePos = QPointF(x, y);
    e.widgetPos = QPointF(x, y);
    e.lastImagePos = QPointF(x, y);
    e.viewZoom = 1.0;
    return e;
}

// ---------------------------------------------------------------------------
//  浮层请求的捕获（A 组的靶子）
// ---------------------------------------------------------------------------
struct EditorReq
{
    bool    fired      = false;
    int     count      = 0;
    QPointF anchor;
    QFont   font;
    QColor  color;
    int     align      = -1;
    qreal   lineHeight = -1.0;
};

static void capture(AbstractTool *tool, EditorReq *req)
{
    QObject::connect(tool, &AbstractTool::requestTextEditor,
                     [req](const QPointF &anchor, const QFont &font,
                           const QColor &color, int align, qreal lineHeight) {
                         req->fired      = true;
                         ++req->count;
                         req->anchor     = anchor;
                         req->font       = font;
                         req->color      = color;
                         req->align      = align;
                         req->lineHeight = lineHeight;
                     });
}

// ===========================================================================
//  [1] 身份证
// ===========================================================================
static void testIdentity()
{
    beginGroup("身份证与初始状态");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    TextOptions *opts = prepareText(&tool, Qt::black);

    check(tool.id() == QStringLiteral("text"), "id 是 text（QSettings 键，定了不能改）");
    check(tool.displayName() == QString::fromUtf8("Text"), "显示名是「Text」");
    checkEqInt(tool.shortcutKey(), Qt::Key_T, "快捷键 T（与 Photoshop/GIMP 一致）");
    checkEqInt(int(tool.cursor().shape()), int(Qt::IBeamCursor),
               "光标是工字形（表达「插入点」，不是十字）");

    check(opts != nullptr, "拿到的是 TextOptions");
    check(qobject_cast<TextOptions *>(tool.options()) != nullptr,
          "options() 返回的确实是 TextOptions");
    check(!tool.hasInlineEditor(), "初始没有编辑浮层");
    check(!tool.isEditing(), "初始不在编辑会话中");
    check(!tool.isBusy(), "初始不是进行中状态");
    check(tool.options() != nullptr
              && tool.options()->settingsGroup() == QStringLiteral("tool/text"),
          "参数分组键是 tool/text（QSettings 持久化用）");

    delete doc;
}

// ===========================================================================
//  [2] 浮层协议：mousePress 请求开浮层，实参一个一个断
// ===========================================================================
static void testEditorProtocol()
{
    beginGroup("浮层协议：requestTextEditor 的每个实参");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    TextOptions *opts = prepareText(&tool, QColor(255, 0, 0));
    opts->setAlign(TextAlign::Center);
    opts->setLineHeight(1.75);

    EditorReq req;
    capture(&tool, &req);

    const QPointF anchor(80, 60);
    const ToolResult r = tool.mousePress(mouseEvent(ToolEventType::Press, 80, 60));

    check(r.handled, "左键按下被工具消费");
    check(r.needsRepaint, "左键按下要求重绘（浮层要出现）");
    check(req.fired, "发出了 requestTextEditor");
    checkEqInt(req.count, 1, "只发了一次（不是每帧都发）");

    check(req.anchor == anchor, "锚点就是点击处的**图像坐标**（不是控件坐标）");
    check(req.color == QColor(255, 0, 0), "颜色取自参数的当前颜色");
    checkEqInt(req.align, int(TextAlign::Center), "对齐取自参数（选项条上点居中要能生效）");

    // ★ lineHeight 必须传全 ★
    //   QFont 里没有"行距"这个概念。漏传的话浮层按 100% 行距显示，
    //   确定之后行距突然变成 1.75 倍 —— 用户看到"确认之后行跳开了"。
    checkNear(req.lineHeight, 1.75, 1e-6, "行距倍数一起传（漏了浮层与结果会不一致）");

    // ★ 字号必须是**像素字号**，且按文档 DPI 算 ★
    //   用 setPointSize 的话结果依赖屏幕 DPI，同一份工程在两台机器上大小不同。
    checkEqInt(req.font.pixelSize(), kPointSize,
               "字体是像素字号（不是磅值），值等于 选项磅值 × 文档DPI/72");
    check(req.font.family() == QStringLiteral("Arial"), "字体族取自参数");
    checkEqInt(int(req.font.weight()), int(QFont::Normal), "默认不是粗体");
    check(!req.font.italic(), "默认不是斜体");

    check(tool.hasInlineEditor(), "开浮层之后 hasInlineEditor 为真");
    check(tool.isEditing(), "处于编辑会话中");
    check(tool.isBusy(), "isBusy 也反映编辑会话（关闭文档时要据此提示）");
    check(tool.editAnchor() == anchor, "观测到的锚点与请求一致");
    check(tool.editingText().isEmpty(), "新会话的文本是空的");

    // ── 画布把文本推回来 ──
    tool.setEditingText(QStringLiteral("Hello"));
    check(tool.editingText() == QStringLiteral("Hello"), "setEditingText 接住了浮层的内容");
    check(tool.editAnchor() == anchor, "推文本不改锚点（锚点全程不变）");
    checkEqInt(req.count, 1, "推文本不会再发一次 requestTextEditor");

    // ── 右键 / 非按下事件不由文字工具处理 ──
    EditorReq req2;
    capture(&tool, &req2);
    ToolEvent right = mouseEvent(ToolEventType::Press, 10, 10);
    right.button = Qt::RightButton;
    check(!tool.mousePress(right).handled, "右键按下不由文字工具处理（归画布上下文菜单）");

    ToolEvent move = mouseEvent(ToolEventType::Move, 30, 30);
    check(!tool.mousePress(move).handled, "非按下事件不由 mousePress 处理");

    tool.cancel();
    delete doc;
}

// ===========================================================================
//  [3] placeText：跳过浮层直接落像素（自检 / 截图用的入口）
// ===========================================================================
static void testPlaceText()
{
    beginGroup("placeText：落像素 + 墨落在该在的框里");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    TextOptions *opts = prepareText(&tool, QColor(255, 0, 0));

    EditorReq req;
    capture(&tool, &req);          // placeText 走 notifyUi=false，不该发信号

    const QPointF anchor(30, 60);
    const bool changed = tool.placeText(QStringLiteral("H"), anchor);

    check(changed, "placeText 报告产生了改动");
    check(!req.fired, "placeText **不发** requestTextEditor（自检/截图没有浮层）");
    check(!tool.hasInlineEditor(), "placeText 收尾后不留编辑状态");
    check(!tool.isBusy(), "placeText 收尾后不是进行中");

    const QRect ink = wholeCanvasInk(doc);
    check(!ink.isEmpty(), "图层上确实出现了墨");
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
               "一次 placeText 压了**一条**撤销记录");

    // ★ "墨落在它该在的框里" ★
    //   measureTextBlock 是引擎给出的框，rasterizeText 画出来的墨必须落在里面。
    //   两者共用同一套度量，所以这条断言抓的是**工具**有没有把锚点/字体用错
    //   （例如拿了控件坐标当图像坐标、或忘了把选项的字体传进去）。
    const QRectF block = measureTextBlock(QStringLiteral("H"), tool.editorFont(),
                                          opts->align(), opts->lineHeight(), anchor);
    const QRect blockI = block.toAlignedRect().adjusted(-5, -5, 5, 5);
    check(blockI.contains(ink),
          "墨完整落在 measureTextBlock 给出的框里（锚点/字体没被用错）");

    // 左对齐 + 锚点在 (30,60)：文字块左边缘就是锚点 x，顶边就是锚点 y
    checkNear(block.left(), anchor.x(), 0.01, "左对齐：文本块左边缘 = 锚点 x");
    checkNear(block.top(),  anchor.y(), 0.01, "文本块顶边 = 锚点 y（锚点在**顶部**不是基线）");
    check(ink.left() >= int(anchor.x()) - 5,
          "墨不会跑到锚点左边（左对齐的锚点就是左边界）");
    check(ink.top() >= int(anchor.y()) - 5,
          "墨不会跑到锚点上方（锚点是首行**顶部**，不是基线）");

    // 颜色：不透明红 → 预乘就是纯红
    checkRgb(layerPixel(doc, 0, ink.center().x(), ink.center().y())
                 & 0x00FFFFFF,
             kRed & 0x00FFFFFF,
             "墨的中心是文字颜色");

    // ★ 墨迹四周都不贴到画布边界（本组最与字体无关的一条）★
    check(ink.left() > 0 && ink.top() > 0
              && ink.right() < kW - 1 && ink.bottom() < kH - 1,
          "墨迹四周都有余量（没有字形被画布切掉）");

    // 画布其他区域干净 —— "只画了该画的那一点"
    checkEqInt(countInk(doc, 0, QRect(0, 0, kW, int(anchor.y()) - 30)),
               0, "文字块上方一大片区域一个墨点都没有");

    delete doc;
}

// ===========================================================================
//  [4] 撤销粒度：两次输入 = 两条记录，逐条可还原
// ===========================================================================
static void testUndoGranularity()
{
    beginGroup("撤销：两次输入两条记录（撞号会被静默合并）");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    prepareText(&tool, QColor(255, 0, 0));

    check(tool.placeText(QStringLiteral("A"), QPointF(10, 20)), "第一段写入");
    const QRect inkFirst = wholeCanvasInk(doc);
    check(tool.placeText(QStringLiteral("B"), QPointF(150, 120)), "第二段写入");

    checkEqInt(static_cast<int>(stack.undoStack()->count()), 2,
               "两次输入是两条撤销记录（standalone 取新描边号）");
    check(!inkFirst.isEmpty(), "第一段确实有墨（前置条件）");

    // 第二段所在区域（远离第一段）
    const QRect nearSecond(140, 110, 90, 70);
    const QRect nearFirst(0, 10, 70, 70);
    check(countInk(doc, 0, nearSecond) > 0, "第二段在位");
    check(countInk(doc, 0, nearFirst) > 0, "第一段也在位");

    stack.undo();
    checkEqInt(countInk(doc, 0, nearSecond), 0, "撤销一次：第二段整段消失");
    check(countInk(doc, 0, nearFirst) > 0, "撤销一次：第一段仍在（粒度正确）");

    stack.undo();
    checkEqInt(countInk(doc, 0, nearFirst), 0, "再撤销一次：第一段也消失");

    stack.redo();
    check(countInk(doc, 0, nearFirst) > 0, "重做：第一段回来");

    delete doc;
}

// ===========================================================================
//  [5] 只改活动图层
// ===========================================================================
static void testActiveLayerOnly()
{
    beginGroup("只改活动图层");

    Document *doc = makeDoc(kWhite);
    CommandStack stack(doc);
    doc->addLayer(QStringLiteral("上层"));

    const int activeIdx = doc->activeLayerIndex();
    const int otherIdx  = (activeIdx == 0) ? 1 : 0;
    Layer *top   = doc->layers().layer(activeIdx);
    Layer *other = doc->layers().layer(otherIdx);

    if (!top || !other) {
        check(false, "两层文档的前置条件没建立起来");
        delete doc;
        return;
    }

    QImage topImg(kW, kH, QImage::Format_ARGB32_Premultiplied);
    topImg.fill(Qt::transparent);
    rawFillRect(topImg, QRect(0, 0, kW, kH), kClear);   // 活动层透明，方便找墨
    top->setImage(topImg);
    doc->markDirty();

    // 另一层涂成蓝色 —— 一旦被文字碰到，逐像素比对立刻红
    QImage otherImg(kW, kH, QImage::Format_ARGB32_Premultiplied);
    otherImg.fill(Qt::transparent);
    rawFillRect(otherImg, QRect(0, 0, kW, kH), kBlue);
    other->setImage(otherImg);
    doc->markDirty();

    const QImage otherBefore = other->image().copy();

    TextTool tool(doc, &stack);
    prepareText(&tool, QColor(255, 255, 0));
    check(tool.placeText(QStringLiteral("Z"), QPointF(30, 60)), "文字写进活动图层");

    check(countInk(doc, activeIdx, QRect(0, 0, kW, kH)) > 0, "活动图层上出现了墨");

    // ★ 逐像素比对另一层 ★
    //   抄近路的实现（例如对"合成结果"上写字、或写错了图层索引）
    //   在这里会立刻露馅，而"活动图层上有墨"那条照样通过。
    bool same = true;
    for (int y = 0; y < kH && same; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (rawPixel(other->image(), x, y) != rawPixel(otherBefore, x, y)) {
                same = false;
                break;
            }
        }
    }
    check(same, "另一个图层纹丝不动（逐像素比对整层）");

    delete doc;
}

// ===========================================================================
//  [6] 选区限制
// ===========================================================================
static void testSelectionClip()
{
    beginGroup("选区：文字被裁在选区内");

    // ── 前置条件：没有选区时，这段文字确实越过了 x = 100 ──
    {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));
        tool.placeText(QStringLiteral("Hello"), QPointF(10, 60));
        const QRect ink = wholeCanvasInk(doc);
        check(!ink.isEmpty() && ink.right() > 110,
              "前置条件：无选区时文字越过 x=100（否则下面的断言测不到东西）");
        delete doc;
    }

    // ── 有选区：左半边 ──
    Document *doc = makeDoc();
    CommandStack stack(doc);
    doc->selection().prepareCanvas(doc->canvasSize());
    doc->selection().selectRect(QRect(0, 0, 100, kH), Selection::Op::Replace, false);
    check(doc->selection().isActive(), "选区已生效（前置条件）");

    TextTool tool(doc, &stack);
    prepareText(&tool, QColor(255, 0, 0));
    check(tool.placeText(QStringLiteral("Hello"), QPointF(10, 60)), "文字写入（部分会在选区外）");

    check(countInk(doc, 0, QRect(0, 0, 100, kH)) > 0, "选区内有墨");
    checkEqInt(countInk(doc, 0, QRect(100, 0, kW - 100, kH)), 0,
               "选区外一个墨点都没有（选区裁剪生效）");

    delete doc;
}

// ===========================================================================
//  [7] 颜色语义：颜色自带的 alpha 就是透明度
// ===========================================================================
static void testColorSemantics()
{
    beginGroup("颜色：alpha 直接体现在预乘像素上");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    prepareText(&tool, QColor(255, 0, 0, 128));

    check(tool.placeText(QStringLiteral("H"), QPointF(30, 60)), "半透明文字写入");

    const QRect ink = wholeCanvasInk(doc);
    check(!ink.isEmpty(), "有墨（前置条件）");

    // 找覆盖度最高的那一点（字形内部），量它的 alpha
    int bestAlpha = 0, bx = -1, by = -1;
    for (int y = ink.top(); y <= ink.bottom(); ++y)
        for (int x = ink.left(); x <= ink.right(); ++x) {
            const int a = qAlpha(layerPixel(doc, 0, x, y));
            if (a > bestAlpha) { bestAlpha = a; bx = x; by = y; }
        }

    check(bx >= 0, "找到了不透明的字形内部像素");
    // 预乘空间里半透明红就是 (128,0,0,128)
    check(qAbs(bestAlpha - 128) <= 2,
          "字形内部 alpha 约等于 128（没被丢掉、也没被叠加两次）");
    const QRgb px = layerPixel(doc, 0, bx, by);
    check(qAbs(qRed(px) - 128) <= 2, "预乘后的红分量也约等于 128");
    checkEqInt(qGreen(px), 0, "绿通道保持 0");
    checkEqInt(qBlue(px), 0, "蓝通道保持 0");

    delete doc;
}

// ===========================================================================
//  [8] 对齐：选项真的走到了引擎（三种对齐只有水平位移不同）
// ===========================================================================
static void testAlignReachesEngine()
{
    beginGroup("对齐：三种对齐只有水平位移不同");

    const QPointF anchor(120, 60);
    const QString text = QStringLiteral("HH");

    // ── 先取一份"参考字体 + 参考行距"，用来算文本块的**排版宽度** ──
    //
    // ★ 为什么不能用"墨迹宽度"当基准 ★
    //   对齐位移是按 advance（排版宽度）算的，而墨迹宽度是字形实际覆盖的
    //   范围 —— 两侧各有一个 side bearing，于是墨迹比 advance 窄几个像素。
    //   拿墨迹宽度去核对位移，会凭空多出一条假红（本组第一版就踩了）。
    //   advance 只有引擎知道，所以直接用 measureTextBlock 取它。
    qreal blockWidth = 0.0;
    qreal refLineHeight = 1.0;
    {
        Document *docRef = makeDoc();
        CommandStack stackRef(docRef);
        TextTool toolRef(docRef, &stackRef);
        TextOptions *optsRef = prepareText(&toolRef, QColor(255, 0, 0));
        refLineHeight = optsRef->lineHeight();
        blockWidth = measureTextBlock(text, toolRef.editorFont(), TextAlign::Left,
                                      refLineHeight, anchor).width();
        delete docRef;
    }
    check(blockWidth > 10.0, "取到了文本块的排版宽度（前置条件）");

    auto inkFor = [&](TextAlign align) {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        TextOptions *opts = prepareText(&tool, QColor(255, 0, 0));
        opts->setAlign(align);
        tool.placeText(text, anchor);
        const QRect ink = wholeCanvasInk(doc);
        delete doc;
        return ink;
    };

    const QRect l = inkFor(TextAlign::Left);
    const QRect c = inkFor(TextAlign::Center);
    const QRect r = inkFor(TextAlign::Right);

    check(!l.isEmpty() && !c.isEmpty() && !r.isEmpty(), "三种对齐都产生了墨");

    // ★ 高度与宽度必须完全一样 ★
    //   这条挡住"对齐顺手把内容改了"（例如居中时另算了一遍字宽，
    //   或者把行距/字号也一起按位置缩放了）。
    checkEqInt(c.height(), l.height(), "居中与左对齐高度相同");
    checkEqInt(r.height(), l.height(), "右对齐与左对齐高度相同");
    check(std::abs(c.width() - l.width()) <= 1, "居中与左对齐宽度相同");
    check(std::abs(r.width() - l.width()) <= 1, "右对齐与左对齐宽度相同");
    checkEqInt(c.top(), l.top(), "三种对齐顶边相同（锚点在顶部）");
    checkEqInt(r.top(), l.top(), "三种对齐顶边相同（锚点在顶部）");

    // ★ 只有水平位移不同，且位移量恰好是半个 / 一个"排版宽度" ★
    //   若对齐在引擎里被施加了两遍（例如工具先按对齐挪一次、引擎又挪一次），
    //   位移会变成 1 倍 / 2 倍宽 —— 这两条会立刻红。
    //   若对齐选项根本没传进引擎，三个 ink 会完全重合。
    check(std::abs(c.left() - (l.left() - blockWidth / 2.0)) <= 2,
          "居中：比左对齐左移半个文本块宽（对齐只施加一次）");
    check(std::abs(r.left() - (l.left() - blockWidth)) <= 2,
          "右对齐：比左对齐左移一个文本块宽（对齐只施加一次）");

    // 锚点语义：三种对齐的基准边都落在 anchor.x 上
    checkNear(l.left(), anchor.x(), 6, "左对齐：墨的左边缘 ≈ 锚点 x");
    checkNear(c.center().x(), anchor.x(), 6, "居中：墨的水平中心 ≈ 锚点 x");
    checkNear(r.right(), anchor.x(), 6, "右对齐：墨的右边缘 ≈ 锚点 x");
}

// ===========================================================================
//  [9] 行距 / 字距：参数走到引擎，且**只施加一次**
// ===========================================================================
static void testLineHeightAndSpacing()
{
    beginGroup("行距与字距：参数生效且不重复施加");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    TextOptions *opts = prepareText(&tool, QColor(255, 0, 0));

    // ── 行距 ──
    //
    // ★ 差值必须恰好 ≈ 一个字体行高 ★
    //   引擎的高度公式是 "(n-1) × fm.height() × 行距 + fm.height()"，
    //   所以 2.0 与 1.0 的高度差 == fm.height()。
    //   若行距被乘了两遍，差值会翻倍 —— 上界因此是有意义的一半。
    opts->setLineHeight(1.0);
    const QRectF one = measureTextBlock(QStringLiteral("H\nH"), tool.editorFont(),
                                        opts->align(), opts->lineHeight(), QPointF(0, 0));
    opts->setLineHeight(2.0);
    const QRectF two = measureTextBlock(QStringLiteral("H\nH"), tool.editorFont(),
                                        opts->align(), opts->lineHeight(), QPointF(0, 0));

    const qreal fmHeight = QFontMetricsF(tool.editorFont()).height();
    check(two.height() > one.height() + 10, "行距变大 → 文本块变高");
    checkNear(two.height() - one.height(), fmHeight, 4,
              "行距 1.0→2.0 的高度差 ≈ 一个字体行高（乘了一次，不是两次）");
    check(std::abs(two.width() - one.width()) <= 1, "行距不影响宽度");

    // ── 行距下限钳制 ──
    opts->setLineHeight(1.0);
    const QRectF zero = measureTextBlock(QStringLiteral("H\nH"), tool.editorFont(),
                                         opts->align(), 0.0, QPointF(0, 0));
    check(zero.height() > 0 && zero.width() > 0,
          "行距退化成 0 时仍算出非空矩形（引擎下限钳到 0.1，否则一个字都不画）");

    // ── 字距由字体携带 ★ ──
    //
    //   ★ 这是"同一件事两处真相"的守门 ★
    //     TextOptions::toFont 把"像素字距"换算成百分比写进 QFont；
    //     engine/Text 只管排版、绝不再设一次字距。若引擎里也设一遍，
    //     下面的宽度增量会翻倍。
    opts->setLetterSpacing(0.0);
    const QRectF s0 = measureTextBlock(QStringLiteral("HHHH"), tool.editorFont(),
                                       opts->align(), opts->lineHeight(), QPointF(0, 0));

    opts->setLetterSpacing(16.0);
    const QFont spaced = tool.editorFont();
    checkEqInt(int(spaced.letterSpacingType()), int(QFont::PercentageSpacing),
               "字距以百分比写进 QFont（缩放字号时字距等比变化）");
    check(spaced.letterSpacing() > 100.0,
          "正字距 → QFont 的百分比 > 100（换算发生在 TextOptions::toFont 里）");

    const QRectF s1 = measureTextBlock(QStringLiteral("HHHH"), tool.editorFont(),
                                       opts->align(), opts->lineHeight(), QPointF(0, 0));
    const qreal grow = s1.width() - s0.width();
    check(grow > 20.0, "字距让文本变宽（参数 → 字体 → 排版这条链是通的）");
    check(grow < 110.0,
          "宽度增量在合理范围内（≈4×16px，而不是再翻一倍——字距只施加一次）");
    check(std::abs(s1.height() - s0.height()) <= 1, "字距不影响高度");

    opts->setLetterSpacing(0.0);
    // ★ Qt 的默认值不是 100 ★
    //   未显式 setLetterSpacing 时 letterSpacing() 返回 0（而不是 100%）。
    //   所以"没有额外字距"的判据是 0；写成 `== 100` 会是一条假红
    //   （本组第一版就是这么错的）。
    const qreal ls0 = tool.editorFont().letterSpacing();
    check(qFuzzyIsNull(ls0),
          "字距 0 时不往字体里塞字距（Qt 未设置时返回 0，不是 100）");

    delete doc;

    // ── 行距的**像素级**验证（整条链：参数 → 工具 → 引擎）──
    //
    // ★ 上面那两条走的是 measureTextBlock，只证明了**引擎**算得对 ★
    //   "工具有没有把 lineHeight 真的传进 rasterizeText"这条断言看不到 ——
    //   传成常量 1.0 的话，浮层（拿的是选项的值）与落到图层的结果会不一致。
    //   这条走 placeText，量的是**真实像素**，把整条链一次钉住。
    {
        Document *docA = makeDoc();
        CommandStack stackA(docA);
        TextTool toolA(docA, &stackA);
        prepareText(&toolA, QColor(255, 0, 0))->setLineHeight(1.0);
        toolA.placeText(QStringLiteral("H\nH"), QPointF(30, 20));
        const QRect inkA = wholeCanvasInk(docA);

        Document *docB = makeDoc();
        CommandStack stackB(docB);
        TextTool toolB(docB, &stackB);
        prepareText(&toolB, QColor(255, 0, 0))->setLineHeight(2.0);
        toolB.placeText(QStringLiteral("H\nH"), QPointF(30, 20));
        const QRect inkB = wholeCanvasInk(docB);

        check(!inkA.isEmpty() && !inkB.isEmpty(), "两种行距都产生了墨（前置条件）");
        checkNear(inkB.height() - inkA.height(), fmHeight, 6,
                  "像素级：行距 1.0→2.0 的墨迹高度差 ≈ 一个字体行高"
                  "（工具真的把行距传下去了）");
        check(std::abs(inkB.width() - inkA.width()) <= 1, "像素级：行距不影响宽度");

        delete docA;
        delete docB;
    }
}

// ===========================================================================
//  [10] 取消 / 提交（Esc 与"切换工具"是两条不同的收尾路径）
// ===========================================================================
static void testCancelAndCommit()
{
    beginGroup("取消 / 提交 / 空文本");

    // ── Esc：什么都不留 ──
    {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));

        tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60));
        tool.setEditingText(QStringLiteral("Discarded"));
        check(tool.isEditing(), "编辑会话进行中（前置条件）");

        tool.cancel();
        check(!tool.hasInlineEditor(), "取消后编辑会话结束");
        check(!tool.isBusy(), "取消后不是进行中状态");
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
                   "取消不产生撤销记录");
        checkEqInt(countInk(doc, 0, QRect(0, 0, kW, kH)), 0,
                   "取消后画布上一个墨点都没有（文字还没落过像素）");
        delete doc;
    }

    // ── 切换工具（commit）：留下并压栈 ──
    {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));

        tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60));
        tool.setEditingText(QStringLiteral("Kept"));
        tool.commit();

        check(!tool.hasInlineEditor(), "提交后编辑会话结束");
        check(countInk(doc, 0, QRect(0, 0, kW, kH)) > 0, "提交后文字留在图层上");
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
                   "提交把这一条压进撤销栈");

        // ★ 重复 commit 不该再压一条 ★
        tool.commit();
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
                   "再次 commit 不重复压栈（状态已摘干净）");

        // ★ 晚到的 setEditingText 不该污染下次会话 ★
        tool.setEditingText(QStringLiteral("Late"));
        check(tool.editingText().isEmpty(),
              "会话结束后的 setEditingText 被挡住（否则旧文本会残留到下一次）");
        delete doc;
    }

    // ── 空文本：开了浮层一个字没敲 ──
    {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));

        tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60));
        tool.commit();     // 文本一直是空的
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
                   "空文本**不压**命令（否则 Ctrl+Z 看起来没反应）");
        checkEqInt(countInk(doc, 0, QRect(0, 0, kW, kH)), 0, "空文本不改像素");
        delete doc;
    }

    // ── 纯空白：有 advance 有包围盒，但没有字形 ──
    //
    //   ★ 这条与"空文本"是两条不同的路径 ★
    //     空文本在工具层就被 text.isEmpty() 拦下；纯空白会一路走到 engine，
    //     由蒙版的"没有墨"判据拦下（见 Text.h 的 maskHasInk）。
    //     两条都必须在**压命令之前**拦住。
    {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));

        tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60));
        tool.setEditingText(QStringLiteral("   \n  "));
        tool.commit();
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
                   "纯空白文本不压命令（engine 的「没有墨」判据拦住了）");
        checkEqInt(countInk(doc, 0, QRect(0, 0, kW, kH)), 0, "纯空白不改像素");
        delete doc;
    }
}

// ===========================================================================
//  [11] 鼠标第二次点击：先把上一段提交掉
// ===========================================================================
static void testSecondClickCommits()
{
    beginGroup("第二次点击：先提交上一段");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    prepareText(&tool, QColor(255, 0, 0));

    tool.mousePress(mouseEvent(ToolEventType::Press, 20, 40));
    tool.setEditingText(QStringLiteral("First"));
    check(tool.isEditing(), "第一段在编辑中");

    // 点别处 = 这段写完了，同时开始下一段（与 Photoshop 一致）
    const ToolResult r = tool.mousePress(mouseEvent(ToolEventType::Press, 140, 120));

    check(r.handled, "第二次点击被消费");
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
               "第一段被提交（压了一条），第二段还没压");
    check(countInk(doc, 0, QRect(0, 20, 90, 70)) > 0, "第一段的墨在它自己的位置上");
    checkEqInt(countInk(doc, 0, QRect(120, 100, 110, 80)), 0,
               "第二段还没敲字，所以那个区域还是空的");
    check(tool.isEditing(), "已经开出了新的编辑会话");
    check(tool.editingText().isEmpty(), "新会话的文本是空的（没被上一段污染）");
    check(tool.editAnchor() == QPointF(140, 120), "新会话的锚点是第二次点击处");

    // 第二段落字
    tool.setEditingText(QStringLiteral("Second"));
    tool.commit();
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 2, "两段共两条撤销记录");
    check(countInk(doc, 0, QRect(120, 100, 110, 80)) > 0, "第二段也落在自己的位置上");

    delete doc;
}

// ===========================================================================
//  [12] 换文档：编辑会话必须收掉（且按"取消"处理）
// ===========================================================================
static void testSetDocumentCancels()
{
    beginGroup("换文档：编辑会话按取消收掉");

    Document *docA = makeDoc();
    CommandStack stackA(docA);
    TextTool tool(docA, &stackA);
    prepareText(&tool, QColor(255, 0, 0));

    tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60));
    tool.setEditingText(QStringLiteral("Half typed"));
    check(tool.isEditing(), "在 docA 上编辑中（前置条件）");

    Document *docB = makeDoc();
    CommandStack stackB(docB);

    tool.setDocument(docB);

    // ★ 为什么按"取消"而不是"提交" ★
    //   浮层是挂在**旧**画布上的，锚点在新文档里可能不存在。
    //   若提交，字会写进旧文档，而用户立刻切走了 —— 不可见、不可撤销，
    //   是最难排查的那类 bug。
    check(!tool.hasInlineEditor(), "换文档后编辑会话被收掉");
    check(!tool.isEditing(), "换文档后不再是编辑中");
    checkEqInt(static_cast<int>(stackA.undoStack()->count()), 0,
               "**没有**往旧文档压命令（按取消处理）");
    checkEqInt(countInk(docA, 0, QRect(0, 0, kW, kH)), 0, "旧文档一个墨点都没多");
    checkEqInt(countInk(docB, 0, QRect(0, 0, kW, kH)), 0, "新文档也干净");

    // 换完之后工具在新文档上照常工作
    tool.setCommandStack(&stackB);
    prepareText(&tool, QColor(255, 0, 0));
    check(tool.placeText(QStringLiteral("New"), QPointF(30, 60)), "换文档后能正常写入");
    checkEqInt(static_cast<int>(stackB.undoStack()->count()), 1, "命令压进了新文档的栈");

    delete docA;
    delete docB;
}

// ===========================================================================
//  [13] 拒绝路径（必须是拒绝，不能是"写了一半"）
// ===========================================================================
static void testRefusals()
{
    beginGroup("拒绝路径");

    // ── 没有撤销栈：连浮层都不该开 ──
    //
    // ★ 为什么"连浮层都不开"★
    //   让用户敲完一段字、确认之后才发现写不进去，比一开始就不让敲更糟。
    //   油漆桶与形状工具是同一条纪律。
    {
        Document *doc = makeDoc();
        TextTool tool(doc, nullptr);
        prepareText(&tool, QColor(255, 0, 0));

        EditorReq req;
        capture(&tool, &req);

        check(!tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60)).handled,
              "没有撤销栈时拒绝开始输入");
        check(!req.fired, "拒绝了就不该发 requestTextEditor（别打空浮层）");
        check(!tool.isEditing(), "拒绝后不在编辑会话中");
        check(!tool.placeText(QStringLiteral("H"), QPointF(30, 60)),
              "没有撤销栈时也拒绝直接写入");
        checkEqInt(countInk(doc, 0, QRect(0, 0, kW, kH)), 0, "拒绝之后画布干净");
        delete doc;
    }

    // ── 图层锁定 / 隐藏 ──
    {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));

        doc->activeLayer()->setLocked(true);
        check(!tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60)).handled,
              "锁定图层拒绝开始输入");
        check(!tool.placeText(QStringLiteral("H"), QPointF(30, 60)), "锁定图层拒绝写入");

        doc->activeLayer()->setLocked(false);
        doc->activeLayer()->setVisible(false);
        check(!tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60)).handled,
              "隐藏图层拒绝开始输入");
        check(!tool.placeText(QStringLiteral("H"), QPointF(30, 60)), "隐藏图层拒绝写入");

        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0, "拒绝路径一条命令都没压");
        checkEqInt(countInk(doc, 0, QRect(0, 0, kW, kH)), 0, "图层上什么都没写");
        delete doc;
    }

    // ── 没有文档（程序启动早期 / 文档全部关闭）──
    {
        TextTool tool(nullptr, nullptr);
        prepareText(&tool, QColor(255, 0, 0));

        check(!tool.mousePress(mouseEvent(ToolEventType::Press, 30, 60)).handled,
              "没有文档时安全拒绝（不崩）");
        check(!tool.placeText(QStringLiteral("H"), QPointF(30, 60)),
              "没有文档时 placeText 也安全拒绝");
        check(!tool.isBusy(), "没有文档时不该进入进行中状态");
    }

    // ── 文字整块在画布外 ──
    //
    // ★ 必须给提示而不是静默 ★
    //   静默失败会让用户以为"确认按钮坏了"。本组只能断到"不产生改动"，
    //   提示本身走 statusMessage 信号（在 [14] 里顺带看一眼）。
    {
        Document *doc = makeDoc();
        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));

        check(!tool.placeText(QStringLiteral("H"), QPointF(-500, -500)),
              "整块在画布外：不产生改动");
        check(!tool.placeText(QStringLiteral("H"), QPointF(kW + 50, kH + 50)),
              "右下方向出界同样拒绝");
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0, "画布外不压命令");
        checkEqInt(countInk(doc, 0, QRect(0, 0, kW, kH)), 0, "画布上什么都没留下");
        delete doc;
    }

    // ── 退化画布（1×1）──
    //
    // ★ 为什么只能测到 1×1 ★
    //   Document 的构造函数把"0 或无效尺寸"兜底成 1×1（见 Document.cpp：
    //   0 尺寸会让后续所有计算失效），所以"画布尺寸为空"那条分支
    //   在公开构造下**不可达** —— 它是纯防御，写不出测试来。
    //   这里退而求其次：1×1 画布上写 48px 的字，必须安全拒绝、不崩。
    {
        auto *doc = new Document(QSize(0, 0), Qt::transparent);
        checkEqInt(doc->canvasSize().width(), 1, "0 尺寸被兜底成 1×1（前置条件）");

        CommandStack stack(doc);
        TextTool tool(doc, &stack);
        prepareText(&tool, QColor(255, 0, 0));

        check(!tool.placeText(QStringLiteral("H"), QPointF(0, 0)),
              "1×1 画布上写 48px 的字：安全拒绝（字形整个落在画布外）");
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0, "不压空命令");
        delete doc;
    }
}

// ===========================================================================
//  [14] 状态栏提示：出界的文字要吭一声
// ===========================================================================
static void testStatusMessage()
{
    beginGroup("状态栏：该吭声的时候要吭声");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    prepareText(&tool, QColor(255, 0, 0));

    int messages = 0;
    QObject::connect(&tool, &AbstractTool::statusMessage,
                     [&messages](const QString &, int) { ++messages; });

    // 出界 → 提示"在画布之外"（否则用户以为确认按钮坏了）
    tool.placeText(QStringLiteral("H"), QPointF(-500, -500));
    checkEqInt(messages, 1, "文字整块在画布外时给了一条状态栏提示");

    // 空文本 → **不提示**（"什么都没发生"的提示看起来像报错）
    tool.placeText(QString(), QPointF(30, 60));
    checkEqInt(messages, 1, "空文本不提示（静默是正确行为）");

    // 纯空白 → 也不提示（同上：正常结果，不是错误）
    tool.placeText(QStringLiteral("   "), QPointF(30, 60));
    checkEqInt(messages, 1, "纯空白同样不提示");

    // 正常写入 → 不提示
    tool.placeText(QStringLiteral("H"), QPointF(30, 60));
    checkEqInt(messages, 1, "正常写入不打扰用户");

    delete doc;
}

// ===========================================================================
//  [15] 前景色同步
// ===========================================================================
static void testForegroundColor()
{
    beginGroup("前景色 → 文字颜色");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    TextTool tool(doc, &stack);
    TextOptions *opts = prepareText(&tool, Qt::black);

    tool.setForegroundColor(QColor(0, 255, 0));
    check(opts->color() == QColor(0, 255, 0), "前景色推到参数的当前颜色上");

    tool.placeText(QStringLiteral("H"), QPointF(30, 60));
    const QRect ink = wholeCanvasInk(doc);
    check(!ink.isEmpty(), "用新颜色写下了文字");

    const QRgb px = layerPixel(doc, 0, ink.center().x(), ink.center().y());
    check(qGreen(px) > 200 && qRed(px) < 60 && qBlue(px) < 60,
          "墨的颜色就是新的前景色（绿）");

    // 无效颜色不该把当前颜色搞坏
    tool.setForegroundColor(QColor());
    check(opts->color() == QColor(0, 255, 0), "无效颜色被忽略（不把颜色清成无效值）");

    delete doc;
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char **argv)
{
    // ★ 必须在这两件事之前设：Qt 读字体目录发生在 QGuiApplication 构造时 ★
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        // Qt 5.15 起不再自带字体，offscreen 平台默认找不到任何字体目录 ——
        // 症状是"所有蒙版都是空的"，而**不是**报错（见 text_test 的说明）。
        qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    }
    QApplication app(argc, argv);

    testIdentity();
    testEditorProtocol();
    testPlaceText();
    testUndoGranularity();
    testActiveLayerOnly();
    testSelectionClip();
    testColorSemantics();
    testAlignReachesEngine();
    testLineHeightAndSpacing();
    testCancelAndCommit();
    testSecondClickCommits();
    testSetDocumentCancels();
    testRefusals();
    testStatusMessage();
    testForegroundColor();

    // ★ 这一行的格式与其它测试保持一致 ★
    //   植入验证脚本按这个模式解析结果 —— 改了它，所有植入验证都会
    //   因为"解析不出来"而集体失效（表现为 (-1,-1)，看着像脚本坏了）。
    std::printf("\n================================================\n");
    std::printf("  text_tool_test：通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
