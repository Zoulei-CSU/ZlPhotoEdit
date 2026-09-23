// ============================================================================
//  perf_test.cpp —— 脏矩形增量合成的离线自检
// ----------------------------------------------------------------------------
//  这个功能**唯一**的失效方式是"画面看起来正常，但某一块停在了旧内容上"：
//  不崩、不报错、松手后可能就恢复了 —— 属于最难复现的一类 bug。
//
//  所以自检不能只验"composite() 返回了非空图"（那种断言什么都抓不住），
//  必须验三件事：
//
//    ① 【正确性基石】区域合成 == 全图合成的对应子区域，**逐位相等**，22 种模式
//       成立的原因：本项目 22 种混合模式全是逐像素（pointwise）运算，
//       没有任何空间邻域运算（无模糊/位移/卷积）→ 矩形内独立合成是精确的
//    ② 【走对了路径】连续局部失效**确实没有**触发全图重算
//       —— 靠 Document 的遥测计数。否则"结果正确"可能只是因为一直在偷偷全量重算
//    ③ 【边界不泄漏】脏矩形外一个像素都不许变
//
//  第二批（2026-09-21）新增三组：
//    ⑧ 【选区轮廓】engine/outline::maskOutline 的算法正确性（精确到线段端点）
//       + 实测 4K 蒙版下的重建成本 —— 评估文档要求的"先量后改"
//       ★ 含 8.2b：hint 给小了会**静默**长出一圈假轮廓（契约，不是 bug）——
//         钉住"真实调用点必须传 Selection::bounds()"这条安全边界
//         （假轮廓落在"扩 1 后的扫描框"上，不是 hint 原样，端点别写错）
//    ⑨ 【选区修订号】"只有真变了才递增"，且全局唯一（撤销赋回旧快照不许撞号）
//    ⑩ 【ImageCommand 区域】可选脏区域被采纳；并证明 restoreImage() 里
//       那句画蛇添足的 markDirty() 已经不在了（在的话区域会被降级成全量）
//
//  第三批（2026-09-22）新增一组：
//    ⑪ 【不透明上层直接覆盖】mode ∈ {Normal, Dissolve} 且图层不透明度 = 1.0
//       且上层像素 alpha == 255 时，整条 source-over **精确退化**成 dst = src
//       （实测 7.8~9.4×，见 docs/性能优化评估.md §4.5）。
//       判据 = 【逐位相同（0 容差）】+【命中计数吻合】—— 缺一条都证明不了：
//       只验结果对（慢路径也算得对）说明不了快路径生效；只验计数说明不了没写错。
//       候选集由 blendChannel 反推（不是抄一份模式清单），
//       所以将来"溶解"真引入随机抖动时，11.1 会立刻变红。
//
//  ★★ 这里的容差必须为 0，不是"误差 ≤ 1" ★★
//     整条脏矩形方案的正确性全押在"逐位相等"上。一旦为了迁就某个数值改动
//     而把它放宽成 ±1，"脏矩形算小了"这类 bug 就会从这条断言下面漏过去
//     （脏 1 个像素与差 1 个数值，在放大后的画面上长得一模一样）。
//     这条理由写在 docs/性能优化评估.md §2.1（正确性基石）与 §4.1（方案 B 的口径风险）。
//
//  编译： ./run_tests.sh perf
//  ★ 需要 Qt5Widgets ★ —— 第 ⑥ 组要构造真正的 CommandStack 来验"压栈不再
//    全图重算"，而 CommandStack.h 里 `#include <QUndoStack>`（Qt5 起
//    QUndoStack / QUndoCommand 都在 QtWidgets 模块）。少加 Qt5Widgets.lib
//    只在**链接期**报"无法解析的外部符号 … QUndoStack …"，
//    看起来像"少编译了某个 .cpp"，其实只是少了一个 lib。
// ============================================================================

#include "core/Document.h"
#include "core/ImageTypes.h"
#include "core/Layer.h"
#include "engine/Blend.h"

#include "commands/CommandStack.h"
#include "commands/DrawCommand.h"
#include "commands/ImageCommand.h"
#include "core/Selection.h"
#include "engine/Outline.h"

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <cstdio>

using namespace zl;

static int g_passed = 0;
static int g_failed = 0;

static void beginGroup(const char *title)
{
    std::printf("\n-- %s\n", title);
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

static void checkEq(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

// 带上下文的版本（模式名 / 矩形要写进消息里，否则 16×7 个断言失败时分不清是哪个）
static void checkMsg(bool ok, const QString &what)
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n", qUtf8Printable(what));
    }
}

static void checkEqMsg(int actual, int expected, const QString &what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n",
                    qUtf8Printable(what), expected, actual);
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  确定性伪随机（不用 QRandomGenerator：自检必须每次跑到同一组数据上）
// ---------------------------------------------------------------------------
static quint32 g_seed = 0x12345678u;

static quint32 nextRand()
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

static QString rectText(const QRect &r)
{
    return QStringLiteral("(%1,%2 %3x%4)").arg(r.x()).arg(r.y())
                                          .arg(r.width()).arg(r.height());
}

// 逐位比较两张同尺寸图，返回不同像素的个数（尺寸不符返回 -1）
//
// ★ 用 constScanLine + QRgb 指针，不用 pixel() ★
//   pixel() 每次都要做格式判定与乘法换算，96×72×16×7 次调用会明显拖慢自检，
//   而且它按格式还原颜色 —— 我们要的是**内存原值**逐位相等。
static int diffPixels(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1;

    int bad = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *la = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *lb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (la[x] != lb[x])
                ++bad;
        }
    }
    return bad;
}

// 造一张"带各种 alpha"的预乘噪声图
//
// ★ 这里刻意**安排**透明/半透明/不透明三种像素，不靠随机数碰运气 ★
//   ① 组的断言是"区域合成 == 全图合成的子区域"。若三层恰好都是全透明
//   （或全不透明），这条断言就只是在比较几张空图 —— 全绿却什么都没验到。
//   这类"验的是空集"的坑比断言写错更难发现，因为它永远不会红。
//   于是把三块区域写死：
//     · (x<2 && y<2)   → 所有图层都透明     → 合成结果**透明**
//     · (2..5, 2..5)   → 只有中层有半透明墨 → 合成结果**半透明**
//     · 其余           → 底层不透明         → 合成结果**不透明**
//   颜色仍逐像素随机，好让 22 种混合模式都有足够变化可算。
static QImage makeNoisyLayer(const QSize &size, int variant)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);

    for (int y = 0; y < size.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const quint32 r = nextRand();

            int a;
            if (x < 2 && y < 2) {
                a = 0;                                        // 透明角
            } else if (x < 6 && y < 6) {
                a = (variant == 1) ? 128 : 0;                 // 仅中层半透明
            } else {
                a = (variant == 1) ? (128 + int(r % 128)) : 255;   // 底层不透明
            }

            if (a == 0) {
                line[x] = 0;
                continue;
            }

            // 写入**预乘**值（否则就不是合法预乘像素了）
            const int cr = int((r >> 0) & 0xFF);
            const int cg = int((r >> 8) & 0xFF);
            const int cb = int((r >> 16) & 0xFF);
            line[x] = qRgba(cr * a / 255, cg * a / 255, cb * a / 255, a);
        }
    }

    return img;
}

