// ============================================================================
//  view_test.cpp —— 画布视图的尺寸与缩放（离屏）
// ----------------------------------------------------------------------------
//  为什么需要它：
//    打开照片后"显示的尺寸不对""滚轮缩放异常"这类问题的根源通常在
//    zoom / offset / canvasSize 三者的换算关系上，而这些换算**只靠读代码
//    看不出来** —— 矩阵的后乘顺序、锚点缩放的反解、缩放上限的钳制，
//    任何一个环节错了都表现为"看起来只是有点怪"。
//
//    本测试用真实的 QImage（可以从命令行指定文件）构造 Document 与
//    CanvasView，然后在 offscreen 平台下真的发滚轮事件，逐条断言：
//
//      ① 文档尺寸 = 图像尺寸（不是别的什么）
//      ② zoomToFit 之后，整幅图确实完整落在视口内且居中
//      ③ 滚轮一格 = 缩放 ×1.25
//      ④ ★ 以光标为锚点缩放：光标下的图像点在缩放前后必须不变 ★
//      ⑤ 缩放上下限被正确钳制
//      ⑥ imageToWidget / widgetToImage 互为逆映射
//
//  用法：
//      view_test [图像文件]
//    不给文件时用程序生成的合成图，这样在 CI 里不依赖外部资源。
// ============================================================================

#include "ui/CanvasView.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "engine/ImageMeta.h"

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QWheelEvent>
#include <QtGlobal>
#include <QtMath>

#include <cmath>
#include <cstdio>

using namespace zl;

static int g_failed = 0;
static int g_passed = 0;

static void check(bool ok, const char *what, const char *detail = "")
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s  %s\n", what, detail);
    }
}

static void checkNear(qreal actual, qreal expected, qreal tol,
                      const char *what)
{
    if (std::fabs(actual - expected) <= tol) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("  [失败] %s  期望 %.4f ± %.4f，实际 %.4f\n",
                what, expected, tol, actual);
}

static void checkEqInt(int actual, int expected, const char *what)
{
    if (actual == expected) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
}

// 造一张合成图（不依赖外部文件），模拟"宽扁的 16:9 照片"
static QImage makeSynthetic()
{
    QImage img(3840, 2160, QImage::Format_ARGB32_Premultiplied);
    img.fill(qRgba(40, 80, 160, 255));
    return img;
}

// ---------------------------------------------------------------------------
//  1. 文档尺寸必须等于图像尺寸
// ---------------------------------------------------------------------------
static void testDocumentSize(const QImage &photo)
{
    std::printf("\n[1] 文档尺寸\n");

    Document doc(photo.size(), Qt::transparent);
    Layer *bg = doc.layers().layer(0);
    if (bg) {
        bg->setImage(photo);
        doc.markDirty();
    }

    const QSize cs = doc.canvasSize();
    std::printf("     图像 %d×%d   →   文档画布 %d×%d\n",
                photo.width(), photo.height(), cs.width(), cs.height());

    check(cs == photo.size(), "画布尺寸 == 图像像素尺寸");

    // 合成结果的尺寸也必须一致 —— 若不一致，画布上画的东西就不是原图
    const QImage comp = doc.composite();
    std::printf("     合成结果 %d×%d\n", comp.width(), comp.height());
    check(comp.size() == photo.size(), "合成结果尺寸 == 图像尺寸");
}

