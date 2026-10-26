#include "ui/IconLibrary.h"

#include "compat/QtCompat.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QSvgRenderer>

namespace zl {
namespace icons {

// ---------------------------------------------------------------------------
//  内部：缓存与渲染
// ---------------------------------------------------------------------------
namespace {

// QIcon 是多档位图的容器，一次渲染好整档交给 Qt 挑。
//   · 16 / 20 / 24 = 菜单、工具栏、列表的常见尺寸
//   · 32 / 48      = 大图标模式与"适合窗口"外的放大型场景
//   每档同时给 1× 与 2× （2× 的 pixmap 带 devicePixelRatio=2），
//   高 DPI 屏上才会是清晰的，而不是把 24px 拉伸成 48px。
//
// ★ 每档要产出两种"状态" ★
//   同一条目有 正常 / 禁用 两种可用性，两者颜色不同，
//   所以每档是 2 色 × 2 倍率 = 4 张（见 icon()）。
const int kSizes[] = {16, 20, 24, 32, 48};
const int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));

QHash<QString, QIcon> &iconCache()
{
    static QHash<QString, QIcon> cache;
    return cache;
}

QHash<QString, QPixmap> &pixmapCache()
{
    static QHash<QString, QPixmap> cache;
    return cache;
}

/// 目标色：从调色板取"这个主题下正常该用的前景色"。
///
/// ★ 只取一次、由调用方传下来 ★
///   每个像素都去问一遍 palette 既慢又容易在换主题的中途取到新旧混合的值。
///
/// ★ 为什么用 WindowText 而不是 ButtonText ★
///   图标主要出现在工具栏、菜单、面板列表上，它们的底色就是窗口/按钮底，
///   与 WindowText 的对比关系最稳。ButtonText 在部分样式下被设成与
///   窗口文字不同的值（例如高亮按钮上），拿它当通用前景色会偏。
static QColor normalColorFromPalette()
{
    const QColor c = QGuiApplication::palette().color(QPalette::WindowText);
    // 调色板没给出有效色（极少见）时按主题兜一个，绝不留空
    return c.isValid() ? c : (isDarkTheme() ? QColor(0xE6, 0xE6, 0xE6)
                                           : QColor(0x33, 0x39, 0x3F));
}

/// 禁用态的目标色：同色压到低透明度。
///
/// ★ 为什么靠 alpha 而不是另取一个灰 ★
///   另取一个灰会在彩色主题（高对比、护眼黄底）上显得突兀；降 alpha
///   则永远是该主题自己的前景色、只是"退后一步"。与 Qt 自己给禁用图标
///   降透明度的做法一致。
static QColor disabledColorFrom(const QColor &normal)
{
    QColor c = normal;
    c.setAlpha(64);          // ≈25%
    return c;
}

/// 把已渲染的位图整块染成 color，返回新位图。
///
/// ★ 原理：CompositionMode_SourceIn ★
///   只在"目标已有像素"的地方上色，因此:
///     · 线描的透明区保持透明（图标不会被填成一块方块）；
///     · 抗锯齿边缘的半透明像素被同步减淡 —— 不会出现一圈生硬的边；
///     · SVG 里用来压"面"的 fill-opacity=0.16 也自然保留（它本来就是
///       alpha，SourceIn 只改色相不改 alpha）。
///
/// ★ 前提：图标必须是单色 ★
///   SourceIn 是整块刷，原始颜色会被完全丢弃。所以 resources/gen_icons.py
///   约定只用一种颜色；将来若要有意保留双色图标（例如红点警告），
///   就得把它排除在上色之外 —— icon_test 里有一条断言守着"不得出现第二种颜色"。
QPixmap tintPixmap(const QPixmap &src, const QColor &color)
{
    if (src.isNull())
        return src;

    QPixmap out = src;                       // 保持同一 DPR
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), color);
    p.end();
    return out;
}

/// 把一份 SVG 渲染成 px×px 的位图；失败返回空 QPixmap。
/// dpr>1 时按 px*dpr 分配缓冲并把 DPR 记在 pixmap 上，
/// 逻辑尺寸仍是 px —— 这样 Qt 画出来的物理像素更多、更清晰。
/// color 有效时渲染完再整块上色（见 tintPixmap）。
QPixmap renderSvg(const QString &svgPath, int px, qreal dpr, const QColor &color)
{
    QSvgRenderer renderer(svgPath);
    if (!renderer.isValid())
        return QPixmap();          // 文件缺失 / XML 写坏 —— 交给调用方降级

    const int phys = qMax(1, qRound(px * dpr));
    QPixmap pm(phys, phys);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // ★ 必须显式传 bounds（2026-10-15 实测，探针见工作日志）★
    //   renderer.render(&p) 不传 bounds 时：
    //     · dpr = 1 —— Qt5.15 / Qt6.8 都正常（SVG 铺满 painter 设备）；
    //     · dpr > 1 —— **两版本一起病态**：只把 SVG 的 defaultSize
    //       （24×24）画到左上角一小块（96×96 画布上非透明区仅 54..95），
    //       高 DPI 屏上工具栏/菜单图标正是"图片左上角一点点区域"的元凶。
    //   bounds 用**逻辑尺寸**（= 物理像素 / DPR）：painter 在带 DPR 的
    //   pixmap 上绘制时逻辑坐标会自动乘 DPR，传 px×px 即铺满整张图。
    //   传了之后 Qt5/Qt6、任意 DPR 行为完全一致。
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();

    if (!color.isValid())
        return pm;
    return tintPixmap(pm, color);      // ★ 交出的是成品色，调用方不必再染
}

} // namespace

