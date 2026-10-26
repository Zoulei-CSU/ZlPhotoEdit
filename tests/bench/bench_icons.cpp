// ============================================================================
//  bench_icons.cpp —— 新旧 IconLibrary 的开销对照
// ----------------------------------------------------------------------------
//  三个场景，都是真实路径：
//    ① 冷启动：首次构建全部图标（旧 = 纯渲染；新 = 渲染 + 上色 + 4 倍位图数）
//    ② 热命中：菜单反复弹出，每次都走 icon() 的缓存查找
//    ③ 换主题：clearCache 之后全量重建（新方案才有这条路径）
//
//  ★ 用 CPU 时间（clock()）而不是墙钟 ★
//    offscreen 下两者接近，但 CPU 时间不受其它进程抢占影响，抖动更小。
// ============================================================================

#include "IconLibrary_old.h"
#include "ui/IconLibrary.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStringList>

#include <cstdio>
#include <ctime>

static QStringList g_names;

/// 跑 n 轮，返回每轮平均微秒数
template <typename F>
static double bench(int n, F f)
{
    const clock_t t0 = clock();
    for (int i = 0; i < n; ++i)
        f();
    const clock_t t1 = clock();
    return double(t1 - t0) * 1e6 / CLOCKS_PER_SEC / n;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // 浅色主题（用户问的正是这种绝大多数场景）
    QPalette pal = app.palette();
    pal.setColor(QPalette::WindowText, QColor(0x33, 0x39, 0x3F));
    app.setPalette(pal);

    for (const QString &n : zl::icons::allNames())
        g_names << n;
    std::printf("图标数 %d\n\n", int(g_names.size()));

    // ── ① 冷构建：每个图标单独计时后取总/均 ──
    const double oldCold = bench(1, [] {
        zl::icons_old::clearCache();
        for (const QString &n : g_names)
            (void)zl::icons_old::icon(n);
    });
    const double newCold = bench(1, [] {
        zl::icons::clearCache();
        for (const QString &n : g_names)
            (void)zl::icons::icon(n);
    });
    std::printf("① 冷构建全部 %d 个图标\n", int(g_names.size()));
    std::printf("   旧 %.1f ms      新 %.1f ms      x%.2f\n",
                oldCold / 1000.0, newCold / 1000.0, newCold / oldCold);

    // ── ② 热命中：菜单弹出时对每个可见动作调一次 icon() ──
    //    先各自把缓存填满
    for (const QString &n : g_names) {
        (void)zl::icons_old::icon(n);
        (void)zl::icons::icon(n);
    }
    const int hitReps = 200;
    const double oldHit = bench(hitReps, [] {
        for (const QString &n : g_names)
            (void)zl::icons_old::icon(n);
    });
    const double newHit = bench(hitReps, [] {
        for (const QString &n : g_names)
            (void)zl::icons::icon(n);
    });
    std::printf("\n② 热命中：一轮 %d 个图标（菜单全部展开）\n", int(g_names.size()));
    std::printf("   旧 %.1f us/轮   新 %.1f us/轮   x%.2f\n",
                oldHit, newHit, newHit / oldHit);

    // ── ③ 换主题：清缓存 + 全量重建 ──
    const double oldSwitch = bench(3, [] {
        zl::icons_old::clearCache();
        for (const QString &n : g_names)
            (void)zl::icons_old::icon(n);
    });
    const double newSwitch = bench(3, [] {
        zl::icons::clearCache();
        for (const QString &n : g_names)
            (void)zl::icons::icon(n);
    });
    std::printf("\n③ 一次主题切换（清缓存 + 全量重建）\n");
    std::printf("   旧 %.1f ms      新 %.1f ms      x%.2f\n",
                oldSwitch / 1000.0, newSwitch / 1000.0, newSwitch / oldSwitch);

    // ── ④ 单次上色的纯开销：一张 48px 位图刷一次 ──
    QPixmap sample(48, 48);
    sample.fill(Qt::transparent);
    {
        QPainter p(&sample);
        p.fillRect(8, 8, 32, 32, QColor(0x33, 0x39, 0x3F));
    }
    const double tint = bench(2000, [&sample] {
        QPixmap out = sample;
        QPainter p(&out);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(out.rect(), QColor(0x33, 0x39, 0x3F));
    });
    // ── ③b 每帧路径：pixmap() 热命中（图层面板每帧画眼睛/锁）──
    for (const QString &n : g_names)
        (void)zl::icons::pixmap(n, 16);
    const int frameReps = 500;
    const double oldPm = bench(frameReps, [] {
        for (const QString &n : g_names)
            (void)zl::icons_old::pixmap(n, 16);
    });
    const double newPm = bench(frameReps, [] {
        for (const QString &n : g_names)
            (void)zl::icons::pixmap(n, 16);
    });
    std::printf("\n③b 每帧路径：pixmap() 热命中，一轮 %d 次\n", int(g_names.size()));
    std::printf("   旧 %.1f us/轮   新 %.1f us/轮   x%.2f\n",
                oldPm, newPm, newPm / oldPm);

    // ── ⑤ 开销拆解：key 拼装各步的单价 ──
    const QColor c(0x33, 0x39, 0x3F);
    const QString nm = QStringLiteral("tool_brush");
    const double tPalette = bench(2000, [] {
        (void)QGuiApplication::palette().color(QPalette::WindowText);
    });
    const double tName = bench(2000, [&c] {
        volatile int n = c.name(QColor::HexArgb).size();
        (void)n;
    });
    const double tKey = bench(2000, [&nm, &c] {
        volatile int n = (nm + QLatin1Char('@') + c.name(QColor::HexArgb)).size();
        (void)n;
    });
    std::printf("\n⑤ 拆解：取 palette %.1f us / name() %.1f us / 拼 key %.1f us\n",
                tPalette, tName, tKey);

    std::printf("\n④ 单张 48px 上色本身：%.1f us（含一次 QPixmap 深拷贝）\n", tint);

    return 0;
}