// ---------------------------------------------------------------------------
//  2. zoomToFit：整幅图必须在视口内且居中
// ---------------------------------------------------------------------------
static void testZoomToFit(CanvasView &cv, Document &doc, const QSize &viewport)
{
    std::printf("\n[2] 适合窗口\n");

    cv.resize(viewport);
    std::printf("     控件尺寸 %d×%d  （resize 之后）\n", cv.width(), cv.height());

    cv.setDocument(&doc);          // setDocument 内部会 zoomToFit
    std::printf("     setDocument 之后：zoom=%.6f  offset=(%.3f, %.3f)\n",
                cv.zoom(), cv.offset().x(), cv.offset().y());

    cv.zoomToFit();
    std::printf("     zoomToFit  之后：zoom=%.6f  offset=(%.3f, %.3f)\n",
                cv.zoom(), cv.offset().x(), cv.offset().y());

    const qreal z = cv.zoom();
    const QPointF tl = cv.imageToWidget(QPointF(0, 0));
    const QPointF br = cv.imageToWidget(QPointF(doc.canvasSize().width(),
                                                doc.canvasSize().height()));

    std::printf("     视口 %d×%d   画布 %d×%d\n",
                viewport.width(), viewport.height(),
                doc.canvasSize().width(), doc.canvasSize().height());
    std::printf("     缩放 %.4f (%.1f%%)   图像左上角在控件 (%6.1f, %6.1f)"
                "   右下角 (%6.1f, %6.1f)\n",
                z, z * 100.0, tl.x(), tl.y(), br.x(), br.y());

    // 期望的缩放：min((W-20)/w, (H-20)/h)
    const qreal expectZ = qMin(qreal(viewport.width() - 20) / doc.canvasSize().width(),
                               qreal(viewport.height() - 20) / doc.canvasSize().height());
    checkNear(z, expectZ, 0.001, "适合窗口的缩放比 = min(可用宽/图宽, 可用高/图高)");

    // 期望的偏移：图像中心对齐控件中心 → offset = widgetCenter - canvasCenter*zoom
    const qreal expectOffX = viewport.width()  / 2.0 - doc.canvasSize().width()  / 2.0 * z;
    const qreal expectOffY = viewport.height() / 2.0 - doc.canvasSize().height() / 2.0 * z;
    std::printf("     期望 offset=(%.3f, %.3f)\n", expectOffX, expectOffY);
    checkNear(cv.offset().x(), expectOffX, 1.0, "适合窗口后的水平偏移正确");
    checkNear(cv.offset().y(), expectOffY, 1.0, "适合窗口后的垂直偏移正确");

    // 整幅图必须落在视口内
    check(tl.x() >= -0.5 && tl.y() >= -0.5,
          "适合窗口后图像左上角不在视口外（左边/上边）",
          tl.x() < -1.0 || tl.y() < -1.0 ? "← 图像被裁掉了" : "");
    check(br.x() <= viewport.width() + 0.5 && br.y() <= viewport.height() + 0.5,
          "适合窗口后图像右下角不在视口外（右边/下边）",
          br.x() > viewport.width() + 1.0 || br.y() > viewport.height() + 1.0
              ? "← 图像超出窗口" : "");

    // 居中
    checkNear((tl.x() + br.x()) / 2.0, viewport.width() / 2.0, 1.0,
              "适合窗口后水平居中");
    checkNear((tl.y() + br.y()) / 2.0, viewport.height() / 2.0, 1.0,
              "适合窗口后垂直居中");
}

// ---------------------------------------------------------------------------
//  3. 滚轮缩放：倍数正确 + 锚点不动
// ---------------------------------------------------------------------------
static void sendWheel(CanvasView &cv, const QPointF &pos, int angleY)
{
    // ★ Qt5 的 QWheelEvent 构造签名（8 个参数）★
    //   (pos, globalPos, pixelDelta, angleDelta, buttons, modifiers, phase, inverted)
    //   Qt6 里 pos/globalPos 合并成了单个 position，且 phase/inverted 位置不同，
    //   所以真实代码里从不手工构造滚轮事件 —— 这段测试代码是 Qt5 专用的，
    //   与 zl::wheelEventPos() 的兼容处理无关。
    const QPointF globalPos(cv.mapToGlobal(pos.toPoint()));
    QWheelEvent ev(pos,                              // pos (local)
                   globalPos,                        // globalPos
                   QPoint(0, 0),                     // pixelDelta
                   QPoint(0, angleY),                // angleDelta
                   Qt::NoButton,                     // buttons
                   Qt::NoModifier,                   // modifiers
                   Qt::NoScrollPhase,                // phase
                   false);                           // inverted
    QApplication::sendEvent(&cv, &ev);
}

