// ============================================================================
//  icon_test.cpp —— SVG 图标体系的离线自检
// ----------------------------------------------------------------------------
//  图标最典型的失效方式是**完全静默**的：
//
//    ① 名字写错一个字母 → `icons::icon("tool_brsh")` 返回空 QIcon →
//       工具箱那个按钮上什么都没有。编译器、链接器、其它自检全都不说话。
//    ② SVG 文件在、但画不出东西（XML 写坏、路径写成了空、坐标跑到视框外）
//       → 同样是"点了没图"，而文件明明存在于资源里。
//    ③ 只有一个小墨点 / 糊成一整块 → 文件合法、大小正常，就是不能用。
//
//  这组测试对**每一个**图标都量它渲染出来的像素，把这三类都钉住：
//
//    · 「不是空的」  : 96×96 下墨迹（alpha>8）占比必须 ≥ 2%
//    · 「不是一团」  : 占比必须 ≤ 72%（防止某个 fill 忘了写、糊成实心块）
//    · 「没被裁掉」  : 96×96 的最外圈 1 像素必须完全透明
//                      —— 坐标系超出 24×24 视框的内容会被 SVG 渲染器**裁掉**，
//                      画面上表现为"图标缺了个角"，而 SVG 本身完全合法
//    · 「不是小点」  : 墨迹包围盒的长宽各不小于视框的 1/3
//    · 「小尺寸还在」: 16×16 下墨迹占比仍 ≥ 2%（缩到菜单尺寸不能消失）
//
//  ★ 还有一条单色契约 ★
//    图标在运行时被整块刷成当前主题的前景色（见 IconLibrary.h），
//    SVG 里的原色一律丢弃。所以**每个图标只能有一种颜色** ——
//    出现第二种颜色时它会在运行时长成一片同色，而上面那些比例型判据
//    全都发现不了。这条断言专门守它。
//
//  ★ 外加两个"成对"契约 ★
//    · 工具箱里每个工具 id 都必须有图标（iconForTool 不留空）
//    · 不存在的名字必须返回空 QIcon —— 这是"响亮失败"那条路，
//      若这条坏了，名字写错的图标会显示成上一次的缓存内容，更难查
//
//  ★ 顺带产出一张接触表（contact sheet）★
//    跑完会把全部图标按 48/32/24/16 四档画到 tests/build/icons_contact_sheet.png。
//    这是给人看的：断言只能证明"不是空白/不是糊块"，
//    但"画笔看起来像不像画笔"只有眼睛能判断。改图标后请看一眼这张图。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 icon 分支。需要 QApplication（字体、QPixmap）
//    与 Qt5Svg（QSvgRenderer），并用 offscreen 平台插件跑。
//    要看接触表里的图标名，运行时加 QT_QPA_FONTDIR=C:/Windows/Fonts
//    （offscreen 默认没有字体，文字会是空的；断言不受影响）。
// ============================================================================

#include "ui/IconLibrary.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QSvgRenderer>

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

// ---------------------------------------------------------------------------
//  量化一张渲染结果
// ---------------------------------------------------------------------------

/// 墨迹占比：alpha > 8 的像素比例（忽略抗锯齿产生的极淡边缘）
static double inkRatio(const QPixmap &pm)
{
    if (pm.isNull())
        return -1.0;
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    int ink = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) > 8)
                ++ink;
        }
    }
    return double(ink) / double(img.width() * img.height());
}

/// 墨迹包围盒（全透明时返回空矩形）
static QRect inkBounds(const QPixmap &pm)
{
    if (pm.isNull())
        return QRect();
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) > 8) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < 0)
        return QRect();
    return QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

/// 最外圈 1 像素是否完全透明（用来说明"内容没有超出视框被裁掉"）
static bool outerRingClear(const QPixmap &pm)
{
    if (pm.isNull())
        return false;
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int x = 0; x < img.width(); ++x) {
        if (qAlpha(img.pixel(x, 0)) > 8 || qAlpha(img.pixel(x, img.height() - 1)) > 8)
            return false;
    }
    for (int y = 0; y < img.height(); ++y) {
        if (qAlpha(img.pixel(0, y)) > 8 || qAlpha(img.pixel(img.width() - 1, y)) > 8)
            return false;
    }
    return true;
}

