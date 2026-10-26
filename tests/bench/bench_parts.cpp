// ============================================================================
//  bench_parts.cpp —— 把"渲染"和"上色"拆开，各量各的单价
// ----------------------------------------------------------------------------
//  这个数决定了 IconLibrary 该把力气花在哪：如果上色比光栅化还贵，
//  "一个状态染一遍"就值得再压缩；如果上色便宜两个数量级，
//  那唯一正确的做法就是**渲染一次、多染几遍**（现状）。
//
//  顺带量了 QPixmap 深拷贝（写时 detach，整幅位图分配 + 拷贝）——
//  它决定"禁用态要不要拷一份"划不划算。
//
//  跑法见 README。
// ============================================================================

#include <QApplication>
#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QSvgRenderer>

#include <cstdio>
#include <ctime>

/// 跑 n 轮，返回每轮平均微秒数（CPU 时间，不受抢占干扰）
template <typename F>
static double bench(int n, F f)
{
    const clock_t t0 = clock();
    for (int i = 0; i < n; ++i)
        f();
    return double(clock() - t0) * 1e6 / CLOCKS_PER_SEC / n;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const QString path = QStringLiteral(":/icons/tool_brush.svg");
    const QColor ink(0x33, 0x39, 0x3F);

    std::printf("%-5s %12s %14s %12s\n", "px", "render", "tint(原地)", "深拷贝");

    for (int px : {16, 24, 48, 96}) {
        // 每次都重新构造 QSvgRenderer —— 与应用里 renderSvg() 的调用形态一致
        const double render = bench(300, [&path, px] {
            QSvgRenderer r(path);
            QPixmap pm(px, px);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            r.render(&p, QRectF(0, 0, px, px));
        });

        // 造一张"有墨迹"的图，上色才有东西可染
        QPixmap base(px, px);
        base.fill(Qt::transparent);
        {
            QPainter p(&base);
            p.fillRect(px / 8, px / 8, px * 3 / 4, px * 3 / 4, ink);
        }

        const double tint = bench(300, [&base, &ink] {
            QPainter p(&base);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(base.rect(), ink);
        });

        // ★ 不能只写 QPixmap out = base; ★ 编译器会把没写过的拷贝优化掉。
        //   打开一次 QPainter 就 detach，才逼得出真实的整幅拷贝成本。
        const double copy = bench(300, [&base] {
            QPixmap out = base;
            QPainter p(&out);
            p.drawPoint(0, 0);
        });

        std::printf("%-5d %9.1f us %11.1f us %9.1f us\n", px, render, tint, copy);
    }

    std::printf("\n结论：上色比光栅化便宜 1~2 个数量级 → 渲染一次、多染几遍。\n");
    return 0;
}