static void testWheelZoom(CanvasView &cv, Document &doc)
{
    std::printf("\n[3] 滚轮缩放\n");

    cv.zoomToFit();

    const QPointF anchor(300.0, 200.0);          // 视口内的一个点

    // ── 3.1 一格滚轮 = ×1.25 ──
    const qreal before = cv.zoom();
    const QPointF imgUnderAnchorBefore = cv.widgetToImage(anchor);

    sendWheel(cv, anchor, 120);

    const qreal after = cv.zoom();
    std::printf("     滚轮一格：%.4f → %.4f  (比值 %.4f)\n",
                before, after, after / before);
    checkNear(after / before, 1.25, 0.01, "向外滚一格 = 放大 1.25 倍");

    // ── 3.2 ★ 锚点不动 ★ ──
    const QPointF imgUnderAnchorAfter = cv.widgetToImage(anchor);
    std::printf("     锚点(%.0f,%.0f) 下的图像点：缩放前 (%.2f, %.2f) → 缩放后 (%.2f, %.2f)\n",
                anchor.x(), anchor.y(),
                imgUnderAnchorBefore.x(), imgUnderAnchorBefore.y(),
                imgUnderAnchorAfter.x(), imgUnderAnchorAfter.y());
    checkNear(imgUnderAnchorAfter.x(), imgUnderAnchorBefore.x(), 1.0,
              "以光标为锚点缩放：锚点下的图像 X 不变");
    checkNear(imgUnderAnchorAfter.y(), imgUnderAnchorBefore.y(), 1.0,
              "以光标为锚点缩放：锚点下的图像 Y 不变");

    // ── 3.3 反向滚一格必须回到原值 ──
    sendWheel(cv, anchor, -120);
    checkNear(cv.zoom(), before, 0.001, "向外再向内各一格，缩放回到原值");

    // ── 3.4 上限钳制 ──
    for (int i = 0; i < 60; ++i)
        sendWheel(cv, anchor, 120);
    std::printf("     连续放大 60 格后：%.1f%%\n", cv.zoom() * 100.0);
    check(cv.zoom() <= 64.0 + 1e-6, "放大有上限（不超过 6400%）");

    // ── 3.5 下限钳制 ──
    for (int i = 0; i < 120; ++i)
        sendWheel(cv, anchor, -120);
    std::printf("     连续缩小 120 格后：%.4f%%\n", cv.zoom() * 100.0);
    check(cv.zoom() >= 0.01 - 1e-9, "缩小有下限（不低于 1%）");
}

// ---------------------------------------------------------------------------
//  4. 坐标换算互为逆映射
// ---------------------------------------------------------------------------
static void testCoordinateRoundTrip(CanvasView &cv)
{
    std::printf("\n[4] 坐标换算\n");

    cv.zoomToFit();

    const QPointF samples[] = { QPointF(0, 0), QPointF(123.5, 77.25),
                                QPointF(1000, 700), QPointF(3839, 2159) };

    bool allOk = true;
    for (const QPointF &p : samples) {
        const QPointF back = cv.widgetToImage(cv.imageToWidget(p));
        if (std::fabs(back.x() - p.x()) > 0.01 || std::fabs(back.y() - p.y()) > 0.01) {
            allOk = false;
            std::printf("     往返失败：(%.2f,%.2f) → (%.2f,%.2f)\n",
                        p.x(), p.y(), back.x(), back.y());
        }
    }
    check(allOk, "imageToWidget / widgetToImage 互为逆映射");
}

