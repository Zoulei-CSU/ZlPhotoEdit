// ============================================================================
//  ruler_test.cpp —— 标尺刻度与单位换算的离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    标尺是**纯呈现**部件 —— 它算错了不会崩、不会报错，只是"尺子上的数字
//    莫名其妙"。而这类错最难靠人眼看出来：数字 100 与 200 放错位置，
//    看起来仍然是"一条正常的尺子"。
//
//    所以刻度选择的每一条判据都要能被机械验证：
//      ① **刻度不能挤在一起**（数字叠字）—— 钉住"主刻度 × 单位像素 ≥ 最小间距"；
//      ② **刻度要好看**（1/2/5×10^n）—— 钉住 niceStep 的取值；
//      ③ **单位换算与 DPI 挂钩** —— 钉住 px/inch/cm/mm/pt 的比值关系；
//      ④ **"-0" 不出现** —— 这是标尺最显眼的一种"看起来像 bug"。
//
//    不需要 QGuiApplication：本文件只链 engine/RulerMetrics.cpp（纯 QtCore）。
//    编译与运行方式见 run_tests.sh 的 `ruler)` 分支。
// ============================================================================

#include "engine/RulerMetrics.h"

#include <QtGlobal>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace zl;
using rulermetrics::Unit;

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

// 浮点相等用**相对误差**比 —— 绝对误差在 1e-9 与 1e+3 两端都不合适。
static void checkNear(qreal actual, qreal expected, const char *what,
                      qreal relTol = 1e-9)
{
    const qreal scale = std::fmax(1.0, std::fabs(expected));
    if (std::fabs(actual - expected) > relTol * scale) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %.10g，实际 %.10g\n",
                    what, double(expected), double(actual));
    } else {
        ++g_passed;
    }
}

static void checkStr(const QString &actual, const QString &expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 \"%s\"，实际 \"%s\"\n",
                    what, qUtf8Printable(expected), qUtf8Printable(actual));
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  ① 单位换算
// ---------------------------------------------------------------------------
static void testPixelsPerUnit()
{
    beginGroup("单位换算：1 单位 = 多少图像像素");

    checkNear(rulermetrics::pixelsPerUnit(Unit::Pixel, 96), 1.0,
              "像素恒为 1（与 DPI 无关）");
    checkNear(rulermetrics::pixelsPerUnit(Unit::Pixel, 300), 1.0,
              "★ 像素单位在 300dpi 下仍为 1（DPI 不该污染像素刻度）");

    checkNear(rulermetrics::pixelsPerUnit(Unit::Inch, 96), 96.0,
              "96dpi 下 1 英寸 = 96 像素");
    checkNear(rulermetrics::pixelsPerUnit(Unit::Inch, 300), 300.0,
              "300dpi 下 1 英寸 = 300 像素");
    checkNear(rulermetrics::pixelsPerUnit(Unit::Centimeter, 96), 96.0 / 2.54,
              "厘米 = dpi / 2.54");
    checkNear(rulermetrics::pixelsPerUnit(Unit::Millimeter, 96), 96.0 / 25.4,
              "毫米 = dpi / 25.4");
    checkNear(rulermetrics::pixelsPerUnit(Unit::Point, 96), 96.0 / 72.0,
              "点 = dpi / 72");

    // 比值关系：1cm = 10mm、1inch = 25.4mm —— 单位之间的换算不能各写各的
    const qreal cm = rulermetrics::pixelsPerUnit(Unit::Centimeter, 120);
    const qreal mm = rulermetrics::pixelsPerUnit(Unit::Millimeter, 120);
    checkNear(cm / mm, 10.0, "1 厘米恰好是 10 毫米（两处换算同源）");

    // ★ 元数据缺失（dpi=0）必须兜底，而不是让刻度全变成 0/inf
    checkNear(rulermetrics::pixelsPerUnit(Unit::Inch, 0), 96.0,
              "★ dpi=0（元数据缺失）兜底到 96");
    checkNear(rulermetrics::pixelsPerUnit(Unit::Inch, -5), 96.0,
              "★ 负 dpi 同样兜底到 96");
}

