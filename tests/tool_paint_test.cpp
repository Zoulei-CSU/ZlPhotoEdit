// ============================================================================
//  tool_paint_test.cpp —— 吸管与油漆桶的离线自检
// ----------------------------------------------------------------------------
//  engine/Fill 那一组（tests/fill_test.cpp）已经把**算法**钉住了：
//  扫描线连通性、容差刻度、透明与不透明黑的区分、边缘过渡带。
//  本组管的是**算法之外**的那些事 —— 也就是"算对了但用错了"。
//
//  这一类错误编译通过、程序不崩、算法测试全绿，症状只是"填出来的东西
//  和看到的不一样"，而且不可复现的解读：
//
//    ① **判据取错了图层**
//       油漆桶要回答"在**这一层**上哪些像素与起点算同一块"，
//       吸管要回答"我**看到**的这个点是什么颜色"。
//       两者恰好相反。取反了会得到一个"看起来只是偶尔奇怪"的工具：
//       在活动层是透明的地方点油漆桶，填出来的形状会溢出用户看到的边界。
//       本测试用"活动层均匀透明、下层左右两半不同色"来区分两者 ——
//       判据取自活动层时填满整张画布，取自合成图时只填左半边。
//       这是**唯一**能一眼看出取哪张图的方法。
//
//    ② **撤销粒度错乱**（最隐蔽）
//       油漆桶"点一下 = 一条撤销记录"，这是它与画笔最大的差别。
//       而 DrawCommand 的描边编号默认取自一个**没人推进**的全局量，
//       所以相邻两条命令的 id 相同 → QUndoStack 会把它们 mergeWith 成一条。
//       症状：连点两下油漆桶，Ctrl+Z 一次把两处都撤掉；
//       更糟的是画笔也受同一个机制影响 —— 画两笔只算一步。
//       本测试用"两次独立填充 → 撤销栈两条 → 逐条撤销逐条还原"钉住它。
//
//    ③ **不透明度 0% 却填满了整张图**
//       QColor(QRgb) 那个构造函数会把 alpha 丢掉、置成 255。
//       于是"填充不透明度 50%"变成 100%，而且没有任何征兆。
//       本测试直接量预乘像素的 alpha 分量。
//
//    ④ **吸管取到纯白**
//       预乘空间里 A=0 意味着 RGB 也是 0，"还原颜色"要除以 0。
//       硬除会得到巨大值再被钳到 255 —— 取一块透明区域得到纯白，
//       一个凭空冒出来的错误颜色。
//
//  ★ 关于画布与配色 ★
//    一律用不透明色与灰阶 —— 灰阶上色差与灰阶差是 1:1（见 engine/Fill.h），
//    于是"容差 20 能不能吃掉灰阶差 10"这类断言可以手算验证，不依赖实现细节。
//    需要造半透明像素时直接写**预乘值**（qRgba(128,0,0,128) 表示
//    "反预乘后是纯红、alpha 128"），不用 setPixel 绕一圈。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 toolpaint 分支。需要 QApplication（ToolOptions.cpp
//    与工具都链 QtWidgets），用 offscreen 插件在无显示器环境里跑。
// ============================================================================

#include "tools/BucketTool.h"
#include "tools/ColorPickerTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolEvent.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "commands/CommandStack.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QtGlobal>

#include <cstdio>

using namespace zl;

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