// ---------------------------------------------------------------------------
//  5. 平移：必须 1:1 跟随光标
// ---------------------------------------------------------------------------
//  ★ 这条能抓住"offset 被缩放"这类 bug ★
//    m_offset 的语义是"图像左上角在控件坐标里的位置"。所以拖动鼠标
//    Δ 个控件像素，图像也必须正好移动 Δ 个控件像素 —— 与当前缩放比无关。
//    若 rebuildViewTransform 把平移放进了被缩放的层里，平移量会变成
//    Δ × zoom，在 33% 缩放下"拖了 300 像素，画面只挪了 100 像素"，
//    手感像拖着一根橡皮筋。
static void testPanFollowsCursor(CanvasView &cv, Document &doc)
{
    std::printf("\n[5] 平移\n");

    // 先回到 100%，排除缩放干扰，单独检查"位移是否被缩放"
    cv.setZoom(1.0, QPointF(cv.width() / 2.0, cv.height() / 2.0));
    checkNear(cv.zoom(), 1.0, 1e-6, "setZoom(1.0) 后缩放为 100%");

    const QPointF before = cv.imageToWidget(QPointF(0, 0));
    cv.panBy(QPointF(100.0, -40.0));
    const QPointF after = cv.imageToWidget(QPointF(0, 0));

    std::printf("     100%% 时平移 (100,-40)：图像原点 (%6.1f,%6.1f) → (%6.1f,%6.1f)\n",
                before.x(), before.y(), after.x(), after.y());
    checkNear(after.x() - before.x(), 100.0, 0.01, "100% 时平移 1:1 跟随光标（X）");
    checkNear(after.y() - before.y(), -40.0, 0.01, "100% 时平移 1:1 跟随光标（Y）");

    // 缩小到 33% 再平移，位移仍必须是控件像素（不随缩放变化）
    cv.setZoom(0.33, QPointF(cv.width() / 2.0, cv.height() / 2.0));
    const QPointF b2 = cv.imageToWidget(QPointF(0, 0));
    cv.panBy(QPointF(100.0, 0));
    const QPointF a2 = cv.imageToWidget(QPointF(0, 0));
    std::printf("     33%% 时平移 (100,0)：位移 %.3f\n", a2.x() - b2.x());
    checkNear(a2.x() - b2.x(), 100.0, 0.01,
              "缩小后平移仍是 1:1（位移不被 zoom 缩放）");
}