// ---------------------------------------------------------------------------
//  ② niceStep —— 1 / 2 / 5 × 10^n
// ---------------------------------------------------------------------------
static void testNiceStep()
{
    beginGroup("刻度步长：1 / 2 / 5 × 10ⁿ");

    checkNear(rulermetrics::niceStep(1.0),   1.0,   "1 → 1");
    checkNear(rulermetrics::niceStep(0.7),   1.0,   "0.7 → 1（上取整）");
    checkNear(rulermetrics::niceStep(1.0),   1.0,   "1 边界 = 1");
    checkNear(rulermetrics::niceStep(1.01),  2.0,   "刚过 1 → 2");
    checkNear(rulermetrics::niceStep(2.0),   2.0,   "2 边界 = 2");
    checkNear(rulermetrics::niceStep(3.0),   5.0,   "3 → 5");
    checkNear(rulermetrics::niceStep(5.0),   5.0,   "5 边界 = 5");
    checkNear(rulermetrics::niceStep(6.0),   10.0,  "6 → 10");
    checkNear(rulermetrics::niceStep(12.0),  20.0,  "12 → 20");
    checkNear(rulermetrics::niceStep(90.0),  100.0, "90 → 100");
    checkNear(rulermetrics::niceStep(100.0), 100.0, "100 边界 = 100");
    checkNear(rulermetrics::niceStep(1000.0),1000.0,"1000 → 1000");
    checkNear(rulermetrics::niceStep(0.25),  0.5,   "0.25 → 0.5（物理单位要能出小数刻度）");
    checkNear(rulermetrics::niceStep(0.012), 0.02,  "0.012 → 0.02");

    // ★ 步长必须 ≥ 输入 ★ 这是本函数的定义（"不小于 minStep 的最小好看值"）。
    //   写反了（取小的那个）的表现是"刻度越来越密"，但每一档单看都合法，
    //   所以这里用一批随机值统一钉住。
    bool allGE = true;
    for (int i = 1; i <= 200; ++i) {
        const qreal x = qreal(i) * 0.137;
        if (rulermetrics::niceStep(x) < x)
            allGE = false;
    }
    check(allGE, "★ 对一批输入恒有 niceStep(x) ≥ x（是「不小于」而不是「最接近」）");

    // ★ 退化输入不能返回 0 ★ 返回 0 会让标尺"一条线都画不出来"
    checkNear(rulermetrics::niceStep(0.0),  1.0, "★ 0 → 1（而不是 0，否则刻度全消失）");
    checkNear(rulermetrics::niceStep(-3.0), 1.0, "★ 负数 → 1");
    check(std::isfinite(double(rulermetrics::niceStep(
              std::numeric_limits<qreal>::quiet_NaN()))),
          "★ NaN → 有限值（不把 NaN 传染给整条标尺）");
}

// ---------------------------------------------------------------------------
//  ③ chooseTicks —— 刻度要跟着缩放走，且不许挤在一起
// ---------------------------------------------------------------------------
static void testChooseTicks()
{
    beginGroup("刻度选择：跟缩放走 + 不挤在一起");

    // 像素单位、96dpi、要求主刻度间屏幕上至少 50px
    const rulermetrics::Ticks t1 =
        rulermetrics::chooseTicks(1.0, Unit::Pixel, 96, 50.0);
    checkNear(t1.major, 50.0, "zoom=1 → 主刻度 50 图像像素");
    checkNear(t1.minor, 10.0, "次刻度 = 主刻度 / 5");

    const rulermetrics::Ticks t8 =
        rulermetrics::chooseTicks(8.0, Unit::Pixel, 96, 50.0);
    checkNear(t8.major, 10.0, "zoom=8 → 主刻度缩到 10 图像像素（放大了，刻度该更细）");
    check(t8.major < t1.major, "★ 放大之后主刻度必须变小（跟着缩放走）");

    const rulermetrics::Ticks t01 =
        rulermetrics::chooseTicks(0.1, Unit::Pixel, 96, 50.0);
    checkNear(t01.major, 500.0, "zoom=0.1 → 主刻度放大到 500 图像像素");
    check(t01.major > t1.major, "★ 缩小之后主刻度必须变大");

    // ★ 档位跳跃时仍然"好看" ★
    const rulermetrics::Ticks t3 =
        rulermetrics::chooseTicks(3.0, Unit::Pixel, 96, 50.0);
    checkNear(t3.major, 20.0, "zoom=3 → 主刻度 20（50/3≈16.7 上取到 20，不是 16.7）");

    // ★ 不变量：主刻度在屏幕上的间距 ≥ 要求的最小间距 ★
    //   这是本函数存在的**全部理由**，破了就会出现叠字。
    bool spacingOK = true;
    for (qreal z = 0.05; z <= 20.0; z *= 1.3) {
        const rulermetrics::Ticks t =
            rulermetrics::chooseTicks(z, Unit::Pixel, 96, 50.0);
        if (t.major * z < 50.0 - 1e-6)
            spacingOK = false;
    }
    check(spacingOK, "★ 任意缩放下，主刻度屏幕间距恒 ≥ 最小间距（不会叠字）");

    // 物理单位：inch 在 96dpi 下 1 单位 = 96 图像像素
    const rulermetrics::Ticks ti =
        rulermetrics::chooseTicks(1.0, Unit::Inch, 96, 50.0);
    checkNear(ti.major, 1.0, "inch / zoom=1 → 主刻度 1 英寸");
    const rulermetrics::Ticks tiQ =
        rulermetrics::chooseTicks(0.25, Unit::Inch, 96, 50.0);
    checkNear(tiQ.major, 5.0, "inch / zoom=0.25 → 主刻度 5 英寸（96×0.25=24px/英寸）");

    // ★ DPI 必须真的参与换算 ★ 300dpi 下同样缩放，1 英寸占 300 像素
    const rulermetrics::Ticks ti300 =
        rulermetrics::chooseTicks(0.25, Unit::Inch, 300, 50.0);
    checkNear(ti300.major, 1.0,
              "★ inch / 300dpi / zoom=0.25 → 主刻度 1 英寸（DPI 参与换算）");

    // 退化：zoom=0 当 1 处理；minSpacing=0 不返回 0 步长
    const rulermetrics::Ticks tz0 =
        rulermetrics::chooseTicks(0.0, Unit::Pixel, 96, 50.0);
    checkNear(tz0.major, t1.major, "★ zoom=0 按 1 处理（不产生 inf/NaN）");
    const rulermetrics::Ticks ts0 =
        rulermetrics::chooseTicks(1.0, Unit::Pixel, 96, 0.0);
    check(ts0.major > 0.0, "★ minSpacing=0 也不返回 0 步长");
}