// ===========================================================================
//  ① 正确性基石
// ===========================================================================
static void testRegionEqualsFullSubRect()
{
    beginGroup("① ★ 正确性基石：区域合成 == 全图合成的对应子区域（逐位相等）");

    const QSize canvas(96, 72);
    g_seed = 0xC0FFEEu;

    QList<QImage>  images;
    QList<qreal>   opacities;
    QList<bool>    visible;
    for (int i = 0; i < 3; ++i) {
        images.append(makeNoisyLayer(canvas, i));
        opacities.append(i == 1 ? 0.75 : 1.0);   // 顺带覆盖"图层不透明度"这条乘法
        visible.append(true);
    }

    // ── 前提断言：这些数据必须真的能产生"内容" ──
    //
    // ★ 为什么必须单独验这一条 ★
    //   如果三层恰好都是全透明，"逐位相等"就只是在比较三张空图 ——
    //   断言全绿但什么都没验到。这类"看起来对、其实验的是空集"的坑，
    //   比断言写错更难发现，因为它永远不会红。
    {
        const QImage probe = blend::compositeLayers(
            images, QList<BlendMode>{ BlendMode::Normal, BlendMode::Normal, BlendMode::Normal },
            opacities, visible, canvas, Qt::transparent);

        int transparent = 0, opaque = 0, semi = 0;
        for (int y = 0; y < canvas.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(probe.constScanLine(y));
            for (int x = 0; x < canvas.width(); ++x) {
                const int a = qAlpha(line[x]);
                if (a == 0)        ++transparent;
                else if (a == 255) ++opaque;
                else               ++semi;
            }
        }
        checkMsg(transparent > 0 && opaque > 0 && semi > 0,
                 QStringLiteral("前提：合成结果里同时有透明/半透明/不透明像素"
                                "（否则本组断言验的是空集）"));
        std::printf("     （合成结果：透明 %d，半透明 %d，不透明 %d，共 %d 像素）\n",
                    transparent, semi, opaque, canvas.width() * canvas.height());
    }

    const QList<BlendMode> modes = allBlendModes();
    // ★ 22 而不是 16 ★
    //   枚举注释里写的是"与 Photoshop 的 16 种常用模式对齐"，但实际
    //   allBlendModes() 已经长到 22 项（含 Dissolve 占位）。这里钉死真实数量：
    //   哪天有人加/删模式，本断言会立刻红，提醒"覆盖策略要跟着重想"。
    checkEq(modes.size(), 22, "★ 混合模式确实是 22 种（数量变了就必须重新想覆盖策略）");

    // ★ 必须包含**非原点**的矩形 ★
    //   如果只测"全图"和"(0,0,1,1)"，那么"把整张图层当成补丁传进去"
    //   这种错位 bug 完全测不出来：只有 r.topLeft() != (0,0) 时才会露出来。
    const QRect regions[] = {
        QRect(0, 0, canvas.width(), canvas.height()),           // 全图（走转发路径）
        QRect(0, 0, 1, 1),                                      // 单像素 · 原点
        QRect(1, 1, 1, 1),                                      // ★ 单像素 · 非原点
        QRect(3, 5, 17, 13),                                    // 内部一块（非原点）
        QRect(canvas.width() - 7, 4, 7, 9),                     // 贴右边
        QRect(2, canvas.height() - 6, 11, 6),                   // 贴下边
        QRect(canvas.width() - 3, canvas.height() - 3, 3, 3),   // 右下角
        QRect(0, 0, canvas.width(), 1),                         // 极扁（整行）
        QRect(0, 0, 1, canvas.height())                         // 极窄（整列）
    };
    const int regionCount = int(sizeof(regions) / sizeof(regions[0]));

    int totalMismatch = 0;

    for (int mi = 0; mi < modes.size(); ++mi) {
        const BlendMode m = modes.at(mi);
        const QList<BlendMode> use{ m, m, m };

        const QImage full = blend::compositeLayers(images, use, opacities,
                                                  visible, canvas, Qt::transparent);
        const QString mname = blendModeName(m);

        for (int ri = 0; ri < regionCount; ++ri) {
            const QRect r = regions[ri];
            const QImage part = blend::compositeLayersRegion(images, use, opacities,
                                                             visible, canvas, r,
                                                             Qt::transparent);

            checkEqMsg(part.isNull() ? -1 : part.width(), r.width(),
                       QStringLiteral("%1 %2 补丁宽度正确").arg(mname, rectText(r)));
            checkEqMsg(part.isNull() ? -1 : part.height(), r.height(),
                       QStringLiteral("%1 %2 补丁高度正确").arg(mname, rectText(r)));

            int bad = 0;
            if (!part.isNull()) {
                for (int y = 0; y < r.height() && bad == 0; ++y) {
                    const QRgb *lp = reinterpret_cast<const QRgb *>(part.constScanLine(y));
                    const QRgb *lf = reinterpret_cast<const QRgb *>(
                        full.constScanLine(r.y() + y));
                    for (int x = 0; x < r.width(); ++x) {
                        if (lp[x] != lf[r.x() + x]) {
                            ++bad;
                            break;
                        }
                    }
                }
            } else {
                bad = -1;
            }

            totalMismatch += (bad == 0 ? 0 : 1);
            checkEqMsg(bad, 0,
                       QStringLiteral("★ %1 %2 与全图对应子区域逐位相等")
                           .arg(mname, rectText(r)));
        }
    }

    checkEq(totalMismatch, 0,
            "★ 22 种模式 × 全部矩形，没有任何一处不一致（容差 0）");
}

// ===========================================================================
//  ② 累积正确性（走真实的 Document，不是裸调 engine）
// ===========================================================================
static void testAccumulation()
{
    beginGroup("② 累积正确性：连续局部失效后，整张缓存 == 全量重算");

    const QSize canvas(80, 60);
    Document doc(canvas, Qt::transparent);

    doc.addLayer(QStringLiteral("中层"));
    doc.addLayer(QStringLiteral("上层"));

    // 让三层的混合模式/不透明度各不相同，避免"三层恒等"把 bug 掩盖掉
    if (Layer *l1 = doc.layers().layer(1)) {
        l1->setBlendMode(BlendMode::Multiply);
        l1->setOpacity(180);
    }
    if (Layer *l2 = doc.layers().layer(2)) {
        l2->setBlendMode(BlendMode::Screen);
        l2->setOpacity(120);
    }

    // 参照实现：每次都用"文档当前的图层数据"独立全量合成一遍
    auto reference = [&doc, canvas]() -> QImage {
        QList<QImage>    imgs;
        QList<BlendMode> modes;
        QList<qreal>     ops;
        QList<bool>      vis;
        for (int i = 0; i < doc.layerCount(); ++i) {
            const Layer *l = doc.layers().layer(i);
            if (!l)
                continue;
            imgs.append(l->image());
            modes.append(l->blendMode());
            ops.append(l->opacityF());
            vis.append(l->isVisible());
        }
        return blend::compositeLayers(imgs, modes, ops, vis, canvas, Qt::transparent);
    };

    (void)doc.composite();                      // 建立缓存，并清掉"全量失效"
    const quint64 full0 = doc.fullCompositeCount();
    const quint64 reg0  = doc.regionCompositeCount();

    int mismatch = 0;
    int sizeOk = 0;

    for (int k = 0; k < 12; ++k) {
        // 在随机图层上画一块随机矩形（模拟画笔）
        const int li = k % doc.layerCount();
        Layer *layer = doc.layers().layer(li);
        if (!layer)
            continue;

        const int w = 3 + int(nextRand() % 20);
        const int h = 3 + int(nextRand() % 16);
        const int x = int(nextRand() % quint32(qMax(1, canvas.width()  - w)));
        const int y = int(nextRand() % quint32(qMax(1, canvas.height() - h)));
        const QRect r(x, y, w, h);

        QImage &img = layer->mutableImage();
        {
            QPainter p(&img);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(r, QColor(int(nextRand() % 256),
                                 int(nextRand() % 256),
                                 int(nextRand() % 256), 255));
            p.end();
        }

        doc.markDirtyRegion(r);

        const QImage got    = doc.composite();
        const QImage expect = reference();

        const int bad = diffPixels(got, expect);
        if (bad != 0) {
            ++mismatch;
            std::printf("     第 %d 次局部失效后不一致（%d 个像素不同，脏矩形 %s）\n",
                        k + 1, bad, qUtf8Printable(rectText(r)));
        }
        if (got.size() == canvas)
            ++sizeOk;
    }

    checkEq(mismatch, 0, "★★ 12 次局部失效后，缓存与全量重算逐位一致");
    checkEq(sizeOk, 12,  "12 次结果尺寸都等于画布");
    checkEq(int(doc.fullCompositeCount() - full0), 0,
            "★★ 全程没有触发过任何一次全图重算");
    checkEq(int(doc.regionCompositeCount() - reg0), 12,
            "12 次都走了区域快路径");
}

// ===========================================================================
//  ③ 兜底路径（"拿不准就全量"的安全阀）
// ===========================================================================
static void testFallback()
{
    beginGroup("③ 兜底：拿不准时必须退回全量重算");

    Document doc(QSize(64, 48), Qt::transparent);
    (void)doc.composite();
    const quint64 f0 = doc.fullCompositeCount();

    // ① 空矩形
    doc.markDirtyRegion(QRect());
    (void)doc.composite();
    checkEq(int(doc.fullCompositeCount() - f0), 1,
            "★ 空矩形 → 按全量处理（不赌【什么都没变】）");

    // ② 全量失效之后再报一个小矩形，不得被"降级"
    doc.markDirty();
    doc.markDirtyRegion(QRect(2, 2, 4, 4));
    (void)doc.composite();
    checkEq(int(doc.fullCompositeCount() - f0), 2,
            "★★ markDirty() 后紧跟一块 markDirtyRegion，仍必须是整图重算");

    // ③ 完全在画布外的矩形：求交后为空 → 也走全量，且绝不越界写入
    doc.markDirtyRegion(QRect(10000, 10000, 10, 10));
    (void)doc.composite();
    checkEq(int(doc.fullCompositeCount() - f0), 3,
            "画布外的脏矩形 → 走全量（宁多算，不越界）");

    // ④ 局部失效之后紧跟 markDirty()，必须整图重算
    doc.markDirtyRegion(QRect(5, 5, 6, 6));
    doc.markDirty();
    (void)doc.composite();
    checkEq(int(doc.fullCompositeCount() - f0), 4,
            "★★ 局部失效后紧跟 markDirty()，必须整图重算（兜底不被快路径吃掉）");

    // ⑤ pendingDirtyRegion() 的约定：空 = 整画布都脏
    checkMsg(doc.pendingDirtyRegion().isEmpty(),
             QStringLiteral("合成之后脏区域被清空"));
    doc.markDirty();
    checkMsg(doc.pendingDirtyRegion().isEmpty(),
             QStringLiteral("★ 全量失效时 pendingDirtyRegion() 返回空（= 整图，不是【无需重绘】）"));

    // ★★ 全量失效必须"吃住"后续的局部失效 —— 直到下一次合成 ★★
    //   这是最容易写反的一条：如果 markDirtyRegion() 把"整图脏"**降级**成
    //   一个小矩形，视图就会只重绘那一小块，画面其余部分停在旧内容上。
    //   （真实场景：切换图层可见性 = 整图变，紧接着画笔又报了个小矩形。）
    doc.markDirtyRegion(QRect(7, 8, 5, 6));
    checkMsg(doc.pendingDirtyRegion().isEmpty(),
             QStringLiteral("★★ 全量失效期间追加的局部失效，仍按整图算（安全阀优先）"));

    // 清掉全量标志，回到"只有局部脏"的状态，再看累积
    (void)doc.composite();
    doc.markDirtyRegion(QRect(7, 8, 5, 6));
    checkMsg(doc.pendingDirtyRegion() == QRect(7, 8, 5, 6),
             QStringLiteral("局部失效时 pendingDirtyRegion() 返回该矩形"));
    doc.markDirtyRegion(QRect(9, 9, 2, 2));
    checkMsg(doc.pendingDirtyRegion() == QRect(7, 8, 5, 6).united(QRect(9, 9, 2, 2)),
             QStringLiteral("★ 多次局部失效会累积成并集（否则中间那块永远不刷新）"));
}