// ---------------------------------------------------------------------------
//  6. 首次显示前就 setDocument（启动路径）
// ---------------------------------------------------------------------------
//  主窗口的构造顺序是"先建文档 → 再 show()"。也就是说 setDocument() 与
//  它内部的 zoomToFit() 可能在画布控件还没拿到最终尺寸时就被调用了。
//  这里复现这个顺序，看看初始缩放是否合理。
static void testStartupSequence(const QImage &photo, const QSize &viewport)
{
    std::printf("\n[6] 启动路径（先 setDocument 再 resize）\n");

    Document doc(photo.size(), Qt::transparent);
    if (Layer *bg = doc.layers().layer(0)) {
        bg->setImage(photo);
        doc.markDirty();
    }

    CanvasView cv;                       // 没有 resize，也没有 show
    std::printf("     setDocument 之前控件默认尺寸 %d×%d  isVisible=%d\n",
                cv.width(), cv.height(), int(cv.isVisible()));
    cv.setDocument(&doc);
    std::printf("     setDocument 之后：zoom=%.4f (%.1f%%)  offset=(%.1f, %.1f)\n",
                cv.zoom(), cv.zoom() * 100.0, cv.offset().x(), cv.offset().y());

    // 模拟主窗口：先 show()，再由布局把尺寸分配下来。
    // ★ 必须调 show() 并且跑一遍事件循环 ★
    //   对未显示的控件调 resize()，Qt 不会派发 resizeEvent（实测：
    //   offset 完全不变），那样就测不出"补做适合窗口"这条逻辑。
    cv.show();
    cv.resize(viewport);
    QApplication::processEvents();

    const qreal zAfter = cv.zoom();
    std::printf("     show + resize 到 %d×%d 之后：zoom=%.4f (%.1f%%)  offset=(%.1f, %.1f)\n",
                viewport.width(), viewport.height(),
                zAfter, zAfter * 100.0, cv.offset().x(), cv.offset().y());

    // 控件拿到真实尺寸后，缩放应该等于按最终尺寸算出的 fitZoom
    const qreal idealZ = qMin(qreal(viewport.width() - 20) / photo.width(),
                              qreal(viewport.height() - 20) / photo.height());
    std::printf("     按最终尺寸应有的 zoom = %.4f (%.1f%%)\n",
                idealZ, idealZ * 100.0);

    checkNear(zAfter, idealZ, 0.001,
              "控件拿到真实尺寸后会补做一次「适合窗口」");
    check(zAfter >= 0.01 - 1e-9 && zAfter <= 64.0 + 1e-6,
          "启动路径的缩放落在合法范围内（没有被钳制到极端值）");

    // 无论缩放到多少，图像都必须完整落在视口内且居中
    const QPointF tl = cv.imageToWidget(QPointF(0, 0));
    const QPointF br = cv.imageToWidget(QPointF(photo.width(), photo.height()));
    check(tl.x() >= -1.0 && tl.y() >= -1.0
              && br.x() <= viewport.width() + 1.0
              && br.y() <= viewport.height() + 1.0,
          "启动路径下图像完整落在视口内",
          "← 说明 offset 计算在尺寸变化后失效了");
    checkNear((tl.x() + br.x()) / 2.0, viewport.width() / 2.0, 1.5,
              "启动路径下图像水平居中");
    checkNear((tl.y() + br.y()) / 2.0, viewport.height() / 2.0, 1.5,
              "启动路径下图像垂直居中");
}

// ---------------------------------------------------------------------------
//  7. 渲染出来的实际占位（走完整 paintEvent 管线）
// ---------------------------------------------------------------------------
//  ★ 为什么数学对了还要再测一遍 ★
//
//   前面几组断言算的都是"矩阵把图像映射到了哪"——它们是**关于数学**的断言。
//   但用户看到的是**像素**。中间还隔着 paintEvent 的十步管线：
//   棋盘格、合成、边框、裁剪……任何一步用错了变换或算错了源矩形，
//   数学断言全部通过而屏幕上的图像依然是错的。
//
//   这条测试的做法：把背景层填成纯红（一个极易识别的标记色），
//   真的调 grab() 让 Qt 渲染一遍 paintEvent，再从渲染结果里
//   量出"红色区域占多大、在哪"。这个包围盒就是**用户实际看到的图像尺寸**。
//
//   ★ 为什么用纯色标记而不是真实照片 ★
//     真实照片的边缘像素颜色不可预知（可能恰好等于背景灰）。
//     纯红与背景灰 (80,80,80) 在三个通道上都拉开很大距离，
//     阈值判定不会误伤，测得的边界也就精确。
// ---------------------------------------------------------------------------

static QImage makeMarkerRed()
{
    QImage img(3840, 2160, QImage::Format_ARGB32_Premultiplied);
    img.fill(qRgba(255, 0, 0, 255));   // 不透明纯红
    return img;
}