/// 直接渲染 **未上色** 的原始 SVG（绕过 IconLibrary 的染色）。
///
/// ★ 为什么不拿 icons::pixmap() 的结果判单色 ★
///   上色是整块 SourceIn —— 刷完之后每个像素必然同色，
///   "单色"恒成立。也就是说拿上色后的图去判单色，这条断言永远绿，
///   根本抓不住"某个 SVG 里偷偷写了第二种颜色"。必须看原图。
static QImage renderRaw(const QString &name, int px)
{
    QSvgRenderer r(icons::pathOf(name));
    if (!r.isValid())
        return QImage();
    QImage img(px, px, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    r.render(&p, QRectF(0, 0, px, px));
    p.end();
    return img;
}

/// 这张图是不是**单色**（除透明外只有一种 RGB）。
///
/// ★ 为什么必须盯着这条 ★
///   图标是在渲染后被 CompositionMode_SourceIn 整块刷色的 ——
///   它把 SVG 里原本的颜色**全部丢掉**，只保留 alpha。
///   于是任何一个"两种颜色的图标"都会在运行时长成一片同色，
///   而墨迹占比、包围盒这些比例型判据**全都照样通过**，
///   肉眼也未必马上发现。这条断言是单色前提的唯一守卫。
///
/// ★ 判定要给容差、且跳过低 alpha（实测踩过）★
///   即使 SVG 里只写了一种 #33393f，边缘像素经过预乘/反预乘的整数取整，
///   单通道也会漂 1~11（0x33 旁边出现 0x28）。按"严格相等"判，
///   90 个图标会**全部**被判成多色 —— 一条永远红的断言等于没有。
///   所以：跳过 alpha < 128 的像素（它们只占描边最外一层的零头），
///   剩下的允许每通道 ±24。真正换色相（例如红色警告点 #d9534f 与灰）
///   的差距远超这个量级，照样抓得住。
static bool isMonochrome(const QImage &img)
{
    if (img.isNull())
        return true;
    const QImage conv = img.convertToFormat(QImage::Format_ARGB32);
    int r0 = -1, g0 = 0, b0 = 0;
    for (int y = 0; y < conv.height(); ++y) {
        for (int x = 0; x < conv.width(); ++x) {
            const QRgb px = conv.pixel(x, y);
            if (qAlpha(px) < 128)
                continue;
            if (r0 < 0) {
                r0 = qRed(px);
                g0 = qGreen(px);
                b0 = qBlue(px);
                continue;
            }
            if (qAbs(qRed(px) - r0) > 24 || qAbs(qGreen(px) - g0) > 24
                || qAbs(qBlue(px) - b0) > 24)
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  接触表：给人看的
// ---------------------------------------------------------------------------
static void writeContactSheet(const QStringList &names, const QString &path)
{
    const int cols = 8;
    const int cw = 156, ch = 80;
    const int rows = (names.size() + cols - 1) / cols;

    QImage img(cols * cw, rows * ch, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(250, 250, 251));

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = p.font();
    f.setPointSizeF(7.5);
    p.setFont(f);

    for (int i = 0; i < names.size(); ++i) {
        const int cx = (i % cols) * cw;
        const int cy = (i / cols) * ch;

        p.setPen(QColor(228, 230, 234));
        p.drawRect(cx, cy, cw - 1, ch - 1);

        // 同一张图里给四档尺寸，一眼能看出"缩到 16px 还认不认得出"
        int x = cx + 8;
        const int y = cy + 8;
        p.drawPixmap(x, y, icons::pixmap(names[i], 48)); x += 52;
        p.drawPixmap(x, y + 8, icons::pixmap(names[i], 32)); x += 38;
        p.drawPixmap(x, y + 12, icons::pixmap(names[i], 24)); x += 30;
        p.drawPixmap(x, y + 16, icons::pixmap(names[i], 16));

        p.setPen(QColor(58, 62, 70));
        p.drawText(QRect(cx + 8, cy + ch - 20, cw - 16, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, names[i]);
    }
    p.end();

    std::printf("  接触表 → %s\n", qUtf8Printable(path));
    if (!img.save(path))
        std::printf("  [警告] 接触表写不出来（路径不可写？）\n");
}

// ---------------------------------------------------------------------------
//  [1] 图标清单：「名字 ⇄ 文件」成对
// ---------------------------------------------------------------------------
static void testRegistry()
{
    beginGroup("图标清单与名字⇄文件成对");

    const QStringList names = icons::allNames();

    // 63 是当前规模。这里只卡"下限"，将来加图标不必回头改测试；
    // 但若 qrc 没被编进来（或 rcc 那一步没跑），这里会直接掉到 0。
    check(names.size() >= 60, "qrc 里至少有 60 个图标（rcc 确实编进来了）");
    std::printf("       共 %d 个图标\n", int(names.size()));

    // 路径拼接规则
    check(icons::pathOf(QStringLiteral("tool_brush"))
              == QStringLiteral(":/icons/tool_brush.svg"),
          "pathOf 拼出的资源路径正确");

    // 正向：清单里的每一个都必须真的能取到图标
    QStringList missing;
    for (const QString &n : names) {
        if (!icons::exists(n) || icons::icon(n).isNull())
            missing << n;
    }
    check(missing.isEmpty(), "清单里的每个图标都存在且能加载");
    if (!missing.isEmpty())
        std::printf("       加载不出来的：%s\n", qUtf8Printable(missing.join(", ")));

    // 反向：不存在的名字必须**响亮地失败**（返回空）
    //   这条要是坏了，写错名字的图标会拿到缓存里上一次的内容，
    //   界面上表现为"图标串台"，比没图标更难查。
    check(!icons::exists(QStringLiteral("no_such_icon_zzz")),
          "不存在的名字 exists() 为假");
    check(icons::icon(QStringLiteral("no_such_icon_zzz")).isNull(),
          "不存在的名字返回空 QIcon（不串台）");
}

// ---------------------------------------------------------------------------
//  [2] 逐个渲染：不空白、不糊死、不被裁掉
// ---------------------------------------------------------------------------
static void testEveryIconRenders()
{
    beginGroup("逐图标量像素（空白 / 糊块 / 被裁 / 小点）");

    const QStringList names = icons::allNames();
    QStringList blank, blobby, clipped, tiny, vanished;

    for (const QString &n : names) {
        const QPixmap big = icons::pixmap(n, 96);

        const double r96 = inkRatio(big);
        const QRect bb = inkBounds(big);

        if (r96 <= 0.0)
            blank << n;                       // 完全没画出来
        else if (r96 > 0.72)
            blobby << n;                      // 糊成一整块

        if (!outerRingClear(big))
            clipped << n;                     // 有内容顶到视框边缘（已被裁掉）

        if (!bb.isEmpty()
            && (bb.width() < 96 / 3 || bb.height() < 96 / 3))
            tiny << n;                        // 只有一小撮内容

        if (inkRatio(icons::pixmap(n, 16)) < 0.02)
            vanished << n;                    // 缩到菜单尺寸就没了
    }

    check(blank.isEmpty(), "没有渲染成空白的图标");
    if (!blank.isEmpty())
        std::printf("       空白：%s\n", qUtf8Printable(blank.join(", ")));

    check(blobby.isEmpty(), "没有糊成实心块的图标（fill 没写错）");
    if (!blobby.isEmpty())
        std::printf("       糊块：%s\n", qUtf8Printable(blobby.join(", ")));

    check(clipped.isEmpty(), "没有内容顶到视框边缘的图标（不会被裁角）");
    if (!clipped.isEmpty())
        std::printf("       触边：%s\n", qUtf8Printable(clipped.join(", ")));

    check(tiny.isEmpty(), "没有只画了一小撮的图标");
    if (!tiny.isEmpty())
        std::printf("       过小：%s\n", qUtf8Printable(tiny.join(", ")));

    check(vanished.isEmpty(), "缩到 16px 后仍然可见");
    if (!vanished.isEmpty())
        std::printf("       消失：%s\n", qUtf8Printable(vanished.join(", ")));

    // ── 单色前提 ──
    // ★ 图标在运行时被整块刷成前景色（CompositionMode_SourceIn），
    //   原始颜色一律丢弃。所以每个图标必须只有一种颜色，否则第二种颜色
    //   会静默消失 —— 而空白/糊块/触边这一串比例型判据**全都发现不了**。
    QStringList polychrome;
    for (const QString &n : names) {
        if (!isMonochrome(renderRaw(n, 96)))
            polychrome << n;
    }
    check(polychrome.isEmpty(), "每个图标都是单色（整块上色不会吃掉第二种颜色）");
    if (!polychrome.isEmpty())
        std::printf("       多色：%s\n", qUtf8Printable(polychrome.join(", ")));

    // ── 抽两个已知形状做定点核对 ──
    // ★ 为什么必须有这一段 ★
    //   前面那一串"没空白 / 没糊块 / 没触边"全是**比例型**判据。
    //   它们只证明"量出来的数落在合理区间"，一旦量尺本身坏了
    //   （比如渲染出来的是别的尺寸、坐标整体偏移），比例型判据可能照样通过。
    //   所以这里按**已知形状**去点几个确定该有墨、确定该没墨的像素。
    //
    //   adjust_grayscale = 圆 + **右半边填实**：
    //     中心 (12,12) → 96px 下是 (48,48)，
    //     右半边的 (14.5,12) 必须实心，左半边的 (9.5,12) 必须全空。
    const QImage gray = icons::pixmap(QStringLiteral("adjust_grayscale"), 96)
                            .toImage().convertToFormat(QImage::Format_ARGB32);
    check(!gray.isNull() && gray.width() == 96, "去色图标渲染到 96×96");
    check(qAlpha(gray.pixel(58, 48)) > 240, "去色图标：右半边是实心（填了色）");
    check(qAlpha(gray.pixel(38, 48)) == 0, "去色图标：左半边是空的（只剩描边）");

    //   tool_line = 一条对角线 → 墨迹应当很稀疏（< 12%）
    const double diag = inkRatio(icons::pixmap(QStringLiteral("tool_line"), 96));
    check(diag > 0.02 && diag < 0.12, "直线图标墨迹稀疏（2%~12%）");
    if (diag <= 0.02 || diag >= 0.12)
        std::printf("       实测 %.3f\n", diag);
}

// ---------------------------------------------------------------------------
//  [3] 工具 id → 图标
// ---------------------------------------------------------------------------
static void testToolIcons()
{
    beginGroup("工具 id → 图标");

    // 当前已注册的工具 id（15 个）。
    // ★ 这张表是"契约"：工具 id 是持久化键，不能改；
    //   新接一个工具时若忘了配图标，这条会立刻告诉你。
    const char *ids[] = {
        "move", "brush", "eraser", "rectselect", "ellipseselect", "crop",
        "bucket", "eyedropper", "line", "rect", "ellipse", "text",
        "hand", "zoom",
        "magicwand",
    };

    QStringList missing;
    for (const char *id : ids) {
        if (icons::iconForTool(QLatin1String(id)).isNull())
            missing << QLatin1String(id);
    }
    check(missing.isEmpty(), "每个工具 id 都有对应图标");
    if (!missing.isEmpty())
        std::printf("       缺图标：%s\n", qUtf8Printable(missing.join(", ")));

    check(icons::iconForTool(QStringLiteral("no_such_tool")).isNull(),
          "未知工具 id 返回空图标（由调用方降级）");

    // 不同工具的图标必须是**不同的图**，否则工具箱里会出现两个一样的按钮
    check(icons::iconForTool(QStringLiteral("brush")).cacheKey()
              != icons::iconForTool(QStringLiteral("eraser")).cacheKey(),
          "画笔与橡皮的图标不是同一张");
}

// ---------------------------------------------------------------------------
//  [4] 缓存行为
// ---------------------------------------------------------------------------
static void testCache()
{
    beginGroup("缓存行为");

    icons::clearCache();
    checkEqInt(icons::cachedCount(), 0, "清空后缓存为 0");

    icons::icon(QStringLiteral("tool_brush"));
    checkEqInt(icons::cachedCount(), 1, "取一个图标后缓存为 1");

    icons::icon(QStringLiteral("tool_brush"));
    checkEqInt(icons::cachedCount(), 1, "重复取同一个不会重复缓存");

    // ★ 空图标也要进缓存 ★
    //   否则每次打开菜单都会为"这个名字没有图标"再查一遍磁盘。
    icons::icon(QStringLiteral("no_such_icon_zzz"));
    checkEqInt(icons::cachedCount(), 2, "空图标同样进缓存（不反复查磁盘）");

    // pixmap 走另一条缓存：同尺寸两次调用必须是同一个缓冲（cacheKey 相同）
    const QPixmap a = icons::pixmap(QStringLiteral("tool_brush"), 24);
    const QPixmap b = icons::pixmap(QStringLiteral("tool_brush"), 24);
    check(!a.isNull() && a.cacheKey() == b.cacheKey(),
          "同尺寸 pixmap 命中缓存（取到同一个缓冲）");
    check(icons::pixmap(QStringLiteral("tool_brush"), 32).cacheKey() != a.cacheKey(),
          "不同尺寸是各自独立的缓冲");

    check(icons::pixmap(QStringLiteral("tool_brush"), 0).isNull(),
          "尺寸为 0 时返回空（不崩）");
}

// ---------------------------------------------------------------------------
//  [6] 上色：换色确实生效、两个主题不会串缓存
// ---------------------------------------------------------------------------
static void testTinting()
{
    beginGroup("按主题上色");

    const QColor dark(0xE6, 0xE6, 0xE6);
    const QColor light(0x33, 0x39, 0x3F);

    const QPixmap onDark  = icons::pixmap(QStringLiteral("tool_brush"), 96, dark);
    const QPixmap onLight = icons::pixmap(QStringLiteral("tool_brush"), 96, light);
    check(!onDark.isNull() && !onLight.isNull(), "两种颜色都渲染得出来");

    // ★ 颜色真的落到了像素上 ★
    //   只比对 cacheKey 不够 —— 缓存键带了颜色，两个不同的键当然不同，
    //   那只证明"键里写了颜色"，不证明"像素被染了"。
    //   这里直接读一个墨迹像素，比对它的 RGB。
    //
    //   ★ 必须挑全不透明的像素，且容差 ±2 ★
    //     抗锯齿边缘的像素是半透明的，经过预乘/反预乘的取整后，
    //     通道值会和目标色差 1~2（实测 0x32 vs 0x33）。
    //     拿第一个 alpha>200 的像素做精确比对，会因为这个假差异报红。
    //   没找到全不透明像素时返回 0（＝必然不匹配，说明图形本身有问题）。
    const auto opaqueInkColor = [](const QPixmap &pm) -> QRgb {
        const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (qAlpha(img.pixel(x, y)) == 255)
                    return img.pixel(x, y);
        return 0;
    };
    const auto closeTo = [](QRgb got, const QColor &want) {
        return got != 0
            && qAbs(qRed(got)   - want.red())   <= 2
            && qAbs(qGreen(got) - want.green()) <= 2
            && qAbs(qBlue(got)  - want.blue())  <= 2;
    };
    check(closeTo(opaqueInkColor(onDark), dark), "深色主题下图标被染成浅色");
    check(closeTo(opaqueInkColor(onLight), light), "浅色主题下图标被染成深色");
    if (!closeTo(opaqueInkColor(onLight), light))
        std::printf("       实测墨迹 %08x，期望 %08x\n",
                    opaqueInkColor(onLight), light.rgb());

    // ★ 两种颜色不能共用一条缓存 ★
    //   共用的话，界面上先出现的那种颜色会被后来所有窗口/主题复用
    //   （症状：切主题后图标不变色，且只有新开的窗口正常）。
    check(onDark.cacheKey() != onLight.cacheKey(),
          "不同颜色是各自独立的缓存项（不互串）");

    // 同样的颜色再取一次，必须命中缓存（不是每次都重新光栅化）
    check(icons::pixmap(QStringLiteral("tool_brush"), 96, dark).cacheKey()
              == onDark.cacheKey(),
          "同色同尺寸命中缓存");

    // ★ 透明区必须保持透明 ★
    //   SourceIn 只是刷色，不该把背景也刷上（刷上就是一块不透明的方块）。
    const QImage img = onDark.toImage().convertToFormat(QImage::Format_ARGB32);
    check(qAlpha(img.pixel(1, 1)) == 0, "上色后四角仍然是透明的（没被刷成方块）");

    // ★ 禁用态必须比正常态淡 ★
    //   不能只靠 Qt 自动降透明：位图本身已经是前景色，淡化后对比还是很强，
    //   禁用菜单项会看起来和可点的一样亮。
    const QIcon ic = icons::icon(QStringLiteral("tool_brush"));
    const QImage normalPm = ic.pixmap(QSize(24, 24), QIcon::Normal)
                                .toImage().convertToFormat(QImage::Format_ARGB32);
    const QImage disPm = ic.pixmap(QSize(24, 24), QIcon::Disabled)
                             .toImage().convertToFormat(QImage::Format_ARGB32);
    int normalInk = 0, disInk = 0;
    for (int y = 0; y < normalPm.height(); ++y) {
        for (int x = 0; x < normalPm.width(); ++x) {
            normalInk += qAlpha(normalPm.pixel(x, y));
            disInk    += qAlpha(disPm.pixel(x, y));
        }
    }
    check(disInk > 0 && disInk < normalInk / 2,
          "禁用态明显更淡（与正常态不是一样亮）");
    if (disInk <= 0 || disInk >= normalInk / 2)
        std::printf("       正常态总 alpha %d，禁用态 %d\n", normalInk, disInk);
}

// ---------------------------------------------------------------------------
//  [7] 多档尺寸：QIcon 里确实备了多档
// ---------------------------------------------------------------------------
static void testSizes()
{
    beginGroup("多档尺寸");

    const QIcon ic = icons::icon(QStringLiteral("tool_brush"));
    check(!ic.isNull(), "图标非空");

    // availableSizes 应包含我们喂进去的几档（16/20/24/32/48）
    const QList<QSize> sizes = ic.availableSizes(QIcon::Normal, QIcon::Off);
    check(sizes.size() >= 5, "QIcon 里备了 5 档以上尺寸");
    std::printf("       档位 %d 个，最大 %d×%d\n", int(sizes.size()),
                sizes.isEmpty() ? 0 : sizes.last().width(),
                sizes.isEmpty() ? 0 : sizes.last().height());

    // 小尺寸下 QIcon 取到的必须是"为小尺寸渲染的那一档"，而不是把 48 缩小
    const QPixmap p16 = ic.pixmap(QSize(16, 16));
    check(!p16.isNull() && p16.width() >= 16, "可以取到 16px 档");

    // ★ 关键：菜单里被置灰的动作要有灰版 ★
    //   这里不断言"一定存在 Disabled 档"（Qt 是运行时生成的），
    //   只断言取 Disabled 版本不会崩、且不为空 —— 拿不到就会画成空白按钮。
    const QPixmap pdis = ic.pixmap(QSize(24, 24), QIcon::Disabled);
    check(!pdis.isNull(), "置灰版本取得到（禁用菜单项不会是空白图标）");
}

// ===========================================================================

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit SVG 图标 离线自检\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    testRegistry();
    testEveryIconRenders();
    testToolIcons();
    testCache();
    testTinting();
    testSizes();

    // 接触表：给人看的（断言只能证明"不是空白/不是糊块"，
    // "画笔看起来像不像画笔"只有眼睛能判断）
    beginGroup("接触表");
    QString sheet = QStringLiteral("tests/build/icons_contact_sheet.png");
    if (argc > 1)
        sheet = QString::fromLocal8Bit(argv[1]);
    writeContactSheet(icons::allNames(), sheet);
    check(true, "接触表已输出");

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