// ---------------------------------------------------------------------------
//  ④ 标签格式化
// ---------------------------------------------------------------------------
static void testFormatLabel()
{
    beginGroup("刻度标签：像素整数 / 物理小数 / 不出现 \"-0\"");

    checkStr(rulermetrics::formatLabel(100.0,    Unit::Pixel, 50.0),   QStringLiteral("100"),
             "像素 100 → \"100\"");
    checkStr(rulermetrics::formatLabel(1500.0,   Unit::Pixel, 500.0),  QStringLiteral("1500"),
             "像素 1500 → \"1500\"");
    checkStr(rulermetrics::formatLabel(2.4,      Unit::Pixel, 1.0),    QStringLiteral("2"),
             "★ 像素单位即使传入小数，标签也是整数（像素没有小数刻度）");

    checkStr(rulermetrics::formatLabel(2.0,  Unit::Inch, 1.0),  QStringLiteral("2"),
             "inch / 主刻度 1 → 不带小数");
    checkStr(rulermetrics::formatLabel(2.5,  Unit::Inch, 0.5),  QStringLiteral("2.5"),
             "inch / 主刻度 0.5 → 带 1 位小数");
    checkStr(rulermetrics::formatLabel(1.25, Unit::Inch, 0.25), QStringLiteral("1.25"),
             "inch / 主刻度 0.25 → 带 2 位小数");
    checkStr(rulermetrics::formatLabel(3.6,  Unit::Centimeter, 0.1), QStringLiteral("3.6"),
             "cm / 主刻度 0.1 → 带 1 位小数");

    // ★ "-0" 归一化 ★ 0 刻度左边那一格会算出 -0.0
    checkStr(rulermetrics::formatLabel(-0.0, Unit::Pixel, 50.0), QStringLiteral("0"),
             "★ -0.0（像素）→ \"0\"，不是 \"-0\"");
    checkStr(rulermetrics::formatLabel(-0.0, Unit::Inch, 0.5), QStringLiteral("0.0"),
             "★ -0.0（inch）→ \"0.0\"，不是 \"-0.0\"");
    checkStr(rulermetrics::formatLabel(-50.0, Unit::Pixel, 50.0), QStringLiteral("-50"),
             "真负数仍然带负号（只归一化 -0）");
}

// ===========================================================================

int main()
{
    std::printf("=== ruler_test：标尺刻度与单位换算 ===\n\n");

    testPixelsPerUnit();
    testNiceStep();
    testChooseTicks();
    testFormatLabel();

    std::printf("\n----------------------------------------\n");
    std::printf("ruler_test: 通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