// ===========================================================================
//  ④ 边界不泄漏：脏矩形外一个像素都不许变
// ===========================================================================
static void testRingUntouched()
{
    beginGroup("④ 边界：脏矩形内更新、矩形外一个像素都不许变");

    const QSize canvas(64, 48);
    Document doc(canvas, Qt::transparent);
    Layer *layer = doc.layers().layer(0);
    checkMsg(layer != nullptr, QStringLiteral("文档自带一个图层"));
    if (!layer)
        return;

    // 铺成不透明底色：任何残留/漏更新都会被立刻看出来
    layer->mutableImage().fill(QColor(30, 60, 90));

    const QImage before = doc.composite().copy();

    const QRect r(20, 15, 12, 9);
    {
        QPainter p(&layer->mutableImage());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(r, QColor(220, 40, 40));
        p.end();
    }
    doc.markDirtyRegion(r);

    const QImage after = doc.composite();

    int innerBad = 0;
    int ringBad  = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        const QRgb *lb = reinterpret_cast<const QRgb *>(before.constScanLine(y));
        const QRgb *la = reinterpret_cast<const QRgb *>(after.constScanLine(y));
        for (int x = 0; x < canvas.width(); ++x) {
            if (r.contains(x, y)) {
                if (la[x] != qRgba(220, 40, 40, 255))
                    ++innerBad;
            } else if (la[x] != lb[x]) {
                ++ringBad;
            }
        }
    }

    checkEq(innerBad, 0, "★ 块内已经更新成新颜色（抓【贴回偏移错位】）");
    checkEq(ringBad, 0,  "★★ 块外一个像素都没变（抓【脏矩形算大/算小、漏更新】）");
}

// ===========================================================================
//  ⑤ 遥测：证明真的走了快路径
// ===========================================================================
static void testTelemetry()
{
    beginGroup("⑤ 遥测：连续 20 次局部失效，一次全图重算都不许有");

    Document doc(QSize(120, 90), Qt::transparent);
    Layer *layer = doc.layers().layer(0);
    if (!layer) {
        checkMsg(false, QStringLiteral("文档自带一个图层"));
        return;
    }
    layer->mutableImage().fill(QColor(15, 25, 35));

    checkEq(int(doc.fullCompositeCount()), 0, "新文档还没合成过");
    (void)doc.composite();
    checkEq(int(doc.fullCompositeCount()), 1, "第一次合成必然是全量");
    checkEq(int(doc.regionCompositeCount()), 0, "第一次合成不走区域路径");

    const quint64 full0 = doc.fullCompositeCount();
    const quint64 reg0  = doc.regionCompositeCount();

    for (int i = 0; i < 20; ++i) {
        const QRect r(5 + (i % 7) * 3, 4 + (i % 5) * 4, 6, 5);
        {
            QPainter p(&layer->mutableImage());
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(r, QColor(200 - i, 100, 50, 255));
            p.end();
        }
        doc.markDirtyRegion(r);
        (void)doc.composite();
    }

    checkEq(int(doc.fullCompositeCount() - full0), 0,
            "★★ 20 次局部失效，全图重算次数 = 0");
    checkEq(int(doc.regionCompositeCount() - reg0), 20,
            "★ 20 次全部走了区域快路径");
}

// ===========================================================================
//  ⑥ 撤销栈：DrawCommand 报出的矩形被采纳（压栈/撤销/重做都不再全图重算）
// ===========================================================================
static void testCommandRegion()
{
    beginGroup("⑥ 撤销栈：DrawCommand 的区域被采纳，压栈不再全图重算");

    const QSize canvas(64, 48);
    Document doc(canvas, Qt::transparent);
    Layer *layer = doc.layers().layer(0);
    if (!layer) {
        checkMsg(false, QStringLiteral("文档自带一个图层"));
        return;
    }
    layer->mutableImage().fill(QColor(10, 20, 30));

    // ★ 栈必须在第一次 composite() 之前建 ★
    //   CommandStack 构造时会连 cleanChanged → setModified，
    //   放在后面建会让"基线计数"的取法变得难以推理。
    CommandStack stack(&doc);

    (void)doc.composite();
    const quint64 full0 = doc.fullCompositeCount();
    const quint64 reg0  = doc.regionCompositeCount();

    const QRect r(14, 11, 9, 7);
    const QImage beforePatch = layer->image().copy(r);
    QImage afterPatch(beforePatch.size(), QImage::Format_ARGB32_Premultiplied);
    afterPatch.fill(QColor(200, 30, 30));

    checkMsg(doc.pendingDirtyRegion().isEmpty(),
             QStringLiteral("前提：上一轮合成之后脏区域是空的"));

    // 直接改图层像素（模拟工具拖动期间的直接写回），再压命令
    {
        QPainter p(&layer->mutableImage());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(r.topLeft(), afterPatch);
        p.end();
    }

    stack.push(DrawCommand::standalone(&doc, 0, r, beforePatch, afterPatch,
                                       QStringLiteral("画笔描边")));

    checkMsg(doc.pendingDirtyRegion() == r,
             QStringLiteral("★ DrawCommand 声明的脏区域被原样采纳"));
    checkEq(int(doc.fullCompositeCount() - full0), 0,
            "★★ 压栈（= 每画完一笔松手）不再触发全图重算");

    const QImage afterPush = doc.composite();
    checkEq(int(doc.regionCompositeCount() - reg0), 1, "这次合成走的是区域快路径");
    checkEq(qRed(afterPush.pixel(16, 13)), 200, "矩形内确实是新颜色");

    // ── 撤销 ──
    const quint64 full1 = doc.fullCompositeCount();
    const quint64 reg1  = doc.regionCompositeCount();

    stack.undo();
    checkMsg(doc.pendingDirtyRegion() == r,
             QStringLiteral("★ 撤销同样只失效 DrawCommand 的矩形"));
    checkEq(int(doc.fullCompositeCount() - full1), 0, "★ 撤销不再全图重算");

    const QImage afterUndo = doc.composite();
    checkEq(int(doc.regionCompositeCount() - reg1), 1, "撤销后的合成也走区域快路径");
    checkEq(qRed(afterUndo.pixel(16, 13)), 10, "撤销后矩形内回到 before 的颜色");
    checkEq(qRed(afterUndo.pixel(0, 0)), 10, "矩形外仍是底色（撤销没有波及别处）");

    // ── 重做 ──
    const quint64 full2 = doc.fullCompositeCount();
    const quint64 reg2  = doc.regionCompositeCount();

    stack.redo();
    checkMsg(doc.pendingDirtyRegion() == r, QStringLiteral("★ 重做也只失效该矩形"));
    checkEq(int(doc.fullCompositeCount() - full2), 0, "★ 重做不再全图重算");

    const QImage afterRedo = doc.composite();
    checkEq(int(doc.regionCompositeCount() - reg2), 1, "重做后的合成也走区域快路径");
    checkEq(qRed(afterRedo.pixel(16, 13)), 200, "重做后矩形内又是新颜色");

    // ── 不实现 IRegionAware 的情况必须仍然全量 ──
    //   用一个普通 QUndoCommand 压栈，验证"默认全量"这条安全阀还在。
    class PlainCommand : public QUndoCommand
    {
    public:
        explicit PlainCommand(QUndoCommand *parent = nullptr)
            : QUndoCommand(QStringLiteral("野生命令"), parent) {}
        void undo() override {}
        void redo() override {}
    };

    const quint64 full3 = doc.fullCompositeCount();
    stack.push(new PlainCommand);
    checkMsg(doc.pendingDirtyRegion().isEmpty(),
             QStringLiteral("★★ 没实现 IRegionAware 的命令 → 默认整图失效（安全阀仍在）"));
    (void)doc.composite();
    checkEq(int(doc.fullCompositeCount() - full3), 1,
            "★ 野生命令确实触发了全图重算");
}

