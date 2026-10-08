// ============================================================================
//  grid_test.cpp —— 网格吸附的离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    吸附是**纯交互**特性 —— 算错了不会崩、不会报错，只是"拖过去的时候
//    没吸上"或者"没到线就被拽走了"。这两类错都只在自己动手拖的时候才感觉得
//    到，而且很容易归因成"这个功能就这样"。所以每一条判据都要机械可验：
//
//      ① **容差内吸、容差外不动** —— 这是"吸附 ≠ 强制取整"的分界线，
//         也是最容易被写成"无条件取整"的一条；
//      ② **最近线是四舍五入、不是截断** —— 截断会让"从右往左拖"与
//         "从左往右拖"手感不同，是个很隐蔽的错；
//      ③ **矩形吸附是整体平移、不是改形** —— 宽高必须一个像素都不变；
//      ④ **每轴挑更近的那条边** —— 否则"想让右边对齐"永远对不上；
//      ⑤ **退化输入不能崩、不能乱吸** —— 间距 <= 0 / 空矩形 / 负容差。
//
//    不需要 QGuiApplication：本文件只链 engine/GridMetrics.cpp（纯 QtCore）。
//    控件那一半（菜单真的改了网格、拖动真的吸上了）在 move_tool / selection /
//    mainwin 里验。
// ============================================================================

#include "engine/GridMetrics.h"

#include <QtGlobal>

#include <cmath>
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

static void checkNear(qreal actual, qreal expected, const char *what,
                      qreal absTol = 1e-9)
{
    if (std::fabs(actual - expected) > absTol) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %.10g，实际 %.10g\n",
                    what, double(expected), double(actual));
    } else {
        ++g_passed;
    }
}

static void checkPoint(const QPointF &actual, qreal ex, qreal ey, const char *what)
{
    checkNear(actual.x(), ex, what);
    checkNear(actual.y(), ey, what);
}

static void checkOffset(const QPoint &actual, int ex, int ey, const char *what)
{
    if (actual.x() != ex || actual.y() != ey) {
        ++g_failed;
        std::printf("  [失败] %s  期望 (%d,%d)，实际 (%d,%d)\n",
                    what, ex, ey, actual.x(), actual.y());
    } else {
        ++g_passed;
    }
}

// ===========================================================================
//  [1] isUsable / toleranceInImage —— 生效判据与容差折算
// ===========================================================================
static void testUsabilityAndTolerance()
{
    beginGroup("生效判据与容差折算");

    check(!gridmetrics::isUsable(0),  "间距 0 → 不可用");
    check(!gridmetrics::isUsable(-8), "间距为负 → 不可用");
    check(gridmetrics::isUsable(1),   "间距 1 → 可用");
    check(gridmetrics::isUsable(64),  "间距 64 → 可用");

    // ★ 容差折算：屏幕像素 ÷ 缩放 ★
    //   缩放 1.0 时二者相等；放大时图像容差变小（屏幕上仍是 8px）；
    //   缩小时图像容差变大。这条保证"手感与缩放无关"。
    checkNear(gridmetrics::toleranceInImage(8.0, 1.0), 8.0,   "缩放 1.0 → 8 屏幕像素 = 8 图像像素");
    checkNear(gridmetrics::toleranceInImage(8.0, 2.0), 4.0,   "缩放 2.0 → 图像容差减半");
    checkNear(gridmetrics::toleranceInImage(8.0, 0.5), 16.0,  "缩放 0.5 → 图像容差翻倍");

    // ★ 非法缩放回落 1.0 ★
    //   回落成 0 会让容差永远是 0 = 永远不吸附（功能静默失效），
    //   比"用 1:1 容差不精确"严重得多。
    checkNear(gridmetrics::toleranceInImage(8.0, 0.0),  8.0, "★ zoom=0 → 按 1.0 处理（不是 0）");
    checkNear(gridmetrics::toleranceInImage(8.0, -3.0), 8.0, "★ zoom<0 → 按 1.0 处理");
}