// 浮点算完再取整，允许 ±tol 的误差。tol 默认 0（要求逐位相同）——
// 只有在期望值是**手算出来的浮点结果**时才放宽，并在断言里写清容差来源。
static void checkRgbNear(QRgb actual, QRgb expected, int tol, const char *what)
{
    const bool ok = qAbs(qAlpha(actual) - qAlpha(expected)) <= tol
                    && qAbs(qRed(actual)   - qRed(expected))   <= tol
                    && qAbs(qGreen(actual) - qGreen(expected)) <= tol
                    && qAbs(qBlue(actual)  - qBlue(expected))  <= tol;
    if (!ok) {
        ++g_failed;
        std::printf("  [失败] %s  (容差 ±%d)\n         期望 a=%d r=%d g=%d b=%d，"
                    "实际 a=%d r=%d g=%d b=%d\n", what, tol,
                    qAlpha(expected), qRed(expected), qGreen(expected), qBlue(expected),
                    qAlpha(actual),   qRed(actual),   qGreen(actual),   qBlue(actual));
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  像素读写
// ---------------------------------------------------------------------------
//  ★ 一律走扫描线，不用 QImage::pixel() / setPixel() ★
//    setPixel() 对预乘格式会"先预乘再写"，pixel() 又会"反预乘再读"，
//    两次隐式转换叠在一起，量到的就不是我们写进去的那个值了。
static QRgb rawPixel(const QImage &img, int x, int y)
{
    return reinterpret_cast<const QRgb *>(img.constScanLine(y))[x];
}

static void rawSet(QImage &img, int x, int y, QRgb c)
{
    reinterpret_cast<QRgb *>(img.scanLine(y))[x] = c;
}

static void rawFillRect(QImage &img, const QRect &r, QRgb c)
{
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            rawSet(img, x, y, c);
}

// 读图层位图（预乘空间）。
// ★ 用 mutableImage() 取"图层自己那份"，而不是 setImage 时那个局部变量 ★
//   QImage 是隐式共享的：setImage 后图层与局部变量**共享**同一块缓冲，
//   此时若写局部变量，scanLine() 会触发 COW 分离 —— 图层里的内容纹丝不动，
//   而测试会以为自己改过了。（这类"改了但没生效"的静默现象本测试必须绕开。）
static QRgb layerPixel(const Document *doc, int layerIndex, int x, int y)
{
    const Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return 0;
    return rawPixel(l->image(), x, y);
}

static void layerSetPixel(Document *doc, int layerIndex, int x, int y, QRgb c)
{
    Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return;
    rawSet(l->mutableImage(), x, y, c);
}

static void layerFillRect(Document *doc, int layerIndex, const QRect &r, QRgb c)
{
    Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return;
    rawFillRect(l->mutableImage(), r, c);
}

// ---------------------------------------------------------------------------
//  构造辅助
// ---------------------------------------------------------------------------

static const int kW = 64;
static const int kH = 48;

// 一张单图层文档，图层底色为 fill（**直接写预乘字节**）
//
// ★ 为什么不写 fill(QColor(...)) ★
//   QImage::fill(const QColor &) 并不会把颜色预乘一遍，它是把 QRgb
//   原样写进位图。也就是说 QColor(128,0,0,128) 在预乘格式里落下的是
//   (128,0,0,128)，语义为"反预乘后是纯红、alpha 128"；而
//   QColor(255,0,0,128) 落下的也是 (128,0,0,128)。
//   两句话听起来都对，但依赖它会让"我到底写了什么"变得要靠实测才知道。
//   测试数据必须是把每个字节都写清楚的那种，所以这里绕开 QColor，
//   直接对整张位图写 QRgb —— 参数 fill 就是位图里的原始预乘值。
static Document *makeDoc(QRgb fill)
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

// 在活动图层上画一块实心矩形并让合成缓存失效
static void paintBlock(Document *doc, const QRect &r, QRgb c)
{
    layerFillRect(doc, doc->activeLayerIndex(), r, c);
    doc->markDirty();
}

static ToolEvent makeEvent(ToolEventType type, int x, int y)
{
    ToolEvent e;
    e.type = type;

    // ★ button 与 buttons 不是一回事，别把释放事件写成 NoButton ★
    //   真实 Qt 事件里：button() = "造成这次事件的那个键"，
    //   buttons() = "此刻仍然按着的所有键"。
    //   所以**松开事件是 button=左键、buttons=空**。
    //   写成 button=NoButton 的话，工具里 `if (e.button != Qt::LeftButton) return;`
    //   会直接早退 —— 状态机永远停不下来，症状是"松手后拖动还在继续"。
    //   （CanvasView 就是按 event->button() / event->buttons() 填的。）
    e.button  = Qt::LeftButton;
    e.buttons = (type == ToolEventType::Release) ? Qt::NoButton : Qt::LeftButton;
    e.imagePos = QPointF(x, y);
    e.widgetPos = QPointF(x, y);
    e.lastImagePos = QPointF(x, y);
    e.viewZoom = 1.0;
    return e;
}

// 把油漆桶的选项配成"判据最干脆"的一档：容差 0、无抗锯齿、
// 不透明 100%。这样"哪些像素属于区域"完全由像素值决定，不掺过渡带。
static BucketOptions *prepareBucket(BucketTool *tool, const QColor &color)
{
    auto *opts = qobject_cast<BucketOptions *>(tool->options());
    if (!opts)
        return nullptr;
    opts->setTolerance(0);
    opts->setAntialias(false);
    opts->setContiguous(true);
    opts->setOpacity(100);
    opts->setColor(color);
    return opts;
}

static const QRgb kWhite = qRgba(255, 255, 255, 255);
static const QRgb kBlack = qRgba(0, 0, 0, 255);
static const QRgb kRed   = qRgba(255, 0, 0, 255);
static const QRgb kBlue  = qRgba(0, 0, 255, 255);
static const QRgb kClear = qRgba(0, 0, 0, 0);

// ===========================================================================
//  [1] 身份证
// ===========================================================================
static void testIdentity()
{
    beginGroup("两个工具的身份证");

    Document *doc = makeDoc(kWhite);
    CommandStack stack(doc);

    BucketTool bucket(doc, &stack);
    check(bucket.id() == QStringLiteral("bucket"),
          "油漆桶 id 是 bucket（QSettings 键名，定了就不能改）");
    check(bucket.shortcutKey() == Qt::Key_G, "油漆桶快捷键是 G（对齐 Photoshop）");
    check(!bucket.displayName().isEmpty(), "油漆桶有显示名");
    check(qobject_cast<BucketOptions *>(bucket.options()) != nullptr,
          "油漆桶的参数对象就是 BucketOptions");

    ColorPickerTool picker(doc, &stack);
    check(picker.id() == QStringLiteral("eyedropper"),
          "吸管 id 是 eyedropper（与 iconForTool 的映射键一致）");
    check(picker.shortcutKey() == Qt::Key_I, "吸管快捷键是 I（对齐 Photoshop）");
    check(!picker.displayName().isEmpty(), "吸管有显示名");
    check(qobject_cast<PickerOptions *>(picker.options()) != nullptr,
          "吸管的参数对象就是 PickerOptions");

    // 两个工具都不写像素/不进撤销栈的能力差异，靠"有无 options"看不出来，
    // 但可以确认它们能安全地在没有撤销栈时构造（文档关闭的瞬间会这样）。
    BucketTool noStack(doc, nullptr);
    check(noStack.fillAt(QPoint(1, 1)) == false,
          "没有撤销栈时油漆桶拒绝填充（而不是留下撤不掉的痕迹）");

    ColorPickerTool pickerNoStack(doc, nullptr);
    check(pickerNoStack.pickAt(QPoint(1, 1)).isValid(),
          "没有撤销栈时吸管照常工作（它本来就不改数据）");

    delete doc;
}

// ===========================================================================
//  [2] 填充的区域形状
// ===========================================================================
static void testRegionShape()
{
    beginGroup("油漆桶：填的是哪一块");

    Document *doc = makeDoc(kWhite);
    CommandStack stack(doc);

    // 白底上画一个 10×10 的黑方块，容差 0 → 只有黑方块该被填。
    const QRect block(5, 5, 10, 10);
    paintBlock(doc, block, kBlack);

    BucketTool tool(doc, &stack);
    prepareBucket(&tool, QColor(255, 0, 0));
    check(tool.fillAt(QPoint(10, 10)), "在方块内部点一下，填充成功");

    checkRgb(layerPixel(doc, 0, 5, 5),   kRed,  "方块左上角被填成红");
    checkRgb(layerPixel(doc, 0, 14, 14), kRed,  "方块右下角被填成红");
    checkRgb(layerPixel(doc, 0, 10, 10), kRed,  "方块中心被填成红");

    // 边界外一个像素都不能溢出 —— 差一格就说明坐标或半径算错了
    checkRgb(layerPixel(doc, 0, 4, 10),  kWhite, "方块左边一格未被填");
    checkRgb(layerPixel(doc, 0, 15, 10), kWhite, "方块右边一格未被填");
    checkRgb(layerPixel(doc, 0, 10, 4),  kWhite, "方块上面一格未被填");
    checkRgb(layerPixel(doc, 0, 10, 15), kWhite, "方块下面一格未被填");
    checkRgb(layerPixel(doc, 0, 0, 0),   kWhite, "画布角落未被填");

    checkEqInt(stack.undoStack()->count(), 1, "一次填充 = 一条撤销记录");

    delete doc;
}

// ===========================================================================
//  [3] 只改活动图层
// ===========================================================================
static void testOnlyActiveLayer()
{
    beginGroup("油漆桶：只动活动图层");

    Document *doc = makeDoc(kWhite);                 // 图层 0：白底
    const int top = doc->addLayer(QStringLiteral("上层"));
    checkEqInt(top, 1, "新图层加在活动图层上方");

    layerFillRect(doc, top, QRect(0, 0, kW, kH), kClear);
    paintBlock(doc, QRect(5, 5, 10, 10), kBlack);    // 上层：透明底 + 黑方块
    doc->setActiveLayerIndex(top);

    CommandStack stack(doc);
    BucketTool tool(doc, &stack);
    prepareBucket(&tool, QColor(255, 0, 0));
    check(tool.fillAt(QPoint(10, 10)), "在上层填充成功");

    checkRgb(layerPixel(doc, top, 10, 10), kRed,   "上层黑方块变成红");
    checkRgb(layerPixel(doc, 0,   10, 10), kWhite, "下层同一位置仍是白（没被串改）");
    checkRgb(layerPixel(doc, top, 20, 20), kClear, "上层方块之外的透明区不受影响");

    delete doc;
}

// ===========================================================================
//  [4] 判据取自活动图层，不取自合成图 ★ 本工具最重要的一个设计决定
// ===========================================================================
static void testCriterionSource()
{
    beginGroup("油漆桶：判据取自活动图层而不是合成图");

    // 下层：左半边白、右半边黑（合成图上有明显的边界）
    Document *doc = makeDoc(kWhite);
    paintBlock(doc, QRect(kW / 2, 0, kW / 2, kH), kBlack);

    // 上层：整层透明。合成图看着是"左白右黑"，但上层自己**处处同色**。
    const int top = doc->addLayer(QStringLiteral("空层"));
    layerFillRect(doc, top, QRect(0, 0, kW, kH), kClear);
    doc->markDirty();
    doc->setActiveLayerIndex(top);

    CommandStack stack(doc);
    BucketTool tool(doc, &stack);
    prepareBucket(&tool, QColor(0, 0, 255));

    // 起点落在合成图的"白色"一侧
    check(tool.fillAt(QPoint(5, 5)), "在空图层上点一下，填充成功");

    // ★ 关键断言 ★
    //   判据取自合成图 → 区域止于左右分界，右半边不该被填；
    //   判据取自活动图层 → 活动层处处同色，整张画布都该被填。
    //   两者的区别只在这一个像素上，所以这一条不是"顺便测测"，
    //   它是这一组测试存在的理由。
    checkRgb(layerPixel(doc, top, kW - 5, 5), kBlue,
             "整层被填满（说明判据取自活动图层，不是合成图）");
    checkRgb(layerPixel(doc, top, 0, 0), kBlue, "左上角也被填");

    delete doc;
}

// ===========================================================================
//  [5] 容差
// ===========================================================================
static void testTolerance()
{
    beginGroup("油漆桶：容差刻度");

    // 白底上放一块灰 110 的方块；起点在灰 100 的背景上。
    // 灰阶上色差与灰阶差 1:1（见 engine/Fill.h），所以"容差 10 vs 20"
    // 对应"灰阶差 10"，可以手算。
    const QRgb bg   = qRgba(100, 100, 100, 255);
    const QRgb patch = qRgba(110, 110, 110, 255);
    const QRect pr(20, 10, 10, 10);

    {
        Document *doc = makeDoc(bg);
        paintBlock(doc, pr, patch);
        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        auto *opts = prepareBucket(&tool, QColor(255, 0, 0));
        opts->setTolerance(5);

        check(tool.fillAt(QPoint(1, 1)), "容差 5：填充成功");
        checkRgb(layerPixel(doc, 0, 25, 15), patch,
                 "色差 10 > 容差 5 → 灰块留在外面");
        checkRgb(layerPixel(doc, 0, 1, 1), kRed, "起点所在的背景被填");

        delete doc;
    }
    {
        Document *doc = makeDoc(bg);
        paintBlock(doc, pr, patch);
        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        auto *opts = prepareBucket(&tool, QColor(255, 0, 0));
        opts->setTolerance(20);

        check(tool.fillAt(QPoint(1, 1)), "容差 20：填充成功");
        checkRgb(layerPixel(doc, 0, 25, 15), kRed,
                 "色差 10 <= 容差 20 → 灰块被吃掉");

        delete doc;
    }
}

// ===========================================================================
//  [6] 连续 / 非连续
// ===========================================================================
static void testContiguous()
{
    beginGroup("油漆桶：连续与不连续");

    const QRect left(5, 5, 5, 5);
    const QRect right(20, 5, 5, 5);

    {
        Document *doc = makeDoc(kWhite);
        paintBlock(doc, left, kBlack);
        paintBlock(doc, right, kBlack);

        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        auto *opts = prepareBucket(&tool, QColor(255, 0, 0));
        opts->setContiguous(true);

        check(tool.fillAt(QPoint(7, 7)), "连续模式：填充成功");
        checkRgb(layerPixel(doc, 0, 7, 7),  kRed,   "起点所在的黑块被填");
        checkRgb(layerPixel(doc, 0, 22, 7), kBlack, "隔着白底的另一个黑块不受影响");

        delete doc;
    }
    {
        Document *doc = makeDoc(kWhite);
        paintBlock(doc, left, kBlack);
        paintBlock(doc, right, kBlack);

        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        auto *opts = prepareBucket(&tool, QColor(255, 0, 0));
        opts->setContiguous(false);

        check(tool.fillAt(QPoint(7, 7)), "非连续模式：填充成功");
        checkRgb(layerPixel(doc, 0, 7, 7),  kRed, "起点所在的黑块被填");
        checkRgb(layerPixel(doc, 0, 22, 7), kRed,
                 "另一个黑块也被填（不连续模式就是要填遍全图的同色像素）");

        delete doc;
    }
}

// ===========================================================================
//  [7] 选区限制
// ===========================================================================
static void testSelectionClip()
{
    beginGroup("油漆桶：选区把填充裁住");

    Document *doc = makeDoc(kWhite);
    doc->selection().prepareCanvas(doc->canvasSize());
    doc->selection().selectRect(QRect(0, 0, kW / 2, kH));

    CommandStack stack(doc);
    BucketTool tool(doc, &stack);
    prepareBucket(&tool, QColor(255, 0, 0));

    check(tool.fillAt(QPoint(5, 5)), "带选区时填充成功");

    checkRgb(layerPixel(doc, 0, 5, 5),        kRed,   "选区内的像素被填");
    checkRgb(layerPixel(doc, 0, kW / 2 - 1, 5), kRed, "选区最右一列被填");
    // ★ 差一格的边界 ★
    checkRgb(layerPixel(doc, 0, kW / 2, 5),    kWhite, "选区外的第一列没被填");
    checkRgb(layerPixel(doc, 0, kW - 1, kH - 1), kWhite, "画布右下角没被填");

    delete doc;
}

// ===========================================================================
//  [8] 填充不透明度
// ===========================================================================
static void testOpacity()
{
    beginGroup("油漆桶：不透明度乘进 alpha");

    Document *doc = makeDoc(kClear);      // 全透明底，一眼看出 alpha
    CommandStack stack(doc);
    BucketTool tool(doc, &stack);
    auto *opts = prepareBucket(&tool, QColor(255, 0, 0));   // 不透明红
    opts->setOpacity(50);

    check(tool.fillAt(QPoint(10, 10)), "不透明度 50% 填充成功");

    // 255 * 50 / 100 = 127（整数除法）。预乘空间里纯红 + alpha 127
    // 就是 (127, 0, 0, 127)。
    // ★ 这一条同时钉住 QColor(qRgba(...)) 丢 alpha 那个陷阱 ★
    //   用了它的话 alpha 会变成 255，量到的就是 (255,0,0,255)。
    checkRgb(layerPixel(doc, 0, 10, 10), qRgba(127, 0, 0, 127),
             "50% 的红写进去是预乘 (127,0,0,127)");

    delete doc;

    // 不透明度 0% = 填了等于没填：不该留下任何像素改动，也不该压命令
    Document *doc2 = makeDoc(kWhite);
    CommandStack stack2(doc2);
    BucketTool tool2(doc2, &stack2);
    auto *opts2 = prepareBucket(&tool2, QColor(255, 0, 0));
    opts2->setOpacity(0);

    check(tool2.fillAt(QPoint(10, 10)) == false, "不透明度 0% 时拒绝（返回 false）");
    checkRgb(layerPixel(doc2, 0, 10, 10), kWhite, "像素一个都没动");
    checkEqInt(stack2.undoStack()->count(), 0, "没有压入任何撤销记录");

    delete doc2;
}

// ===========================================================================
//  [9] 撤销粒度 ★ 最容易静默失效的一条
// ===========================================================================
static void testUndoGranularity()
{
    beginGroup("油漆桶：两次点击 = 两条撤销记录");

    const QRect left(5, 5, 5, 5);
    const QRect right(20, 5, 5, 5);

    Document *doc = makeDoc(kWhite);
    paintBlock(doc, left, kBlack);
    paintBlock(doc, right, kBlack);

    CommandStack stack(doc);
    BucketTool tool(doc, &stack);
    prepareBucket(&tool, QColor(255, 0, 0));

    check(tool.fillAt(QPoint(7, 7)),   "第一次点击（左边方块）成功");
    check(tool.fillAt(QPoint(22, 7)),  "第二次点击（右边方块）成功");

    // ★ 这一组断言盯的是 DrawCommand 的描边编号 ★
    //   若相邻两条 DrawCommand 的 id() 相同，QUndoStack 会把第二条
    //   合并进第一条 —— 于是"点两下"变成"一步撤销"，用户按 Ctrl+Z
    //   一次丢掉两处改动。编译期、运行期都不会有任何提示。
    checkEqInt(stack.undoStack()->count(), 2,
               "撤销栈里有两条独立记录（没有被悄悄合并）");

    stack.undo();
    checkRgb(layerPixel(doc, 0, 22, 7), kBlack, "撤销一次：右边方块还原");
    checkRgb(layerPixel(doc, 0, 7, 7),  kRed,   "撤销一次：左边方块仍在（粒度正确）");

    stack.undo();
    checkRgb(layerPixel(doc, 0, 7, 7), kBlack, "再撤销一次：左边方块也还原");
    check(!stack.canUndo(), "撤销栈已空");

    // 重做要能原样回来（两条一起）
    stack.redo();
    checkRgb(layerPixel(doc, 0, 7, 7), kRed, "重做一次：左边方块回来");

    delete doc;
}

// ===========================================================================
//  [10] 拒绝路径
// ===========================================================================
static void testRefusals()
{
    beginGroup("油漆桶：该拒绝的时候拒绝");

    // ── 图层锁定 ──
    {
        Document *doc = makeDoc(kWhite);
        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        prepareBucket(&tool, QColor(255, 0, 0));
        doc->activeLayer()->setLocked(true);

        check(tool.fillAt(QPoint(10, 10)) == false, "锁定图层上拒绝填充");
        checkRgb(layerPixel(doc, 0, 10, 10), kWhite, "锁定图层的像素没被改");
        checkEqInt(stack.undoStack()->count(), 0, "没有压入撤销记录");

        delete doc;
    }
    // ── 图层隐藏 ──
    {
        Document *doc = makeDoc(kWhite);
        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        prepareBucket(&tool, QColor(255, 0, 0));
        doc->activeLayer()->setVisible(false);

        check(tool.fillAt(QPoint(10, 10)) == false, "隐藏图层上拒绝填充");
        checkRgb(layerPixel(doc, 0, 10, 10), kWhite, "隐藏图层的像素没被改");

        delete doc;
    }
    // ── 画布外 ──
    {
        Document *doc = makeDoc(kWhite);
        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        prepareBucket(&tool, QColor(255, 0, 0));

        check(tool.fillAt(QPoint(-1, 5)) == false, "画布左侧外拒绝");
        check(tool.fillAt(QPoint(5, -1)) == false, "画布上方外拒绝");
        check(tool.fillAt(QPoint(kW, 5)) == false, "画布右侧外拒绝");
        check(tool.fillAt(QPoint(5, kH)) == false, "画布下方外拒绝");
        checkEqInt(stack.undoStack()->count(), 0, "越界点击没有留下任何记录");
        checkRgb(layerPixel(doc, 0, 0, 0), kWhite, "画布内容完全没变");

        delete doc;
    }
    // ── 鼠标右键 / 非按下事件 ──
    {
        Document *doc = makeDoc(kWhite);
        CommandStack stack(doc);
        BucketTool tool(doc, &stack);
        prepareBucket(&tool, QColor(255, 0, 0));

        ToolEvent rightClick = makeEvent(ToolEventType::Press, 10, 10);
        rightClick.button  = Qt::RightButton;
        rightClick.buttons = Qt::RightButton;
        const ToolResult r = tool.mousePress(rightClick);
        check(!r.handled, "右键不被油漆桶处理");
        checkEqInt(stack.undoStack()->count(), 0, "右键没有产生填充");

        const ToolResult done = tool.mousePress(makeEvent(ToolEventType::Press, 10, 10));
        check(done.handled, "左键按下被处理");
        checkEqInt(stack.undoStack()->count(), 1, "左键按下产生了一条撤销记录");

        delete doc;
    }
}

// ===========================================================================
//  [11] 吸管：取的是合成图
// ===========================================================================
static void testPickerUsesComposite()
{
    beginGroup("吸管：取合成图（所见即所得）");

    // 下层纯红、上层全透明。活动图层是上层。
    Document *doc = makeDoc(kRed);
    const int top = doc->addLayer(QStringLiteral("空层"));
    layerFillRect(doc, top, QRect(0, 0, kW, kH), kClear);
    doc->markDirty();
    doc->setActiveLayerIndex(top);

    CommandStack stack(doc);
    ColorPickerTool picker(doc, &stack);

    const QColor c = picker.pickAt(QPoint(10, 10));
    check(c.isValid(), "在画布内取到了颜色");
    checkRgb(c.rgba(), kRed,
             "取到的是下层那一眼看到的红，而不是活动图层的透明");

    delete doc;
}

// ===========================================================================
//  [12] 吸管：取样范围与 alpha
// ===========================================================================
static void testPickerSampling()
{
    beginGroup("吸管：取样范围与 alpha");

    // ── 全透明区域：必须得到 alpha 0，而不是纯白 ──
    {
        Document *doc = makeDoc(kClear);
        CommandStack stack(doc);
        ColorPickerTool picker(doc, &stack);

        const QColor c = picker.pickAt(QPoint(10, 10));
        check(c.isValid(), "透明区域取色返回有效色（不是无效色）");
        checkEqInt(c.alpha(), 0, "透明区域 alpha 是 0");
        // ★ 这一条钉住"预乘空间除以 0 → 纯白"那个陷阱 ★
        //   硬除会得到巨大值被钳到 255，于是"取一块空白"得到纯白。
        checkEqInt(c.red(), 0, "透明区域没有变成纯白（red 是 0）");
        checkEqInt(c.green(), 0, "透明区域没有变成纯白（green 是 0）");
        checkEqInt(c.blue(), 0, "透明区域没有变成纯白（blue 是 0）");

        delete doc;
    }
    // ── 半透明像素：alpha 必须原样传出来 ──
    {
        // 预乘 (128,0,0,128) ⇔ 反预乘 (255,0,0) + alpha 128
        Document *doc = makeDoc(qRgba(128, 0, 0, 128));
        CommandStack stack(doc);
        ColorPickerTool picker(doc, &stack);

        const QColor c = picker.pickAt(QPoint(10, 10));
        checkEqInt(c.alpha(), 128, "半透明像素的 alpha 是 128（没被抹成 255）");
        checkEqInt(c.red(), 255, "预乘分量被正确还原成 255");
        checkEqInt(c.green(), 0, "green 仍是 0");
        checkEqInt(c.blue(), 0, "blue 仍是 0");

        delete doc;
    }
    // ── 点取样 vs 3×3 平均 ──
    {
        // 3×3 邻域里放 4 个白角（上下左右中心都是黑），
        //   点取样 → (0,0,0)
        //   3×3   → 4/9 白 = 113.33 → 113
        // 期望值是手算出来的，不依赖实现。
        Document *doc = makeDoc(kBlack);
        for (int dy = -1; dy <= 1; dy += 2)
            for (int dx = -1; dx <= 1; dx += 2)
                layerSetPixel(doc, 0, 10 + dx, 10 + dy, kWhite);
        doc->markDirty();

        CommandStack stack(doc);
        ColorPickerTool picker(doc, &stack);
        auto *opts = qobject_cast<PickerOptions *>(picker.options());
        check(opts != nullptr, "吸管的参数对象是 PickerOptions");

        opts->setRadius(PickRadius::Point);
        const QColor one = picker.pickAt(QPoint(10, 10));
        checkEqInt(one.red(), 0, "点取样取到中心那个黑像素");

        opts->setRadius(PickRadius::Average3x3);
        const QColor avg = picker.pickAt(QPoint(10, 10));
        checkEqInt(avg.red(), 113, "3×3 平均 = 4/9 × 255 = 113");
        checkEqInt(avg.alpha(), 255, "3×3 平均后仍是完全不透明");

        delete doc;
    }
    // ── 画布外 ──
    {
        Document *doc = makeDoc(kRed);
        CommandStack stack(doc);
        ColorPickerTool picker(doc, &stack);

        check(!picker.pickAt(QPoint(-1, 10)).isValid(), "画布外取色返回无效色");
        check(!picker.pickAt(QPoint(kW, 10)).isValid(), "画布右外取色返回无效色");

        delete doc;
    }
}

// ===========================================================================
//  [13] 吸管的事件路径
// ===========================================================================
static void testPickerEvents()
{
    beginGroup("吸管：事件路径与信号");

    Document *doc = makeDoc(kRed);
    const int top = doc->addLayer(QStringLiteral("蓝层"));
    layerFillRect(doc, top, QRect(0, 0, kW / 2, kH), kBlue);   // 左半蓝
    layerFillRect(doc, top, QRect(kW / 2, 0, kW / 2, kH), kClear); // 右半透明
    doc->markDirty();
    doc->setActiveLayerIndex(top);

    CommandStack stack(doc);
    ColorPickerTool picker(doc, &stack);

    QColor fg, bg;
    int fgCount = 0, bgCount = 0;
    // ★ 用 QObject::connect 全名 ★
    //   这里不是 QObject 的成员函数（测试是自由函数），
    //   非限定形式的 connect 找不到 —— 它是个静态成员，不是全局函数。
    QObject::connect(&picker, &AbstractTool::requestForegroundColor,
                     &picker, [&](const QColor &c) { fg = c; ++fgCount; });
    QObject::connect(&picker, &AbstractTool::requestBackgroundColor,
                     &picker, [&](const QColor &c) { bg = c; ++bgCount; });

    // ── 左键点一下：取前景色 ──
    picker.mousePress(makeEvent(ToolEventType::Press, 5, 5));
    checkEqInt(fgCount, 1, "按下发出了一次前景色请求");
    checkRgb(fg.rgba(), kBlue, "取到的前景色是合成图上的蓝");

    // ── 拖动中持续取色 ──
    const int before = picker.pickCount();
    ToolEvent move = makeEvent(ToolEventType::Move, 6, 5);
    move.buttons = Qt::LeftButton;
    picker.mouseMove(move);
    picker.mouseMove(makeEvent(ToolEventType::Move, 7, 5));
    check(picker.pickCount() > before, "按住拖动时会连续取色");

    picker.mouseRelease(makeEvent(ToolEventType::Release, 7, 5));

    // 松手之后再移动不该继续取色（否则鼠标划过画布就会乱改前景色）
    const int afterRelease = picker.pickCount();
    picker.mouseMove(makeEvent(ToolEventType::Move, 30, 5));
    checkEqInt(picker.pickCount(), afterRelease, "松开之后再移动不再取色");

    // ── Alt + 左键：取背景色 ──
    ToolEvent altPress = makeEvent(ToolEventType::Press, 5, 5);
    altPress.modifiers.alt = true;
    picker.mousePress(altPress);
    checkEqInt(bgCount, 1, "Alt + 左键发出了一次背景色请求");
    checkRgb(bg.rgba(), kBlue, "取到的背景色是合成图上的蓝");
    picker.mouseRelease(makeEvent(ToolEventType::Release, 5, 5));

    // ── 画布外：静默忽略，不改颜色 ──
    const int fgBefore = fgCount;
    picker.mousePress(makeEvent(ToolEventType::Press, kW + 10, 5));
    checkEqInt(fgCount, fgBefore, "画布外按下不发前景色请求");
    checkRgb(picker.lastPickedColor().rgba(), kBlue, "最近取到的颜色没有被越界点击污染");
    picker.mouseRelease(makeEvent(ToolEventType::Release, kW + 10, 5));

    // ── 观测口本身 ──
    checkEqInt(picker.lastPickPos().x(), 5, "记录的最近取色位置是最后一次有效取色");
    check(picker.pickCount() > 0, "取色计数在累加");

    delete doc;
}

// ===========================================================================
//  [14] 合成管线：预乘量只乘一次 alpha
// ===========================================================================
//  ★ 为什么把这条不变量放在"工具测试"里 ★
//
//    它其实属于 engine/Blend —— 但本轮之所以能发现它，正是因为吸管
//    要读合成图（[12] 组那条"半透明像素的预乘分量被正确还原"）。
//    与其再造一个 blend_test，不如在这里**直接**把它钉一遍：
//    不带任何工具、只调 Document::composite()，这样将来 Blend 改了，
//    无论吸管在不在，这条断言都会红。
//
//  错的写法是写回时 `mixR * finalA`（二次预乘）。它的症状：
//  只要结果 alpha 不是 255，颜色就被压暗到 outA 倍。
//  目标不透明时 outA == 255，那一步退化成乘 1 —— 所以**第 2 条断言
//  （叠在白底上）在 bug 态下也是绿的**，它同时是一份"为什么当初没发现"的记录。
static void testCompositePremultiply()
{
    beginGroup("合成管线：半透明内容不会被二次预乘");

    // ── 单层半透明图在透明底上：合成结果必须逐位等于它自己 ──
    //    raw (128,0,0,128) 语义 = 反预乘后的纯红、alpha 128
    {
        Document *doc = makeDoc(qRgba(128, 0, 0, 128));
        const QImage comp = doc->composite();
        check(!comp.isNull(), "合成结果不是空图");
        check(comp.format() == QImage::Format_ARGB32_Premultiplied,
              "合成结果的格式是 ARGB32_Premultiplied（吸管依赖这一点）");
        checkRgb(rawPixel(comp, 10, 10), qRgba(128, 0, 0, 128),
                 "半透明的纯红合成后仍是 (128,0,0,128)，没有变暗");
        delete doc;
    }

    // ── 四分之一透明：同样不能被压暗 ──
    //    raw (64,128,192,64) → 反预乘 (255, 128*255/64=510→钳 255, ...)
    //    这里只关心**写回是不是原样**，所以直接比原始字节。
    {
        Document *doc = makeDoc(qRgba(64, 32, 16, 64));
        const QImage comp = doc->composite();
        checkRgb(rawPixel(comp, 5, 5), qRgba(64, 32, 16, 64),
                 "alpha 64 的像素合成后仍是 (64,32,16,64)");
        delete doc;
    }

    // ── 两层：手算一遍 source-over 的期望值 ──
    //   下层 (100,100,100,200)：da = 200/255, dr = 100/200 = 0.5
    //   上层 (128,  0,  0,128)：ea = 128/255, sr = 128/128 = 1.0（纯红）
    //   outA = da + ea(1-da)            = 0.89265 → 228
    //   mixR = sr·ea + dr·da·(1-ea)     = 0.69728 → 178
    //   mixG = 0   ·ea + 0.5·da·(1-ea)  = 0.19532 →  50
    //   注：mixR/G/B 已经是预乘量，直接 ×255。容差 1 是给浮点留的。
    {
        Document *doc = makeDoc(qRgba(100, 100, 100, 200));
        const int top = doc->addLayer(QStringLiteral("半透明红"));
        layerFillRect(doc, top, QRect(0, 0, kW, kH), qRgba(128, 0, 0, 128));
        doc->markDirty();

        const QImage comp = doc->composite();
        checkRgbNear(rawPixel(comp, 20, 20), qRgba(178, 50, 50, 228), 1,
                     "两层 source-over 的结果是 (178,50,50,228)（手算值）");
        delete doc;
    }

    // ── 反面对照：目标不透明时颜色**不该**变 ──
    //    这一条在"二次预乘"那个 bug 下依然是绿的（outA == 255 时退化），
    //    留着它是为了说明"为什么白底上一直没看出来"。
    {
        Document *doc = makeDoc(kWhite);
        paintBlock(doc, QRect(0, 0, kW, kH), kWhite);
        const int top = doc->addLayer(QStringLiteral("半透明红"));
        layerFillRect(doc, top, QRect(0, 0, kW, kH), qRgba(128, 0, 0, 128));
        doc->markDirty();

        const QImage comp = doc->composite();
        const QRgb px = rawPixel(comp, 20, 20);
        checkEqInt(qAlpha(px), 255, "白底上的合成结果是完全不透明");
        checkEqInt(qRed(px), 255, "红色通道：纯红叠白底 = 255（与 alpha 无关）");
        // G: 0·ea + 255·1·(1-ea) → 0.49804*255 = 127
        checkEqInt(qGreen(px), 127, "绿色通道：50% 红叠白 = 127（手算值）");
        delete doc;
    }
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 吸管 / 油漆桶 离线自检\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    testIdentity();
    testRegionShape();
    testOnlyActiveLayer();
    testCriterionSource();
    testTolerance();
    testContiguous();
    testSelectionClip();
    testOpacity();
    testUndoGranularity();
    testRefusals();
    testPickerUsesComposite();
    testPickerSampling();
    testPickerEvents();
    testCompositePremultiply();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