// 在渲染结果里找出"红色区域"的包围盒
static QRect redBoundingBox(const QImage &shot)
{
    int minX = shot.width(), minY = shot.height(), maxX = -1, maxY = -1;

    for (int y = 0; y < shot.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(shot.constScanLine(y));
        for (int x = 0; x < shot.width(); ++x) {
            const QRgb c = line[x];
            // 明显的红：红通道高、绿蓝通道低。
            // 缩放插值只会让红略微变暗（红与红之间插值仍是红），
            // 而边框那条深色描边会把最外圈压暗 —— 后面的容差已经算上。
            if (qRed(c) > 200 && qGreen(c) < 60 && qBlue(c) < 60) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    if (maxX < 0)
        return QRect();   // 一个红像素都没有
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

static void testRenderedGeometry(const QSize &viewport)
{
    std::printf("\n[7] 渲染占位（grab 走完整 paintEvent）\n");

    const QImage red = makeMarkerRed();

    Document doc(red.size(), Qt::transparent);
    if (Layer *bg = doc.layers().layer(0)) {
        bg->setImage(red);
        doc.markDirty();
    }

    CanvasView cv;
    cv.show();                 // 见 [6]：不 show 的话 resize 不会派发 resizeEvent
    cv.resize(viewport);
    QApplication::processEvents();
    cv.setDocument(&doc);      // 内部 zoomToFit
    cv.zoomToFit();

    // ── 7.1 适合窗口：红色区域应恰好等于矩阵算出的图像矩形 ──
    {
        const QRectF expect = cv.imageToWidget(
            QRectF(QPointF(0, 0), QSizeF(doc.canvasSize())));

        const QImage shot = cv.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QRect got = redBoundingBox(shot);

        std::printf("     视口 %d×%d\n", viewport.width(), viewport.height());
        std::printf("     期望图像矩形 x=%6.1f y=%6.1f w=%6.1f h=%6.1f\n",
                    expect.x(), expect.y(), expect.width(), expect.height());
        std::printf("     实测红色包围盒 x=%4d y=%4d w=%4d h=%4d  （渲染结果 %d×%d）\n",
                    got.x(), got.y(), got.width(), got.height(),
                    shot.width(), shot.height());

        check(!got.isEmpty(), "渲染结果里能找到图像（不是一片背景灰）");

        // 边框的深色描边压在图像最外圈上，会让包围盒每边各缩进约 1 像素，
        // 加上缩放的取整，容差给 3 像素。
        checkNear(got.left(),   expect.left(),   3.0, "渲染出的图像左边界位置正确");
        checkNear(got.top(),    expect.top(),    3.0, "渲染出的图像上边界位置正确");
        checkNear(got.width(),  expect.width(),  4.0, "渲染出的图像宽度正确（= 用户看到的显示尺寸）");
        checkNear(got.height(), expect.height(), 4.0, "渲染出的图像高度正确（= 用户看到的显示尺寸）");

        // 整幅图必须完整落在视口里 —— 这是"打开照片后尺寸不对"最直接的症状
        check(got.left() >= 0 && got.top() >= 0
                  && got.right() < shot.width() && got.bottom() < shot.height(),
              "渲染出的图像完整落在视口内（没有被裁掉一角）");
    }

    // ── 7.2 100%：图像远大于视口，红色区域应铺满整个视口 ──
    //
    //  3840×2160 在 100% 下必然超出 1280×800 的视口，
    //  所以四个方向都应该顶到边 —— 只要有一边露出背景灰，
    //  就说明图像被画小/画偏了。
    {
        cv.setZoom(1.0, QPointF(viewport.width() / 2.0, viewport.height() / 2.0));
        cv.setOffset(QPointF(0, 0));      // 图像左上角对齐控件左上角

        const QImage shot = cv.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QRect got = redBoundingBox(shot);

        std::printf("     100%% + 图像左上角对齐控件左上角：红色包围盒 x=%d y=%d w=%d h=%d\n",
                    got.x(), got.y(), got.width(), got.height());

        check(got.left() <= 1 && got.top() <= 1,
              "100% 时图像左上角贴合控件左上角");
        check(got.right() >= shot.width() - 2 && got.bottom() >= shot.height() - 2,
              "100% 时图像铺满视口（右侧/下侧没有露出背景）");
    }
}

// ---------------------------------------------------------------------------
//  8. 预览画布尺寸（任意角度旋转的预览）
// ---------------------------------------------------------------------------
//
//  任意角度旋转会**改变画布尺寸**，所以 CanvasView 的预览通道多了一维：
//  "这张预览图对应多大的画布"。这条测试钉的就是这一维。
//
//  ★ 漏掉它会怎样 ★
//    画布仍按**文档的旧尺寸**呈现，而预览图是"旋转后的大图"。于是
//    撑到旧画布之外的那四个角被旧取景框裁掉 —— 而真实结果里那里是
//    填充区。用户看到的是"旋转把内容裁了"，且不报错、不崩、数学全对。
//
//  ★ 判据只看渲染结果 ★
//    预览图（绿）与文档内容（红）在三个通道上都拉开距离，于是能从
//    grab() 出的像素里干净地量出"预览被画到了多大"。只断言
//    setPreviewImage 的入参等于在测"我自己传的参数"。
// ---------------------------------------------------------------------------

static QImage makeMarkerGreen(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(qRgba(0, 200, 0, 255));
    return img;
}

static QRect greenBoundingBox(const QImage &shot)
{
    int minX = shot.width(), minY = shot.height(), maxX = -1, maxY = -1;

    for (int y = 0; y < shot.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(shot.constScanLine(y));
        for (int x = 0; x < shot.width(); ++x) {
            const QRgb c = line[x];
            // 明显的绿：绿通道高、红蓝通道低。
            // 边框那条深色描边会把最外圈压暗，容差已经算上。
            if (qGreen(c) > 150 && qRed(c) < 90 && qBlue(c) < 90) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    if (maxX < 0)
        return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

static int countGreen(const QImage &shot)
{
    int n = 0;
    for (int y = 0; y < shot.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(shot.constScanLine(y));
        for (int x = 0; x < shot.width(); ++x) {
            const QRgb c = line[x];
            if (qGreen(c) > 150 && qRed(c) < 90 && qBlue(c) < 90)
                ++n;
        }
    }
    return n;
}

static void testPreviewCanvasSize(const QSize &viewport)
{
    std::printf("\n[8] 预览画布尺寸（旋转预览）\n");

    // 文档 100×80 纯红；预览 200×80 纯绿 —— 即"旋转后画布变成 200×80"。
    // 两个尺寸刻意不等宽，这样"画布按哪一份尺寸呈现"一眼可辨。
    const QSize docSize(100, 80);
    const QSize previewCanvasSize(200, 80);

    QImage red(docSize, QImage::Format_ARGB32_Premultiplied);
    red.fill(qRgba(255, 0, 0, 255));

    Document doc(docSize, Qt::transparent);
    if (Layer *bg = doc.layers().layer(0)) {
        bg->setImage(red);
        doc.markDirty();
    }

    CanvasView cv;
    cv.show();
    cv.resize(viewport);
    QApplication::processEvents();
    cv.setDocument(&doc);

    // 先摆成"100% + 图像左上角对齐控件左上角"，位置断言才好读
    cv.setZoom(1.0, QPointF(viewport.width() / 2.0, viewport.height() / 2.0));
    cv.setOffset(QPointF(0, 0));

    // ── 8.1 没有预览时，红色区域就是文档画布 ──
    {
        const QImage shot = cv.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QRect got = redBoundingBox(shot);

        std::printf("     无预览：红色包围盒 x=%d y=%d w=%d h=%d\n",
                    got.x(), got.y(), got.width(), got.height());

        checkNear(got.width(),  docSize.width(),  4.0, "无预览时按文档尺寸呈现（宽）");
        checkNear(got.height(), docSize.height(), 4.0, "无预览时按文档尺寸呈现（高）");
    }

    // ── 8.2 设置带画布尺寸的预览：应铺满**新**画布，而不是旧画布 ──
    {
        const QImage green = makeMarkerGreen(previewCanvasSize.width(),
                                             previewCanvasSize.height());
        cv.setPreviewImage(green, previewCanvasSize);

        // setPreviewImage 在画布尺寸变化时会自动 zoomToFit（让用户看到
        // 旋转出来的四角）。这里重新摆回 100% 对齐，位置断言才有唯一解释。
        cv.setZoom(1.0, QPointF(viewport.width() / 2.0, viewport.height() / 2.0));
        cv.setOffset(QPointF(0, 0));

        const QImage shot = cv.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QRect got = greenBoundingBox(shot);

        std::printf("     预览画布 %d×%d：绿色包围盒 x=%d y=%d w=%d h=%d\n",
                    previewCanvasSize.width(), previewCanvasSize.height(),
                    got.x(), got.y(), got.width(), got.height());

        check(!got.isEmpty(), "预览图确实被渲染出来了");
        checkNear(got.left(), 0.0, 3.0, "预览从左边界开始");
        checkNear(got.top(),  0.0, 3.0, "预览从上边界开始");
        checkNear(got.width(),  previewCanvasSize.width(),  4.0,
                  "★ 预览按**自带的画布尺寸**呈现（而不是文档的旧尺寸）");
        checkNear(got.height(), previewCanvasSize.height(), 4.0,
                  "★ 预览的高度按自带画布尺寸呈现");

        // 这条是"抓 bug"的关键判据：漏掉预览画布尺寸时，预览会被挤在
        // 100 宽的旧画布里，不可能超过文档宽度多少。
        check(got.width() > docSize.width() + 50,
              "★ 预览明显超出文档画布（证明画布尺寸确实换成了预览的）");

        // 红色是文档的真实内容，此刻应当被预览完全盖住
        check(redBoundingBox(shot).isEmpty(),
              "预览期间画布画的是预览图，不是文档内容");
    }

    // ── 8.3 清除预览：画面必须完全回到文档，不留任何痕迹 ──
    //
    //  ★ 这里同时钉住"预览画布尺寸也要一起清" ★
    //    只清图不清尺寸的话，画布仍按 200×80 呈现，右侧会凭空多出
    //    一片棋盘格，且"适合窗口"会按错误的尺寸算。
    {
        cv.clearPreviewImage();

        const QImage shot = cv.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QRect gotRed = redBoundingBox(shot);
        const int green = countGreen(shot);

        std::printf("     清除预览：红色包围盒 w=%d h=%d，残留绿色像素 %d\n",
                    gotRed.width(), gotRed.height(), green);

        checkEqInt(green, 0, "清除预览后一个预览像素都不剩");
        checkNear(gotRed.width(),  docSize.width(),  4.0,
                  "清除预览后画布尺寸回到文档的（宽）");
        checkNear(gotRed.height(), docSize.height(), 4.0,
                  "清除预览后画布尺寸回到文档的（高）");
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 画布视图 离屏测试\n");
    std::printf("================================================\n");

    // 有命令行参数就加载真实文件，否则用合成图
    QImage photo;
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        ImageMetadata meta;
        photo = loadImageWithMetadata(path, &meta);
        std::printf("\n加载: %s\n", qPrintable(path));
        std::printf("     结果 %d×%d  dpi=(%.1f, %.1f)\n",
                    photo.width(), photo.height(), meta.dpiX, meta.dpiY);
    }
    if (photo.isNull()) {
        photo = makeSynthetic();
        std::printf("\n使用合成图 %d×%d\n", photo.width(), photo.height());
    }

    Document doc(photo.size(), Qt::transparent);
    if (Layer *bg = doc.layers().layer(0)) {
        bg->setImage(photo);
        doc.markDirty();
    }

    CanvasView cv;
    const QSize viewport(1280, 800);

    testDocumentSize(photo);
    testZoomToFit(cv, doc, viewport);
    testWheelZoom(cv, doc);
    testCoordinateRoundTrip(cv);
    testPanFollowsCursor(cv, doc);
    testStartupSequence(photo, viewport);
    testRenderedGeometry(viewport);
    testPreviewCanvasSize(viewport);

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