// ===========================================================================
//  [2] nearestLine / snapValue —— 最近线是四舍五入
// ===========================================================================
static void testNearestAndSnap()
{
    beginGroup("最近网格线与一维吸附");

    // ★ 四舍五入，不是截断 ★
    checkNear(gridmetrics::nearestLine(30.0, 64), 0.0,  "30 的最近线是 0（不是 64）");
    checkNear(gridmetrics::nearestLine(33.0, 64), 64.0, "33 的最近线是 64（刚过半）");
    checkNear(gridmetrics::nearestLine(32.0, 64), 64.0, "★ 32 恰好在中点 → 靠 64（qRound 半数远离零）");
    checkNear(gridmetrics::nearestLine(95.0, 64), 64.0, "95 的最近线是 64");
    checkNear(gridmetrics::nearestLine(-30.0, 64), 0.0, "负坐标同样按最近线算");

    // ── 容差内吸 ──
    checkNear(gridmetrics::snapValue(62.0, 64, 4.0), 64.0, "离 64 差 2、容差 4 → 吸到 64");
    checkNear(gridmetrics::snapValue(62.0, 64, 1.0), 62.0, "★ 差 2 但容差只有 1 → 原样不动");

    // ── 容差边界（恰好相等要吸，> 才不吸）──
    checkNear(gridmetrics::snapValue(60.0, 64, 4.0), 64.0, "★ 差 4 == 容差 4 → 吸（闭区间）");
    checkNear(gridmetrics::snapValue(59.0, 64, 4.0), 59.0, "★ 差 5 > 容差 4 → 不吸");

    // ── 负容差按 0：只有已经贴合才"吸"（等于不动）──
    checkNear(gridmetrics::snapValue(63.0, 64, -1.0), 63.0, "负容差 → 不吸");
    checkNear(gridmetrics::snapValue(64.0, 64, -1.0), 64.0, "负容差 → 已在线上，结果不变（证明没崩）");

    // ── 退化 ──
    checkNear(gridmetrics::snapValue(37.0, 0, 10.0),  37.0, "间距 0 → 原样返回（不除零）");
    checkNear(gridmetrics::snapValue(37.0, -4, 10.0), 37.0, "间距为负 → 原样返回");
    checkNear(gridmetrics::nearestLine(37.0, 0), 37.0, "最近线：间距 0 → 原样返回");

    // ── 二维点：两轴独立 ──
    checkPoint(gridmetrics::snapPoint(62.0, 130.0, 64, 4.0), 64.0, 128.0,
               "点吸附：x 到 64、y 到 128");
    checkPoint(gridmetrics::snapPoint(100.0, 200.0, 64, 4.0), 100.0, 200.0,
               "点吸附：两轴都超容差 → 原样");
}

// ===========================================================================
//  [3] snapOffsetForRect —— 整体平移、不改形、每轴挑最近边
// ===========================================================================
static void testRectOffset()
{
    beginGroup("矩形吸附：整体平移量");

    // ★ 只算平移，形状不变 ★
    //   left=30 right=67 g=64 → 右边(67)离 64 只差 3，左边(30)离 0 差 30
    //   → 挑右边 → dx = 64-67 = -3
    //   （y 轴特意放在 210，离上下两条线都 > 8，好让这条只验 x 轴行为）
    {
        const QRect r(30, 210, 38, 20);   // left=30, right=67（含端点 30..67）
        const QPoint off = gridmetrics::snapOffsetForRect(r, 64, 8.0);
        checkOffset(off, -3, 0, "★ 挑更近的右边吸：dx = -3（形状不变）");
        // 复核：平移后 right 恰好落在 64
        check(r.right() + off.x() == 64, "★ 平移后右边 = 64（贴合网格）");
        check((r.width()) == 38,          "★ 宽度一个像素都没变（是平移不是缩放）");
    }

    // ★ y 轴独立吸附：一个高瘦的框，只有竖直方向能吸上 ★
    {
        const QRect r(400, 200, 10, 20);  // top=200 离 192 差 8（恰好等于容差）
        const QPoint off = gridmetrics::snapOffsetForRect(r, 64, 8.0);
        checkOffset(off, 0, -8, "★ y 轴独立：top 差 8（= 容差）→ 吸到 192");
    }

    // 两轴都吸附：一个左上角的框
    //   top=126 → 离 128 差 2；bottom=... left=126 → 128
    {
        const QRect r(126, 126, 10, 10);  // left=126 right=135 bottom=135
        const QPoint off = gridmetrics::snapOffsetForRect(r, 64, 8.0);
        // left 离 128 差 2，right=135 离 128 差 7 → 挑左边 → dx = +2
        checkOffset(off, 2, 2, "两轴各挑最近边：dx=dy=+2");
    }

    // ★ 两边都超容差 → 该轴不动 ★
    {
        const QRect r(30, 30, 10, 10);    // left=30(离0差30,离64差34) right=39(离64差25)
        const QPoint off = gridmetrics::snapOffsetForRect(r, 64, 4.0);
        checkOffset(off, 0, 0, "★ 两边都超容差 → 不吸附（爱放哪放哪）");
    }

    // ── 退化：空矩形 / 间距非法 ──
    checkOffset(gridmetrics::snapOffsetForRect(QRect(), 64, 8.0), 0, 0, "空矩形 → (0,0)");
    checkOffset(gridmetrics::snapOffsetForRect(QRect(10, 10, 5, 5), 0, 8.0), 0, 0,
                "间距 0 → (0,0)");
    checkOffset(gridmetrics::snapOffsetForRect(QRect(10, 10, 5, 5), -4, 8.0), 0, 0,
                "间距为负 → (0,0)");

    // ── 已经贴合 → 偏移为 0，且**不会**被推走 ──
    {
        const QRect r(64, 128, 20, 20);   // left=64, top=128 恰好在网格上
        const QPoint off = gridmetrics::snapOffsetForRect(r, 64, 8.0);
        checkOffset(off, 0, 0, "★ 已贴合 → 偏移 0（吸附是二值的，不会来回抖）");
    }
}

// ===========================================================================

int main()
{
    std::printf("=== grid_test：网格吸附纯函数 ===\n\n");

    testUsabilityAndTolerance();
    testNearestAndSnap();
    testRectOffset();

    std::printf("\n----------------------------------------\n");
    std::printf("grid_test: 通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}