// ===========================================================================
//  ⑦ 成本与写时复制（诊断为主 + 一条粗粒度的防退化断言）
// ===========================================================================
static void testCostAndCow()
{
    beginGroup("⑦ 成本与写时复制");

    const QSize canvas(1600, 1200);        // 缓存约 7.7 MB，足以看出"贴回时被深拷贝"
    Document doc(canvas, Qt::transparent);
    doc.addLayer(QStringLiteral("中"));
    doc.addLayer(QStringLiteral("上"));

    for (int i = 0; i < doc.layerCount(); ++i) {
        Layer *l = doc.layers().layer(i);
        if (!l)
            continue;
        QImage &img = l->mutableImage();
        for (int y = 0; y < img.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                line[x] = qRgba((x * 3 + y) & 0xFF, (x + y * 5) & 0xFF,
                                (x * 7 + y) & 0xFF, 200);
            }
        }
    }

    QElapsedTimer timer;
    const QRect r(300, 300, 64, 64);

    auto paintOne = [&doc, &r]() {
        Layer *l = doc.layers().layer(0);
        if (!l)
            return;
        QPainter p(&l->mutableImage());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(r, QColor(255, 128, 0, 255));
        p.end();
    };

    // ── ① 一次全量合成的成本（做基线）──
    doc.markDirty();
    timer.start();
    const QImage full = doc.composite();
    const qint64 msFull = qMax<qint64>(timer.elapsed(), 1);
    checkMsg(!full.isNull(), QStringLiteral("全量合成有结果"));

    // ── ② 共享态下的 20 次区域贴回 ──
    //   模拟真实界面：画布会把合成结果留在自己的成员里，
    //   于是缓存一直被"外部"引用着 → QPainter 贴回时可能触发写时复制。
    QImage held = doc.composite();
    timer.start();
    for (int i = 0; i < 20; ++i) {
        paintOne();
        doc.markDirtyRegion(r);
        held = doc.composite();
    }
    const qint64 msShared = timer.elapsed();

    // ── ③ 无人持有时（引用计数回到 1）的 20 次区域贴回 ──
    held = QImage();
    timer.start();
    for (int i = 0; i < 20; ++i) {
        paintOne();
        doc.markDirtyRegion(r);
        (void)doc.composite();
    }
    const qint64 msFree = timer.elapsed();

    std::printf("     （1600x1200 x3 层：全量合成 %lld ms；"
                "20 次区域贴回 %lld ms（共享态）/ %lld ms（非共享态））\n",
                static_cast<long long>(msFull),
                static_cast<long long>(msShared),
                static_cast<long long>(msFree));

    // ★ 确定性探针（不是计时断言）：QPainter 画在一张**被共享**的 QImage 上
    //   会触发写时复制 —— 底层缓冲指针会换掉。这不是 Qt 的缺陷，是它的语义。
    //   把它钉在自检里，是为了让"合成缓存最好别被界面长期持有"这条结论
    //   有一个可执行的证据，而不是一句代码注释。
    {
        QImage probe(256, 256, QImage::Format_ARGB32_Premultiplied);
        probe.fill(Qt::red);
        QImage sharedRef = probe;                 // 引用计数 +1
        const void *p0 = probe.constBits();
        {
            QImage patch(4, 4, QImage::Format_ARGB32_Premultiplied);
            patch.fill(Qt::blue);
            QPainter p(&probe);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(QPoint(0, 0), patch);
            p.end();
        }
        const void *p1 = probe.constBits();
        checkMsg(p0 != p1,
                 QStringLiteral("共享中的 QImage 被 QPainter 绘制 → 写时复制（缓冲被换掉）"));
    }

    // ★ 防退化断言 ★
    //   只要求量级正确，不做脆弱的时间断言。
    //   判据选的是"20 次区域贴回的总耗时 < 1 次全量合成" —— 不用"单次耗时"，
    //   因为那要做整数除法：4ms/20 = 0，"0 * 30 < 101"恒真，断言会变成空转。
    //   真实机理下 20 次 64×64 补丁的像素量只有全图的 4% 左右，余量极大；
    //   一旦有人在区域路径里偷偷做整图重算，20 次会变成 20 张全图 → 立刻红。
    checkMsg(msFree * 5 < msFull,
             QStringLiteral("★ 20 次区域贴回的总耗时不到 1 次全量合成的 1/5"
                            "（%1 ms vs %2 ms；防止区域路径退化成全量）")
                 .arg(msFree).arg(msFull));
}


// ===========================================================================
//  ⑧ 选区轮廓（engine/outline::maskOutline）
// ===========================================================================

// 把路径拆成一组"整数端点的线段"。用它做断言而不是浮点比较 ——
// maskOutline 产出的坐标全是整数（扫描得到的行列号），换算成 qreal 是精确的。
struct Seg
{
    int x1, y1, x2, y2;
};

static QList<Seg> segmentsOf(const QPainterPath &path)
{
    QList<Seg> out;
    QPointF cur;
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) {
            cur = QPointF(e.x, e.y);
        } else if (e.type == QPainterPath::LineToElement) {
            const Seg seg{ qRound(cur.x()), qRound(cur.y()),
                           qRound(e.x),   qRound(e.y) };
            out.append(seg);
            cur = QPointF(e.x, e.y);
        }
    }
    return out;
}

// 线段是无向的：把两个端点按字典序排一下，便于做集合比较。
static Seg normalizeSeg(const Seg &s)
{
    const bool swap = (s.x1 > s.x2) || (s.x1 == s.x2 && s.y1 > s.y2);
    return swap ? Seg{ s.x2, s.y2, s.x1, s.y1 } : s;
}

