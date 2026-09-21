#include "ui/IconLibrary.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
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

/// 把一份 SVG 渲染成 px×px 的位图；失败返回空 QPixmap。
/// dpr>1 时按 px*dpr 分配缓冲并把 DPR 记在 pixmap 上，
/// 逻辑尺寸仍是 px —— 这样 Qt 画出来的物理像素更多、更清晰。
QPixmap renderSvg(const QString &svgPath, int px, qreal dpr)
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
    renderer.render(&p);
    p.end();

    return pm;
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
    QHash<QString, QIcon> &cache = iconCache();
    const auto it = cache.constFind(name);
    if (it != cache.constEnd())
        return it.value();

    QIcon result;                                     // 默认空图标
    const QString path = pathOf(name);

    const QPixmap probe = renderSvg(path, kSizes[2], 1.0);
    if (!probe.isNull()) {
        for (int i = 0; i < kSizeCount; ++i) {
            result.addPixmap(renderSvg(path, kSizes[i], 1.0));
            result.addPixmap(renderSvg(path, kSizes[i], 2.0));
        }
    }

    // ★ 空图标也要进缓存 ★
    //   否则每次开菜单都会去磁盘（资源）上找一遍不存在的文件，
    //   而且"这个名字没有图标"这个结论每次都重算一次。
    cache.insert(name, result);
    return result;
}

QPixmap pixmap(const QString &name, int px)
{
    if (px <= 0)
        return QPixmap();

    const QString key = name + QLatin1Char('@') + QString::number(px);
    QHash<QString, QPixmap> &cache = pixmapCache();
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();

    const QPixmap pm = renderSvg(pathOf(name), px, 1.0);
    cache.insert(key, pm);
    return pm;
}

QIcon iconForTool(const QString &toolId)
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
    };

    for (const auto &e : kMap) {
        if (toolId == QLatin1String(e.id))
            return icon(QLatin1String(e.icon));
    }
    return QIcon();
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