// ---------------------------------------------------------------------------
//  公开接口
// ---------------------------------------------------------------------------
QString pathOf(const QString &name)
{
    return QStringLiteral(":/icons/") + name + QStringLiteral(".svg");
}

bool exists(const QString &name)
{
    return QFileInfo::exists(pathOf(name));
}

QIcon icon(const QString &name)
{
    // ★ 颜色必须进缓存键 ★
    //   同一张 SVG 在两种主题下要出两种颜色。若只按名字缓存，先建的那份
    //   会被后来的窗口/主题直接复用（典型症状：深色主题一切都好，一切到
    //   浅色，已经建过的菜单图标仍是白的）。
    const QColor normal = normalColorFromPalette();
    const QString key = name + QLatin1Char('@') + normal.name(QColor::HexArgb);

    QHash<QString, QIcon> &cache = iconCache();
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();

    QIcon result;                                     // 默认空图标
    const QString path = pathOf(name);

    const QPixmap probe = renderSvg(path, kSizes[2], 1.0, normal);
    if (!probe.isNull()) {
        // 禁用态另起一档颜色。★ 不能只靠 Qt 自动降透明度 ★
        //   我们已经是把整块位图染成前景色，Qt 的"自动淡化"作用在
        //   一张同色位图上，在浅色主题下几乎看不出差别（灰对灰）。
        const QColor disabled = disabledColorFrom(normal);
        for (int i = 0; i < kSizeCount; ++i) {
            result.addPixmap(renderSvg(path, kSizes[i], 1.0, normal), QIcon::Normal, QIcon::Off);
            result.addPixmap(renderSvg(path, kSizes[i], 2.0, normal), QIcon::Normal, QIcon::Off);
            result.addPixmap(renderSvg(path, kSizes[i], 1.0, disabled), QIcon::Disabled, QIcon::Off);
            result.addPixmap(renderSvg(path, kSizes[i], 2.0, disabled), QIcon::Disabled, QIcon::Off);
        }
    }

    // ★ 空图标也要进缓存 ★
    //   否则每次开菜单都会去磁盘（资源）上找一遍不存在的文件，
    //   而且"这个名字没有图标"这个结论每次都重算一次。
    cache.insert(key, result);
    return result;
}

QPixmap pixmap(const QString &name, int px, const QColor &color)
{
    if (px <= 0)
        return QPixmap();

    const QColor c = color.isValid() ? color : normalColorFromPalette();
    const QString key = name + QLatin1Char('@') + QString::number(px)
                        + QLatin1Char('@') + c.name(QColor::HexArgb);
    QHash<QString, QPixmap> &cache = pixmapCache();
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();

    const QPixmap pm = renderSvg(pathOf(name), px, 1.0, c);
    cache.insert(key, pm);
    return pm;
}

QColor currentColor()
{
    return normalColorFromPalette();
}

QString iconNameForTool(const QString &toolId)
{
    // ★ 工具的 id 一旦确定就不能改 ★（它同时是 QSettings 的分组名）
    //   所以这张表按 id 写、不按显示名写。
    //   后半段是 Phase 6 才接入的工具 —— 图标先画好，接工具时不必回头补图。
    static const struct { const char *id; const char *icon; } kMap[] = {
        {"move",           "tool_move"},
        {"brush",          "tool_brush"},
        {"eraser",         "tool_eraser"},
        {"rectselect",     "tool_rectselect"},
        {"ellipseselect",  "tool_ellipseselect"},
        {"crop",           "tool_crop"},
        // ── 以下是 Phase 6「绘制工具补全」要接的，图标已就位 ──
        {"bucket",         "tool_bucket"},
        {"eyedropper",     "tool_eyedropper"},
        {"line",           "tool_line"},
        {"rect",           "tool_rect"},
        {"ellipse",        "tool_ellipse"},
        {"text",           "tool_text"},
        {"hand",           "tool_hand"},
        {"zoom",           "tool_zoom"},
        // ── 以下是"魔术棒"接入时补的（第 15 个工具）──
        //   图标在 Phase 6 之前就先画好了（见 gen_icons.py 的图标表），
        //   所以这里只是把 id 与那张图接上。
        {"magicwand",      "tool_magicwand"},
    };

    for (const auto &e : kMap) {
        if (toolId == QLatin1String(e.id))
            return QLatin1String(e.icon);
    }
    return QString();
}

QIcon iconForTool(const QString &toolId)
{
    const QString name = iconNameForTool(toolId);
    return name.isEmpty() ? QIcon() : icon(name);
}

QStringList allNames()
{
    QDir dir(QStringLiteral(":/icons"));
    QStringList names = dir.entryList(QStringList() << QStringLiteral("*.svg"),
                                      QDir::Files);
    for (QString &n : names)
        n.chop(4);                        // 去掉 ".svg"
    names.sort();
    return names;
}

int cachedCount()
{
    return iconCache().size();
}

void clearCache()
{
    iconCache().clear();
    pixmapCache().clear();
}

} // namespace icons
} // namespace zl