// 两组线段是否**完全一致**（集合意义，逐条一一对应）。
static bool sameSegSet(QList<Seg> a, QList<Seg> b)
{
    if (a.size() != b.size())
        return false;

    for (int i = 0; i < a.size(); ++i)
        a[i] = normalizeSeg(a[i]);
    for (int i = 0; i < b.size(); ++i)
        b[i] = normalizeSeg(b[i]);

    for (const Seg &sa : a) {
        bool found = false;
        for (int i = 0; i < b.size(); ++i) {
            const Seg &sb = b[i];
            if (sa.x1 == sb.x1 && sa.y1 == sb.y1
                && sa.x2 == sb.x2 && sa.y2 == sb.y2) {
                b.removeAt(i);
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return b.isEmpty();
}

static QString segText(const QList<Seg> &segs)
{
    QString out;
    for (int i = 0; i < segs.size(); ++i) {
        if (i)
            out += QStringLiteral("、");
        out += QStringLiteral("(%1,%2)-(%3,%4)")
                   .arg(segs[i].x1).arg(segs[i].y1)
                   .arg(segs[i].x2).arg(segs[i].y2);
    }
    return out;
}

// 造一张"某矩形内为 v、其余为 0"的灰度蒙版
static QImage makeBlockMask(const QSize &size, const QRect &block, int v = 255)
{
    QImage m(size, QImage::Format_Grayscale8);
    m.fill(0);
    const QRect r = block.intersected(QRect(QPoint(0, 0), size));
    for (int y = r.top(); y <= r.bottom(); ++y) {
        uchar *line = m.scanLine(y);
        for (int x = r.left(); x <= r.right(); ++x)
            line[x] = uchar(v);
    }
    return m;
}

static void testOutlineAlgorithm()
{
    beginGroup("⑧ 选区轮廓：算法正确性（精确到线段端点）");

    // ── 8.1 一个 20×20 的实心方块 → 正好 4 段，落在方块四条边上 ──
    //
    // ★ 这里钉的是"坐标口径"：下边取 30、右边取 30（矩形外沿），
    //   而不是 29（最后一个像素）。这个口径来自原实现，
    //   是"边界线画在内外交界处"的直接体现 —— 改成别的会让蚂蚁线
    //   整体内缩一像素，而那种偏差肉眼极难判断。
    {
        const QImage mask = makeBlockMask(QSize(64, 64), QRect(10, 10, 20, 20));
        const QList<Seg> got = segmentsOf(outline::maskOutline(mask, QRect(10, 10, 20, 20)));

        QList<Seg> want;
        want.append(Seg{ 10, 10, 30, 10 });
        want.append(Seg{ 10, 30, 30, 30 });
        want.append(Seg{ 10, 10, 10, 30 });
        want.append(Seg{ 30, 10, 30, 30 });

        checkEqMsg(got.size(), 4, QStringLiteral("实心方块 → 正好 4 段边界"));
        checkMsg(sameSegSet(got, want),
                 QStringLiteral("★ 四段端点精确等于方块四条边（实测 %1）").arg(segText(got)));
    }

    // ── 8.2 包围盒提示只是优化，不许改变结果 ──
    //
    // ★ 这条断言是"提示"这个参数的安全带 ★
    //   给提示 = 只扫那个小方块；不给 = 扫整张蒙版。
    //   两者结果必须完全一致，否则"用户框一个小区域"和"框一个刚好覆盖它的大区域"
    //   会得到不同的蚂蚁线 —— 这种偏差没有任何报错，只能靠这条断言钉住。
    {
        const QImage mask = makeBlockMask(QSize(64, 64), QRect(10, 10, 20, 20));
        const QList<Seg> withHint = segmentsOf(outline::maskOutline(mask, QRect(10, 10, 20, 20)));
        const QList<Seg> noHint   = segmentsOf(outline::maskOutline(mask, QRect()));
        const QList<Seg> loose    = segmentsOf(outline::maskOutline(mask, QRect(0, 0, 64, 64)));

        checkMsg(sameSegSet(withHint, noHint),
                 QStringLiteral("★★ 传不传包围盒提示，结果完全一致（提示只省扫描量）"));
        checkMsg(sameSegSet(withHint, loose),
                 QStringLiteral("★★ 提示给松一点，结果也完全一致"));
    }

    // ── 8.2b ★ hint 给小了会凭空长出一圈假轮廓（这是契约，不是 bug）──
    //
    //   ★ 为什么值得专门钉住一个"错误用法" ★
    //     因为它是**静默**的：传小 hint 得到的不是空路径、不是报错，
    //     而是一条看起来挺合理的矩形蚂蚁线。谁为了"更快"随手估一个
    //     小 hint，就会得到一条不属于任何选区的假线，而且没有任何提示。
    //     真实调用点传的是 Selection::bounds()（按定义包住全部非零像素），
    //     所以是安全的 —— 这条断言就是那条"安全"的边界线。
    //
    //   ★ 假轮廓长在哪：不是 hint 本身，而是**扩 1 后的扫描框** ★
    //     实现里 scanRect = hint.adjusted(-1,-1,1,1)（边界线位于内外交界处，
    //     必须能看到外圈未选中的邻居）。这里 hint = QRect(15,15,10,10)，
    //     扩 1 → QRect(14,14,12,12)，即 x∈[14,25]、y∈[14,25]；扫描把框外
    //     一律当成"未选中"，于是四条边都被判成"内侧选中、外侧未选中"，
    //     在 14 与 26 两条线上各长出一段假线。
    //     ★ 所以端点必须按"扩后的框"来写 —— 按 hint 原样写就成了错的断言。
    {
        const QImage mask = makeBlockMask(QSize(64, 64), QRect(10, 10, 20, 20));
        const QList<Seg> tight = segmentsOf(outline::maskOutline(mask, QRect(15, 15, 10, 10)));

        QList<Seg> want;
        want.append(Seg{ 14, 14, 26, 14 });
        want.append(Seg{ 14, 26, 26, 26 });
        want.append(Seg{ 14, 14, 14, 26 });
        want.append(Seg{ 26, 14, 26, 26 });

        checkMsg(sameSegSet(tight, want),
                 QStringLiteral("★★ hint 给小了 → 在「扩 1 后的扫描框」边界上生出假轮廓（实测 %1）")
                     .arg(segText(tight)));
    }

    // ── 8.3 带洞（回字形）→ 8 段 ──
    {
        QImage mask(64, 64, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 5; y < 45; ++y) {
            uchar *line = mask.scanLine(y);
            for (int x = 5; x < 45; ++x)
                line[x] = 255;
        }
        for (int y = 15; y < 35; ++y) {
            uchar *line = mask.scanLine(y);
            for (int x = 15; x < 35; ++x)
                line[x] = 0;
        }
        const QList<Seg> got = segmentsOf(outline::maskOutline(mask, QRect()));
        checkEqMsg(got.size(), 8,
                   QStringLiteral("★ 回字形 → 内外各 4 段（少一段说明某条边被吃掉了）"));
        checkMsg(got.size() == 8 ? sameSegSet(got, [&] {
                      QList<Seg> w;
                      w.append(Seg{ 5, 5, 45, 5 });   w.append(Seg{ 5, 45, 45, 45 });
                      w.append(Seg{ 5, 5, 5, 45 });   w.append(Seg{ 45, 5, 45, 45 });
                      w.append(Seg{ 15, 15, 35, 15 }); w.append(Seg{ 15, 35, 35, 35 });
                      w.append(Seg{ 15, 15, 15, 35 }); w.append(Seg{ 35, 15, 35, 35 });
                      return w; }())
                          : false,
                 QStringLiteral("★ 内外八段端点都对（实测 %1）").arg(segText(got)));
    }

    // ── 8.4 空蒙版 / 全 0 → 空路径（绝不能画出"整圈画布边框"）──
    {
        QImage empty(32, 32, QImage::Format_Grayscale8);
        empty.fill(0);
        checkEqMsg(segmentsOf(outline::maskOutline(empty, QRect())).size(), 0,
                   QStringLiteral("全 0 蒙版 → 空路径（否则会在画布边缘画出一圈假蚂蚁线）"));
        checkEqMsg(segmentsOf(outline::maskOutline(QImage(), QRect())).size(), 0,
                   QStringLiteral("空 QImage → 空路径（不许崩）"));
        checkEqMsg(segmentsOf(outline::maskOutline(empty, QRect(100, 100, 0, 0))).size(), 0,
                   QStringLiteral("退化包围盒 → 空路径"));
    }

    // ── 8.5 ★ 抗锯齿/羽化边缘：边界落在"越过阈值"那一列，不是最外圈 ──
    //
    // ★ 这条是"形状工具取 alpha 而不是转灰度"（铁律 §13.1）的同族纪律 ★
    //   蒙版边缘天然存在 1~254 的过渡值。若判据写成"> 0 就算选中"，
    //   蚂蚁线会跑到羽化的最外圈（视觉上像选区胖了一圈，且随羽化半径漂移）；
    //   写成"== 255"则跑到内圈。必须落在过渡带正中（阈值 128）。
    {
        QImage grad(32, 8, QImage::Format_Grayscale8);
        grad.fill(0);
        for (int y = 0; y < 8; ++y) {
            uchar *line = grad.scanLine(y);
            for (int x = 0; x < 32; ++x)
                line[x] = uchar(x * 8);          // 0,8,...,248
        }
        // x*8 >= 128 的第一个 x 是 16 → 左边界落在 16（不是 x=1，也不是 x=31）
        QList<Seg> want;
        want.append(Seg{ 16, 0, 32, 0 });
        want.append(Seg{ 16, 8, 32, 8 });
        want.append(Seg{ 16, 0, 16, 8 });
        want.append(Seg{ 32, 0, 32, 8 });

        const QList<Seg> got = segmentsOf(outline::maskOutline(grad, QRect()));
        checkMsg(sameSegSet(got, want),
                 QStringLiteral("★★ 渐变边缘的边界落在越过 128 的 x=16，不是最外圈（实测 %1）")
                     .arg(segText(got)));
    }

    // ── 8.6 阈值是参数，且默认值就是 128 ──
    {
        const QImage soft = makeBlockMask(QSize(32, 32), QRect(8, 8, 16, 16), 100);
        checkEqMsg(segmentsOf(outline::maskOutline(soft, QRect())).size(), 0,
                   QStringLiteral("★ 默认阈值 128：值 100 判为『未选中』→ 无轮廓"));
        checkEqMsg(segmentsOf(outline::maskOutline(soft, QRect(), 64)).size(), 4,
                   QStringLiteral("阈值降到 64：同一张图判为『选中』→ 4 段"));
    }
}

// ── 8.7 实测：大蒙版下的轮廓重建成本（"先量后改"的证据）──
//
// 为什么要在这里花时间量一把：
//   评估文档 §1.3 说"有选区时每个画笔事件都要重建一次轮廓"，量级是**推断**
//   出来的（4K 全选 20~60 ms/帧），并未实测。本轮先把它量出来，
//   再决定要不要拆。测量对象是**真代码**（不是复制一份算法来量），
//   因为算法已经搬到 engine，可以直接调用。
static void testOutlineCost()
{
    beginGroup("⑧·成本 选区轮廓重建的实测开销");

    // ── 输入的选取：要**贴近真实**，不要拿一个非法输入当最坏情况 ──
    //
    //   "全 255 的蒙版 + 整张的 hint"其实不会出现在真实路径上：
    //   全选时 Selection::isActive() 为假，CanvasView 直接早退、根本不画蚂蚁线。
    //   真实的最坏情况是**一个很大的活动选区**（比如先全选、再减掉一个小角，
    //   或者拉一个覆盖绝大部分画布的大椭圆）—— 它的 bounds() 几乎等于整张画布，
    //   但蒙版里确实有 0 与非 0 的交界。下面造的就是它。
    const QSize big(4000, 3000);
    QImage bigMask(big, QImage::Format_Grayscale8);
    bigMask.fill(0);
    for (int y = 10; y < 2990; ++y) {
        uchar *line = bigMask.scanLine(y);
        for (int x = 10; x < 3990; ++x)
            line[x] = 255;
    }
    const QRect wholeHint = outline::maskOutline(bigMask, QRect()).boundingRect().toAlignedRect();

    QElapsedTimer timer;

    timer.start();
    const QPainterPath pFull = outline::maskOutline(bigMask, wholeHint);
    const qint64 msFull = qMax<qint64>(timer.elapsed(), 1);

    const QRect smallHint(1800, 1200, 200, 200);
    timer.start();
    const QPainterPath pSmall = outline::maskOutline(bigMask, smallHint);
    const qint64 msSmall = qMax<qint64>(timer.elapsed(), 1);

    std::printf("     （4000x3000 灰度蒙版，扫描整张：%lld ms / %d 段；"
                "只扫 200x200 的包围盒：%lld ms / %d 段）\n",
                static_cast<long long>(msFull), pFull.elementCount(),
                static_cast<long long>(msSmall), pSmall.elementCount());

    checkMsg(!pFull.isEmpty(),
             QStringLiteral("整张扫描仍然产出画布四条边（不是空路径）"));

    // ★ 方向性断言，不做脆弱的时间绝对值断言 ★
    //   量级差距是几百倍（12M 像素 vs 40K 像素），留 4 倍余量足够稳。
    checkMsg(msSmall * 4 < msFull,
             QStringLiteral("★★ 只扫包围盒比扫整张快 4 倍以上（%1 ms vs %2 ms）")
                 .arg(msSmall).arg(msFull));

    // ★ 这条把"为什么必须改"钉成数字 ★
    //   下一次有人问"轮廓重建到底贵不贵"，答案在这里，不在注释里。
    checkMsg(msFull > 0,
             QStringLiteral("（4K 一次整张轮廓重建实测 %1 ms；"
                            "改造前它在**每个画笔事件**里都发生一次）").arg(msFull));
}

// ===========================================================================
//  ⑨ Selection 修订号
// ===========================================================================

static void testSelectionRevision()
{
    beginGroup("⑨ Selection 修订号：只有真变了才递增");

    const QSize canvas(32, 32);

    // ── 9.1 真改内容的路径必须递增 ──
    {
        Selection s;
        const quint64 r0 = s.revision();

        s.prepareCanvas(canvas);
        const quint64 r1 = s.revision();
        checkMsg(r1 != r0, QStringLiteral("prepareCanvas 建蒙版 → 递增"));

        quint64 prev = r1;
        auto bumped = [&prev](const Selection &sel, const QString &what) {
            const bool ok = sel.revision() != prev;
            prev = sel.revision();
            checkMsg(ok, what);
        };

        s.selectRect(QRect(4, 4, 8, 8));
        bumped(s, QStringLiteral("selectRect → 递增"));

        s.selectEllipse(QRect(6, 6, 10, 10));
        bumped(s, QStringLiteral("selectEllipse → 递增"));

        s.markModified();
        bumped(s, QStringLiteral("markModified（工具直写 mutableMask 后调用）→ 递增"));

        s.translate(QPoint(1, 1));
        bumped(s, QStringLiteral("translate → 递增"));

        s.feather(2.0);
        bumped(s, QStringLiteral("feather → 递增"));

        s.invert();
        bumped(s, QStringLiteral("invert → 递增"));

        s.resizeCanvas(QSize(40, 40));
        bumped(s, QStringLiteral("resizeCanvas → 递增"));

        s.setMask(makeBlockMask(QSize(40, 40), QRect(2, 2, 6, 6)));
        bumped(s, QStringLiteral("setMask → 递增"));

        s.clear();
        bumped(s, QStringLiteral("clear → 递增"));
    }

    // ── 9.2 ★★ 提前返回的分支：一步都没改，绝不许递增 ★★ ──
    //
    //   这一组是整条优化的生命线。如果它们也递增，
    //   "修订号没变就不重建轮廓"的判据就永远为假 ——
    //   功能全对、像素断言全绿，而那个优化**完全不生效**，
    //   且没有任何外部症状（只是慢）。
    //   这类"优化静默失效"只能靠这里钉住。
    {
        Selection t;
        t.prepareCanvas(canvas);
        t.selectRect(QRect(4, 4, 8, 8));
        const quint64 a = t.revision();

        t.translate(QPoint(0, 0));            // delta 为空 → 提前返回
        checkMsg(t.revision() == a, QStringLiteral("★ translate(0,0) 不算改动（提前返回）"));

        t.feather(0.0);                       // radius <= 0 → 提前返回
        checkMsg(t.revision() == a, QStringLiteral("★ feather(0) 不算改动"));

        t.feather(-1.0);
        checkMsg(t.revision() == a, QStringLiteral("★ feather(负数) 不算改动"));

        t.applyMask(QImage(3, 3, QImage::Format_Grayscale8), Selection::Op::Add);
        checkMsg(t.revision() == a, QStringLiteral("★ 尺寸不匹配的 applyMask 不算改动"));

        t.applyPath(QPainterPath(), Selection::Op::Add);
        checkMsg(t.revision() == a, QStringLiteral("★ 空路径的 applyPath 不算改动"));
    }
    {
        Selection inactive;                   // 从未 prepareCanvas → 没有蒙版
        const quint64 b = inactive.revision();
        inactive.invert();
        checkMsg(inactive.revision() == b, QStringLiteral("★ 没有选区时 invert() 不算改动"));
        inactive.translate(QPoint(3, 3));
        checkMsg(inactive.revision() == b, QStringLiteral("★ 没有选区时 translate() 不算改动"));
        inactive.selectRect(QRect(0, 0, 4, 4));
        checkMsg(inactive.revision() == b, QStringLiteral("★ 没有蒙版时 selectRect() 不算改动"));
    }

    // ── 9.3 ★★ 全局唯一：撤销把旧快照赋回来时不许撞号 ★★ ──
    //
    //   判据"修订号相同 ⇒ 内容相同"要成立，发号器就必须全局单调递增。
    //   若每个 Selection 各数各的，下面这条就会撞上：
    //     改一次(2) → 快照(2) → 再改一次(3) → 赋回快照(2) → 又改一次(3)
    //                                             ↑ 3 已经用过了，但内容不同
    //   碰撞的后果是 CanvasView 认为"没变"，蚂蚁线停在上一版选区上。
    {
        Selection x;
        x.prepareCanvas(canvas);
        x.selectRect(QRect(1, 1, 4, 4));

        const Selection snap = x.clone();
        checkMsg(snap.revision() == x.revision(),
                 QStringLiteral("克隆代表同一份内容 → 带同一个号"));

        x.selectRect(QRect(9, 9, 4, 4));
        const quint64 after = x.revision();
        checkMsg(after != snap.revision(),
                 QStringLiteral("★★ 内容不同 → 号一定不同"));

        x = snap;                             // 模拟"撤销把快照赋回来"
        checkMsg(x.revision() == snap.revision(), QStringLiteral("赋回快照 → 号也跟着回来"));

        x.selectRect(QRect(20, 20, 4, 4));    // 内容与 after 那次**不同**
        checkMsg(x.revision() != after,
                 QStringLiteral("★★ 赋回快照后再改，新号不许撞上历史号"
                                "（每个对象各数各的计数器就会撞上）"));
    }

    // ── 9.4 号是单调递增的（不是随机数、也不是内容哈希）──
    {
        Selection s;
        s.prepareCanvas(canvas);
        quint64 last = s.revision();
        bool increasing = true;
        for (int i = 0; i < 5; ++i) {
            s.selectRect(QRect(i, i, 4, 4));
            if (s.revision() <= last)
                increasing = false;
            last = s.revision();
        }
        checkMsg(increasing, QStringLiteral("连续改动 → 号严格递增"));
    }
}

// ===========================================================================
//  ⑩ ImageCommand 的可选脏区域
// ===========================================================================

static void testImageCommandRegion()
{
    beginGroup("⑩ ImageCommand：可选脏区域");

    const QSize canvas(64, 64);
    const QRect r(8, 8, 16, 16);

    Document doc(canvas, Qt::transparent);
    Layer *layer = doc.layers().layer(0);
    if (!layer) {
        checkMsg(false, QStringLiteral("文档自带一个图层"));
        return;
    }

    const QImage before = layer->image().copy();
    QImage after = before.copy();
    {
        QPainter p(&after);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(r, QColor(255, 0, 0, 255));
        p.end();
    }

    doc.composite();                          // 让缓存有效、全量标志落下来

    // ── ① 不带区域（默认）→ 仍走整图失效：这就是向后兼容 ──
    {
        const quint64 full0 = doc.fullCompositeCount();
        CommandStack stack(&doc);
        stack.push(new ImageCommand(&doc, 0, before, after, QStringLiteral("调整")));

        checkMsg(doc.pendingDirtyRegion().isEmpty(),
                 QStringLiteral("★ 不传区域的 ImageCommand → 仍是整图失效（默认值 = 我不知道，向后兼容）"));

        doc.composite();
        checkEqMsg(int(doc.fullCompositeCount() - full0), 1,
                   QStringLiteral("不报区域 → 老老实实整图重算一次"));
    }

    // ── ② 带区域 → 被采纳 ──
    {
        layer->setImage(before);              // 复位，让下一次 redo 真的改变像素
        doc.markDirty();
        doc.composite();

        const quint64 full0 = doc.fullCompositeCount();
        const quint64 reg0  = doc.regionCompositeCount();

        CommandStack stack(&doc);
        stack.push(new ImageCommand(&doc, 0, before, after,
                                    QStringLiteral("移动图层"), r));

        // ★★ 这条同时钉住两件事 ★★
        //   ① 命令报出的区域被 CommandStack 采纳了；
        //   ② restoreImage() 里**没有**那句画蛇添足的 m_doc->markDirty() ——
        //      有它在，这里就会读到空（全量失效），下面这条立刻红。
        //      那句之所以危险，是因为它跑在 push() 内部，把刚设好的
        //      区域失效降级成全量，而画面完全正确、只是慢。
        checkMsg(doc.pendingDirtyRegion() == r,
                 QStringLiteral("★★ 命令报出的脏区域被采纳（区域：%1）")
                     .arg(rectText(doc.pendingDirtyRegion())));

        const QImage composed = doc.composite();
        checkEqMsg(int(doc.fullCompositeCount() - full0), 0,
                   QStringLiteral("★★ 带区域的压栈没有触发任何一次全图重算"));
        checkEqMsg(int(doc.regionCompositeCount() - reg0), 1,
                   QStringLiteral("这次合成走的是区域快路径"));
        checkEqMsg(qRed(composed.pixel(12, 12)), 255,
                   QStringLiteral("区域内的像素确实变成新内容"));

        // ── ③ 撤销也要保住区域 ──
        const quint64 full1 = doc.fullCompositeCount();
        stack.undo();
        checkMsg(doc.pendingDirtyRegion() == r,
                 QStringLiteral("★ 撤销同样按区域失效（undo 从栈里取命令，不许自己记指针）"));
        doc.composite();
        checkEqMsg(int(doc.fullCompositeCount() - full1), 0,
                   QStringLiteral("★★ 撤销没有触发全图重算"));
    }

    // ── ④ 合并时的"空传播"规则 ──
    //
    //   ★ 直接用 mergeWith 验，不等 QUndoStack 来触发 ★
    //     因为这条规则的错误形态是"悄悄算小了"：QRect().united(r) 会返回 r，
    //     等于把"我不知道"当成了"这一小块脏" —— 一旦被采纳，
    //     画面就残留旧内容（不崩、不报错）。
    {
        ImageCommand *a = new ImageCommand(&doc, 0, before, after,
                                           QStringLiteral("a"), QRect(0, 0, 4, 4));
        ImageCommand *b = new ImageCommand(&doc, 0, before, after,
                                           QStringLiteral("b"), QRect(10, 10, 4, 4));
        a->mergeWith(b);
        checkMsg(a->dirtyRegion() == QRect(0, 0, 4, 4).united(QRect(10, 10, 4, 4)),
                 QStringLiteral("两条都报了区域 → 合并后取并集（%1）")
                     .arg(rectText(a->dirtyRegion())));

        ImageCommand *c = new ImageCommand(&doc, 0, before, after,
                                           QStringLiteral("c"), QRect(0, 0, 4, 4));
        ImageCommand *d = new ImageCommand(&doc, 0, before, after,
                                           QStringLiteral("d"));      // 不带区域
        c->mergeWith(d);
        checkMsg(c->dirtyRegion().isEmpty(),
                 QStringLiteral("★★ 一方说『我不知道』→ 合并结果也必须『不知道』"
                                "（不能退化成并集，那是把区域算小了）"));

        ImageCommand *e = new ImageCommand(&doc, 0, before, after, QStringLiteral("e"));
        ImageCommand *f = new ImageCommand(&doc, 0, before, after,
                                           QStringLiteral("f"), QRect(3, 3, 4, 4));
        e->mergeWith(f);
        checkMsg(e->dirtyRegion().isEmpty(),
                 QStringLiteral("★★ 反过来（自己不知道、对方知道）同样要『不知道』"));

        delete a; delete b; delete c; delete d; delete e; delete f;
    }
}

// ===========================================================================
//  ⑪ 不透明上层直接覆盖（快路径）★ 2026-09-22 新增 ★
// ===========================================================================
//  见 docs/性能优化评估.md §4.5。改动只有几行，但它落在**全项目最热的内核**上，
//  而它的全部价值就是"结果与慢路径逐位相同"——写错的表现是"某些像素差 1"，
//  不崩不报、肉眼不可见。所以这一组的判据要同时钉住两件事，缺一不可：
//
//    【逐位相同】候选模式下，结果与上层图**逐位相等**（0 差异，不是 ±1）
//    【真的走了快路径】命中计数 == 符合条件的像素数
//       ★ 反过来：只验"结果对"是**证明不了快路径生效的**（慢路径也算得对）；
//         只验计数是证明不了没写错的。两条一起才有意义。
//
//  ★ 候选集不硬编码，而是**由 blendChannel 反推** ★
//    只有"上层分量原样通过"（blendChannel(mode, base, blend) == blend）的模式
//    才允许覆盖。将来若真实现"溶解"（引入随机抖动），11.1 会立刻变红，
//    提醒把 Dissolve 从 Blend.cpp 的快路径列表里一并拿掉 —— 那道雷是特意埋的。
//
//  ★ 慢路径"没被改动"这件事不靠本组兜 ★
//    本组只加了**提前返回的分支**，慢路径代码一个字节没动；半透明像素的合成
//    仍由 ① 组（区域 == 全图，逐位）与其余 21 个测试程序覆盖。
//
//  ★ 两个写测试时踩到的坑，都留在代码注释里（别删）★
//    11.1 —— "在一点上取到同一个值" ≠ "恒等映射"：只采样 (base, blend) 一个点，
//            变亮与线性光会碰巧通过，候选集虚增到 8 个；
//    11.5 —— fastPixelsOut 是**按层累加**的：两层不透明照片 → 2 × 像素数。
// ===========================================================================

// 全不透明（alpha=255）的确定性图案，模拟"照片图层"
static QImage makeOpaqueLayer(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const quint32 r = nextRand();
            line[x] = qRgba(int(r & 0xFF), int((r >> 8) & 0xFF),
                            int((r >> 16) & 0xFF), 255);
        }
    }
    return img;
}

static void testOpaqueUpperFastPath()
{
    beginGroup("⑪ ★ 不透明上层直接覆盖：逐位相同 + 快路径命中计数");

    const QSize canvas(48, 32);
    g_seed = 0xA11CEu;

    const QImage base  = makeOpaqueLayer(canvas);   // 下层照片
    const QImage upper = makeOpaqueLayer(canvas);   // 上层照片

    const int total = canvas.width() * canvas.height();

    // ── 11.1 候选集：由 blendChannel 反推（不是抄一份模式清单）──
    //
    //  ★ 判据必须是"对**任意** base 都恒等"，不能只在单个采样点上相等 ★
    //    第一版就写错了，而且错得很有教育意义：只试 (base=0.3, blend=0.7) 时，
    //      · 变亮   = max(0.3, 0.7)        = 0.7   ← 碰巧相等
    //      · 线性光 = 0.3 + 2*0.7 - 1      = 0.7   ← 碰巧相等
    //    于是候选集从 2 个虚增到 8 个，11.2 里那 6 个模式全红。
    //    "在一点上取到同一个值" ≠ "这个函数是恒等映射" —— 采样点要够多。
    //
    //  ★ 分量模式（色相/饱和度/颜色/明度）必须**单独排除** ★
    //    它们在 compositeLayer 里压根不走 blendChannel，而是去 HSL 空间取分量；
    //    blendChannel 对它们只是 default 分支退回 blendNormal，
    //    所以"blendChannel 恒等"对它们并不成立。
    const QList<BlendMode> componentModes = {
        BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity
    };
    const QList<qreal> samples = { 0.0, 0.13, 0.3, 0.5, 0.7, 0.87, 1.0 };

    QList<BlendMode> candidates;
    for (BlendMode m : allBlendModes()) {
        if (componentModes.contains(m))
            continue;
        bool identity = true;
        for (qreal v : samples) {
            for (qreal b : samples) {
                if (blend::blendChannel(m, b, v) != v) {
                    identity = false;
                    break;
                }
            }
            if (!identity)
                break;
        }
        if (identity)
            candidates.append(m);
    }
    checkEqMsg(candidates.size(), 2,
               QStringLiteral("★ 「上层分量原样通过」的模式恰好 2 个（Normal / Dissolve）"
                              "—— 若溶解真实现了随机抖动，这条会红，"
                              "必须同时把 Dissolve 从 Blend.cpp 的快路径列表里拿掉"));
    checkMsg(candidates.contains(BlendMode::Normal)
                 && candidates.contains(BlendMode::Dissolve),
             QStringLiteral("这两个就是 Normal 与 Dissolve"));

    // ── 11.2 逐模式：候选 → 全命中且逐位等于上层；非候选 → 一个像素都不许命中 ──
    //
    //  ★ 非候选那半同样重要 ★ 把门写漏（比如只认"上层 alpha==255"、不看模式），
    //  下面 18 个非候选模式会立刻因为"走了快路径"而变红。
    for (BlendMode m : allBlendModes()) {
        quint64 fp = 0;
        const QImage out = blend::compositeLayer(base, upper, m, 1.0, &fp);
        const QString tag = blendModeName(m);

        if (candidates.contains(m)) {
            checkEqMsg(int(fp), total,
                       QStringLiteral("%1：整张图 %2 个像素都走了快路径").arg(tag).arg(total));
            // ★★ 0 容差：结果必须与上层**逐位相同** ★★
            checkEqMsg(diffPixels(out, upper), 0,
                       QStringLiteral("★★ %1：结果与上层逐位相等（0 差异，不是 ±1）").arg(tag));
        } else {
            checkEqMsg(int(fp), 0,
                       QStringLiteral("★ %1 不是候选 → 一个像素都不许走快路径"
                                      "（覆盖会算错）").arg(tag));
        }
    }

    // ── 11.3 图层不透明度 < 1.0 → 门必须关上 ──
    //
    //  ★ 光验"不透明的走快路"是不够的：判据若错写成"只看模式"，
    //    11.2 照样全绿（那里 opacity 恰好是 1.0）。这条专抓"门少带一个条件"。
    for (BlendMode m : candidates) {
        quint64 fp = 0;
        const QImage out = blend::compositeLayer(base, upper, m, 0.5, &fp);
        checkEqMsg(int(fp), 0,
                   QStringLiteral("opacity=0.5 时不许走快路径（%1）").arg(blendModeName(m)));
        checkMsg(diffPixels(out, upper) > 0,
                 QStringLiteral("★★ opacity=0.5 的结果不该等于上层（%1）"
                                "—— 相等说明不透明度整条被跳过了").arg(blendModeName(m)));
    }

    // ── 11.4 ★ 判据是**逐像素**的，不是"整层否决" ★ ──
    //
    //  这张图上半不透明、下半半透明。它同时钉住两件事：
    //    · 不透明的那些像素必须走快路径，且结果 == 上层原值；
    //    · 半透明的那些必须照旧走慢路径。
    //  两种写错的判据（"整层看 alpha" / "只看 alpha 不看模式"）各会让其中一条红。
    QImage mixed(canvas, QImage::Format_ARGB32_Premultiplied);
    int opaqueCount = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        QRgb *dst = reinterpret_cast<QRgb *>(mixed.scanLine(y));
        const QRgb *src = reinterpret_cast<const QRgb *>(upper.constScanLine(y));
        for (int x = 0; x < canvas.width(); ++x) {
            if (y < canvas.height() / 2) {
                dst[x] = src[x];                       // 不透明（上层 alpha 本来就是 255）
                ++opaqueCount;
            } else {
                // 半透明：预乘分量按比例压到 alpha=128，保持颜色不变
                const int a = 128;
                dst[x] = qRgba(qRed(src[x]) * a / 255,
                               qGreen(src[x]) * a / 255,
                               qBlue(src[x]) * a / 255, a);
            }
        }
    }

    {
        quint64 fp = 0;
        const QImage out = blend::compositeLayer(base, mixed, BlendMode::Normal, 1.0, &fp);

        checkEqMsg(int(fp), opaqueCount,
                   QStringLiteral("★ 只数上半那张（%1 个像素）走快路径，"
                                  "半透明的照旧走慢路").arg(opaqueCount));

        int badOpaque = 0, badSemi = 0;
        for (int y = 0; y < canvas.height(); ++y) {
            const QRgb *o = reinterpret_cast<const QRgb *>(out.constScanLine(y));
            const QRgb *u = reinterpret_cast<const QRgb *>(upper.constScanLine(y));
            for (int x = 0; x < canvas.width(); ++x) {
                if (y < canvas.height() / 2) {
                    if (o[x] != u[x])
                        ++badOpaque;
                } else if (o[x] == u[x]) {
                    ++badSemi;      // 半透明像素原样透传 = 慢路径被跳过了
                }
            }
        }
        checkEqMsg(badOpaque, 0,
                   QStringLiteral("★★ 不透明那半逐位等于上层原值（0 差异）"));
        checkEqMsg(badSemi, 0,
                   QStringLiteral("半透明那半确实被合成过（没有原样透传）"));
    }

    // ── 11.5 ★ 经真实入口驱动 ★ ──
    //
    //  ⑪ 组到此为止都是直接调 compositeLayer()，而这正是批次一踩过的坑：
    //  底层 API 全绿，真实调用链却根本没走那条路（`BrushTool` 里还写着
    //  markDirty()）。Document 用的是 compositeLayersRegion / compositeLayers，
    //  所以必须从这两个入口再验一遍。
    {
        const QList<QImage> imgs = { base, upper };
        const QList<BlendMode> modes = { BlendMode::Normal, BlendMode::Normal };
        const QList<qreal> ops = { 1.0, 1.0 };
        const QList<bool> vis = { true, true };

        quint64 fp = 0;
        const QImage whole = blend::compositeLayersRegion(
            imgs, modes, ops, vis, canvas, QRect(QPoint(0, 0), canvas),
            Qt::transparent, &fp);
        //  ★ 计数是**逐层累加**的，不是"整张图算了几个像素" ★
        //    两层不透明照片：底层画在透明画布上 → 整张走快路；
        //    顶层再覆盖一遍 → 又整张走快路。所以是 2 × total。
        //    这不是重复计数，而是"本次调用共有多少个像素由快路径写出"。
        checkEqMsg(int(fp), total * 2,
                   QStringLiteral("★★ 经 compositeLayersRegion（Document 的实际入口）"
                                  "两层各整张命中 → %1 个像素").arg(total * 2));
        checkEqMsg(diffPixels(whole, upper), 0,
                   QStringLiteral("★★ 两层不透明照片合成 → 结果与上层逐位相同"
                                  "（= 直接覆盖）"));

        const QImage full = blend::compositeLayers(imgs, modes, ops, vis, canvas,
                                                   Qt::transparent);
        checkEqMsg(diffPixels(full, upper), 0,
                   QStringLiteral("compositeLayers（全图版）结果一致"));
    }

    // ── 11.6 区域路径：只数被重算的那一块 ──
    {
        const QRect r(7, 5, 16, 12);
        const QList<QImage> imgs = { base, upper };
        const QList<BlendMode> modes = { BlendMode::Normal, BlendMode::Normal };
        const QList<qreal> ops = { 1.0, 1.0 };
        const QList<bool> vis = { true, true };

        quint64 fp = 0;
        const QImage patch = blend::compositeLayersRegion(
            imgs, modes, ops, vis, canvas, r, Qt::transparent, &fp);
        checkEqMsg(int(fp), r.width() * r.height() * 2,
                   QStringLiteral("区域重算：只数这一块，且按层累加（%1 × 2 层）")
                       .arg(r.width() * r.height()));
        checkEqMsg(diffPixels(patch, upper.copy(r)), 0,
                   QStringLiteral("★★ 补丁与上层的对应子区域逐位相同"));
    }

    // ── 11.7 边界：上层比画布小 → 只数被盖住的像素，其余保持下层原样 ──
    {
        const QSize small(20, 10);
        const QImage smallUpper = makeOpaqueLayer(small);

        quint64 fp = 0;
        const QImage out = blend::compositeLayer(base, smallUpper,
                                                 BlendMode::Normal, 1.0, &fp);
        checkEqMsg(int(fp), small.width() * small.height(),
                   QStringLiteral("★ 只数上层实际盖住的 %1 个像素，不是整张画布")
                       .arg(small.width() * small.height()));

        int badInside = 0, badOutside = 0;
        for (int y = 0; y < canvas.height(); ++y) {
            const QRgb *o = reinterpret_cast<const QRgb *>(out.constScanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(base.constScanLine(y));
            const bool covered = (y < small.height());
            for (int x = 0; x < canvas.width(); ++x) {
                if (covered && x < small.width()) {
                    const QRgb *u = reinterpret_cast<const QRgb *>(
                        smallUpper.constScanLine(y));
                    if (o[x] != u[x])
                        ++badInside;
                } else if (o[x] != b[x]) {
                    ++badOutside;
                }
            }
        }
        checkEqMsg(badInside, 0, QStringLiteral("上层覆盖区 == 上层原值（逐位）"));
        checkEqMsg(badOutside, 0, QStringLiteral("没盖到的部分保持下层原样（逐位）"));
    }
}

// ===========================================================================

int main()
{
    std::printf("=== perf_test：脏矩形增量合成（+ 批次二：轮廓 / 修订号 / 命令区域"
                " + 不透明覆盖快路径）===\n");
    testRegionEqualsFullSubRect();
    testAccumulation();
    testFallback();
    testRingUntouched();
    testTelemetry();
    testCommandRegion();
    testCostAndCow();

    testOutlineAlgorithm();
    testOutlineCost();
    testSelectionRevision();
    testImageCommandRegion();
    testOpaqueUpperFastPath();

    std::printf("\n----------------------------------------\n");
    std::printf("perf_test: 通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
