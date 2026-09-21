// ============================================================================
//  selection_test.cpp —— core/Selection 与 engine/Filters 的选区部分离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    选区这套代码有三个"编译通过、运行不报错、结果悄悄错"的重灾区：
//
//      ① **静默失效**：Selection::setFromPath() / applyPath() 在没有蒙版时
//         （新文档从未做过选区）会直接 return，一个标志位都不改。
//         表现是"用户拉了半天框，松手后什么都没有"，而且没有任何报错。
//         这一条是本测试最重要的回归点 —— 见 testPrepareCanvas()。
//
//      ② **布尔运算的边界**：四种运算（新建/添加/减去/交集）在
//         抗锯齿边缘处的灰阶值很难靠肉眼判断对错。
//
//      ③ **羽化的两极**：羽化过度会把选区中心也削掉（中心不再是 255），
//         羽化不足则边缘仍然生硬。这两种毛病在界面上都不明显，
//         必须逐像素断言"中心仍是 255、远处仍是 0、交界处落在中间"。
//
//    这些函数都不需要 QGuiApplication（QImage / QPainter / QPainterPath
//    对 Grayscale8 的操作不需要 GUI 环境），所以能脱离界面直接跑。
//    编译方式见 run_tests.sh。
// ============================================================================

#include "core/Selection.h"
#include "engine/Filters.h"

#include <QImage>
#include <QPainterPath>
#include <QSize>
#include <QtGlobal>

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

static void checkEq(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

// 读灰度蒙版某点；越界或空蒙版返回 -1（便于断言"不该有值"）
static int at(const Selection &sel, int x, int y)
{
    const QImage &m = sel.mask();
    if (m.isNull() || x < 0 || y < 0 || x >= m.width() || y >= m.height())
        return -1;
    return int(m.constScanLine(y)[x]);
}

static const QSize kCanvas(64, 64);

// ---------------------------------------------------------------------------
//  1. ★ 核心回归：prepareCanvas 与"静默失效" ★
// ---------------------------------------------------------------------------
static void testPrepareCanvas()
{
    std::printf("1. prepareCanvas（静默失效的回归测试）\n");

    // ── 初始状态 ──
    Selection sel;
    check(!sel.isActive(), "新建的选区：isActive() == false");
    check(sel.isAllSelected(), "新建的选区：isAllSelected() == true");
    check(sel.mask().isNull(), "新建的选区：蒙版为空（这是正常状态，不是错误）");

    // ── 复现那个坑：不调 prepareCanvas 就直接设路径 ──
    //
    // 这一段是在**固化错误行为**，目的是让"忘记调 prepareCanvas"
    // 这件事立刻可见。如果哪天有人把 setFromPath 改成"自动按路径
    // 包围盒分配蒙版"，这里会失败 —— 那时应该同步更新这段断言，
    // 而不是留着它误导后人。
    QPainterPath path;
    path.addRect(QRectF(10, 10, 20, 20));
    sel.setFromPath(path, false);

    check(!sel.isActive(), "★ 没有 prepareCanvas 时 setFromPath 静默失效（isActive 仍为 false）");
    check(sel.mask().isNull(), "★ 没有 prepareCanvas 时蒙版仍为空");

    // ── 正确的顺序：先 prepareCanvas，再设路径 ──
    sel.prepareCanvas(kCanvas);

    check(!sel.mask().isNull(), "prepareCanvas 之后蒙版已建立");
    checkEq(sel.mask().width(), 64, "蒙版宽度 == 画布宽度");
    checkEq(sel.mask().height(), 64, "蒙版高度 == 画布高度");
    checkEq(sel.mask().format(), int(QImage::Format_Grayscale8), "蒙版格式为 Grayscale8");

    // ★ prepareCanvas 必须**不**把状态变成"有选区" ★
    //   全 255 的蒙版在语义上等价于"没有选区限制"。如果这里置了
    //   m_active，所有工具都会白白走一遍逐像素蒙版乘法。
    check(!sel.isActive(), "★ prepareCanvas 不改变 isActive()（全 255 等价于全选）");
    check(sel.isAllSelected(), "prepareCanvas 之后仍视为全选");
    checkEq(at(sel, 0, 0), 255, "★ prepareCanvas 底色是 255 而不是 0");

    // ── 现在 setFromPath 应该真的生效了 ──
    QPainterPath path2;
    path2.addRect(QRectF(10, 10, 20, 20));
    sel.setFromPath(path2, false);

    check(sel.isActive(), "prepareCanvas 之后 setFromPath 生效");
    check(!sel.isAllSelected(), "有选区时不再是全选");
    checkEq(at(sel, 20, 20), 255, "矩形内部被选中");
    checkEq(at(sel, 5, 5), 0, "矩形外部未选中");
    checkEq(at(sel, 63, 63), 0, "远处角落未选中");

    // ── prepareCanvas 不能覆盖已有的合法选区 ──
    int before = at(sel, 20, 20);
    sel.prepareCanvas(kCanvas);
    checkEq(at(sel, 20, 20), before, "★ 已有选区时 prepareCanvas 不覆盖内容");
    check(sel.isActive(), "★ 已有选区时 prepareCanvas 不改变 isActive()");

    // ── 尺寸对不上时的降级行为 ──
    //
    // 走到这个分支说明"画布尺寸变了但没人通知选区"（正常流程里
    // Document::syncSelectionToCanvas 会处理，所以不该发生）。
    // 此时旧蒙版已经没有任何可用语义，行为被定义为"退回全选"。
    // 这里把这个行为钉住，免得以后有人改成"留着旧内容"却没人知道。
    sel.prepareCanvas(QSize(32, 32));
    checkEq(sel.mask().width(), 32, "画布尺寸变化后蒙版跟着重建（宽）");
    checkEq(sel.mask().height(), 32, "画布尺寸变化后蒙版跟着重建（高）");
    check(!sel.isActive(), "★ 尺寸对不上 → 退回\u201c全选\u201d状态（旧选区作废）");
    checkEq(at(sel, 16, 16), 255, "★ 重建后的底色是 255（全选）");

    // ── 非法尺寸不能崩 ──
    sel.prepareCanvas(QSize(0, 0));
    checkEq(sel.mask().width(), 32, "非法尺寸（0×0）被忽略，蒙版不变");
}

// ---------------------------------------------------------------------------
//  2. 矩形选区的精确边界
// ---------------------------------------------------------------------------
static void testSelectRect()
{
    std::printf("2. 矩形选区的边界\n");

    Selection sel;
    sel.prepareCanvas(kCanvas);

    // 关抗锯齿才能断言精确的 0/255 —— 开着的话边缘会有过渡灰阶，
    // 那是**正确行为**（下一节专门测它），但不适合做精确比较。
    sel.selectRect(QRect(10, 10, 20, 20), Selection::Op::Replace, false);

    checkEq(at(sel, 10, 10), 255, "左上角（含）被选中");
    checkEq(at(sel, 29, 29), 255, "右下角（含）被选中");
    checkEq(at(sel, 30, 30), 0, "右下角的右边一格未选中");
    checkEq(at(sel, 9, 10), 0, "左边一格未选中");
    checkEq(at(sel, 10, 9), 0, "上边一格未选中");

    // bounds() 应给出与输入一致的正矩形
    const QRect b = sel.bounds();
    check(b == QRect(10, 10, 20, 20), "bounds() 与选择区域一致");
}

// ---------------------------------------------------------------------------
//  3. 四种布尔运算
// ---------------------------------------------------------------------------
static void testBooleanOps()
{
    std::printf("3. 四种布尔运算\n");

    // ── Replace ──
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);
        sel.selectRect(QRect(0, 0, 32, 32), Selection::Op::Replace, false);
        checkEq(at(sel, 0, 0), 255, "Replace：被选区域 255");
        checkEq(at(sel, 63, 63), 0, "Replace：未选区域 0");
    }

    // ── Add（并集）──
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);
        sel.selectRect(QRect(0, 0, 32, 32), Selection::Op::Replace, false);
        sel.selectRect(QRect(32, 0, 32, 32), Selection::Op::Add, false);
        checkEq(at(sel, 10, 10), 255, "Add：原区域仍在");
        checkEq(at(sel, 40, 10), 255, "Add：新区域被加入");
        checkEq(at(sel, 40, 40), 0, "Add：没碰过的区域仍未选中");
    }

    // ── Add 在"原本全选"上的语义 ──
    //
    // 新文档的用户第一次操作就是 Shift 拖一个框，此时底色 255。
    // 并集的结果应该仍是全选 —— 而不是"变成那个框"。
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);   // 底色 255
        sel.selectRect(QRect(10, 10, 20, 20), Selection::Op::Add, false);
        checkEq(at(sel, 0, 0), 255, "★ Add 于全选之上：边角仍是 255（并集正确）");
        checkEq(at(sel, 30, 30), 255, "★ Add 于全选之上：框内也是 255");
    }

    // ── Subtract（差集）──
    //
    // ★ 这里的实现是"按比例扣减"，不是直接归零 ★
    //   所以对 255 的像素减去 255 得到 0（完全扣除），
    //   而对 128 的像素减去 255 得到 0 附近（比例扣减）。
    //   这个设计是为了让抗锯齿/羽化边缘在叠加运算后仍然平滑。
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);
        sel.selectRect(QRect(0, 0, 64, 32), Selection::Op::Replace, false);  // 上半
        sel.selectRect(QRect(0, 0, 32, 32), Selection::Op::Subtract, false); // 减去左上
        checkEq(at(sel, 10, 10), 0, "Subtract：被减区域变 0");
        checkEq(at(sel, 40, 10), 255, "Subtract：未减到的区域保持 255");
        checkEq(at(sel, 10, 40), 0, "Subtract：原本就没选的区域仍是 0");
    }

    // ── Subtract 于全选之上（挖洞）──
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);
        sel.selectRect(QRect(10, 10, 20, 20), Selection::Op::Subtract, false);
        checkEq(at(sel, 20, 20), 0, "★ Subtract 于全选之上：框内被挖空");
        checkEq(at(sel, 0, 0), 255, "★ Subtract 于全选之上：框外仍是全选");
    }

    // ── Intersect（交集）──
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);
        sel.selectRect(QRect(0, 0, 32, 32), Selection::Op::Replace, false);
        sel.selectRect(QRect(16, 16, 32, 32), Selection::Op::Intersect, false);
        checkEq(at(sel, 20, 20), 255, "Intersect：重叠区被保留");
        checkEq(at(sel, 5, 5), 0, "Intersect：只有第一块的地方被剔除");
        checkEq(at(sel, 40, 40), 0, "Intersect：只有第二块的地方被剔除");

        const QRect b = sel.bounds();
        check(b == QRect(16, 16, 16, 16), "Intersect：结果包围盒正确");
    }
}

// ---------------------------------------------------------------------------
//  4. 抗锯齿边缘
// ---------------------------------------------------------------------------
static void testAntialias()
{
    std::printf("4. 抗锯齿边缘\n");

    Selection sel;
    sel.prepareCanvas(kCanvas);

    // 椭圆在"像素边界"上必然产生非整覆盖，开抗锯齿时边缘一定有过渡值。
    sel.selectEllipse(QRect(8, 8, 48, 48), Selection::Op::Replace, true);

    check(sel.isActive(), "椭圆选区建立成功");
    checkEq(at(sel, 32, 32), 255, "椭圆中心完全选中");

    // 统计"部分选中"的像素数。边缘若无过渡，说明抗锯齿没生效 ——
    // 那正是"椭圆看起来有锯齿"的原因。
    const QImage &m = sel.mask();
    int partial = 0;
    for (int y = 0; y < m.height(); ++y) {
        const uchar *line = m.constScanLine(y);
        for (int x = 0; x < m.width(); ++x) {
            if (line[x] > 0 && line[x] < 255)
                ++partial;
        }
    }
    check(partial > 50, "★ 抗锯齿产生足量的过渡像素（边缘平滑）");

    // 四个角必须在椭圆之外
    checkEq(at(sel, 8, 8), 0, "椭圆外接矩形的左上角在椭圆之外");
    checkEq(at(sel, 55, 55), 0, "椭圆外接矩形的右下角在椭圆之外");
}

// ---------------------------------------------------------------------------
//  5. 反选
// ---------------------------------------------------------------------------
static void testInvert()
{
    std::printf("5. 反选\n");

    // 无选区时反选应当什么都不做（死胡同，实现里主动返回）
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);
        sel.invert();
        check(!sel.isActive(), "★ 全选状态反选：不做任何事（避免得到全 0 蒙版）");
    }

    // 有选区时反选
    {
        Selection sel;
        sel.prepareCanvas(kCanvas);
        sel.selectRect(QRect(0, 0, 32, 32), Selection::Op::Replace, false);
        sel.invert();
        checkEq(at(sel, 10, 10), 0, "反选：原选中区变 0");
        checkEq(at(sel, 40, 40), 255, "反选：原未选区变 255");
        checkEq(at(sel, 63, 0), 255, "反选：右上角（原本未选）现在是 255");

        // 反选两次应回到原状
        sel.invert();
        checkEq(at(sel, 10, 10), 255, "★ 反选两次回到原状");
        checkEq(at(sel, 40, 40), 0, "★ 反选两次回到原状（另一侧）");
    }
}

// ---------------------------------------------------------------------------
//  6. clone / clear 的语义
// ---------------------------------------------------------------------------
static void testCloneAndClear()
{
    std::printf("6. clone 与 clear\n");

    Selection sel;
    sel.prepareCanvas(kCanvas);
    sel.selectRect(QRect(10, 10, 20, 20), Selection::Op::Replace, false);

    Selection copy = sel.clone();
    checkEq(at(copy, 20, 20), 255, "clone 复制了蒙版内容");
    check(copy.isActive(), "clone 复制了 active 标志");

    // ★ 必须是深拷贝 ★
    //   QImage 是隐式共享的。如果 clone 直接赋值 m_mask 而不 copy()，
    //   改副本会连带改原对象 —— 撤销快照就废了（而且不崩溃）。
    sel.selectRect(QRect(40, 40, 10, 10), Selection::Op::Replace, false);
    checkEq(at(copy, 20, 20), 255, "★ clone 是深拷贝：改原对象不影响副本");
    checkEq(at(copy, 45, 45), 0, "★ clone 是深拷贝：副本没有后来加的内容");

    copy.clear();
    check(!copy.isActive(), "clear 之后 isActive() == false");
    check(copy.isAllSelected(), "clear 之后 isAllSelected() == true");
    check(copy.mask().isNull(), "clear 释放了蒙版");
    checkEq(at(copy, 20, 20), -1, "clear 之后蒙版为空");
}

// ---------------------------------------------------------------------------
//  7. 羽化（engine/filters::featherGrayMask）
// ---------------------------------------------------------------------------
static void testFeather()
{
    std::printf("7. 羽化\n");

    // ── 边界情况 ──
    check(filters::featherGrayMask(QImage(), 5.0).isNull(), "空输入 → 返回空");
    {
        QImage src(8, 8, QImage::Format_Grayscale8);
        src.fill(200);
        const QImage r = filters::featherGrayMask(src, 0.0);
        checkEq(int(r.constScanLine(0)[0]), 200, "半径 0 → 原样返回");
        const QImage r2 = filters::featherGrayMask(src, -3.0);
        checkEq(int(r2.constScanLine(0)[0]), 200, "负半径 → 原样返回");
    }

    // ── 真实羽化 ──
    QImage mask(64, 64, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = 16; y < 48; ++y) {
        uchar *line = mask.scanLine(y);
        for (int x = 16; x < 48; ++x)
            line[x] = 255;
    }

    const QImage blurred = filters::featherGrayMask(mask, 6.0);

    checkEq(int(blurred.constScanLine(32)[32]), 255,
            "★ 羽化后中心仍是 255（不能把选区内部一起削掉）");
    checkEq(int(blurred.constScanLine(0)[0]), 0,
            "★ 羽化后远处仍是 0（羽化范围不能无限扩散）");
    checkEq(int(blurred.constScanLine(2)[2]), 0, "羽化影响范围有界");

    // 交界处必须出现中间值 —— 这正是"羽化"的定义
    const int edge = int(blurred.constScanLine(32)[16]);
    check(edge > 0 && edge < 255, "★ 边界处出现过渡灰阶（这才是羽化）");

    // ── ★ 半径的口径：扩散范围应符合标称值，不能是它的 3 倍 ★ ──
    //
    //   三次盒式模糊是串联的，总扩散半径 ≈ 3 × 单趟半径。
    //   如果实现里把用户给的 radius 直接当作**单趟**半径
    //   （本项目历史版本就是这么写的），实际羽化范围会变成标称值的
    //   3 倍 —— 用户填"羽化 6 像素"却得到 18 像素的模糊，
    //   选区的硬边彻底消失。这个 bug 不会报错、不会崩，
    //   只表现为"羽化好像比数字厉害得多"，极难发现。
    //
    //   下面两条断言把它钉死。标称 6 像素 → 单趟半径 2 → 总扩散 6 像素：
    //     · 距边界 3 像素处（x=13，边界在 x=16）必须已被染到
    //     · 距边界 8 像素处（x=8）必须还没被染到
    //   若有人改回"直接当单趟半径"，第二条会立刻失败（那时扩散 18 像素）。
    const uchar *line = blurred.constScanLine(32);
    check(int(line[13]) > 0, "★ 羽化确实向外渗出了（距边界 3 像素内有过渡）");
    checkEq(int(line[8]), 0, "★ 羽化扩散范围符合标称半径（距边界 8 像素处仍为 0）");

    // 检查过渡带确实存在且单调：沿一条水平线从外到内扫描，
    // 值应非递减（可能出现平台，但不该回退）。
    bool monotonic = true;
    int prev = 0;
    int crossings = 0;
    for (int x = 8; x < 32; ++x) {
        const int v = int(line[x]);
        if (v < prev)
            monotonic = false;
        if (v > 0 && v < 255)
            ++crossings;
        prev = v;
    }
    check(monotonic, "★ 羽化过渡沿法线方向单调（无过冲、无振铃）");
    check(crossings >= 3, "过渡带宽度合理（至少几个像素）");

    // ── 格式鲁棒性 ──
    {
        // 非 Grayscale8 输入会被现场转换，不应崩、不应返回空
        QImage rgb(16, 16, QImage::Format_ARGB32_Premultiplied);
        rgb.fill(Qt::white);
        const QImage r = filters::featherGrayMask(rgb, 3.0);
        check(!r.isNull(), "非灰度输入不会返回空（内部转换）");
        checkEq(r.format(), int(QImage::Format_Grayscale8), "输出统一为 Grayscale8");
    }
}

// ---------------------------------------------------------------------------
//  8. 综合场景：模拟一次选区工具的完整操作序列
// ---------------------------------------------------------------------------
static void testToolSequence()
{
    std::printf("8. 综合：模拟选区工具的操作序列\n");

    // 这个场景完全照抄 SelectionTool 的调用顺序，用来保证
    // "工具的调用方式"与"Selection 的接口约定"始终对得上。
    Selection sel;

    // ① mousePress：prepareCanvas
    sel.prepareCanvas(kCanvas);

    // ② 松手：生成形状蒙版 → 羽化 → 合入
    QImage shape(kCanvas, QImage::Format_Grayscale8);
    shape.fill(0);
    {
        for (int y = 10; y < 40; ++y) {
            uchar *line = shape.scanLine(y);
            for (int x = 10; x < 40; ++x)
                line[x] = 255;
        }
    }
    const QImage feathered = filters::featherGrayMask(shape, 4.0);
    sel.applyMask(feathered, Selection::Op::Replace);

    check(sel.isActive(), "第一次操作后选区处于活动状态");
    checkEq(at(sel, 25, 25), 255, "选区中心完全选中");

    // ③ 第二次操作：Add 一个不重叠的区域
    QImage shape2(kCanvas, QImage::Format_Grayscale8);
    shape2.fill(0);
    {
        for (int y = 45; y < 60; ++y) {
            uchar *line = shape2.scanLine(y);
            for (int x = 45; x < 60; ++x)
                line[x] = 255;
        }
    }
    sel.applyMask(shape2, Selection::Op::Add);

    checkEq(at(sel, 50, 50), 255, "Add 之后新区域也被选中");
    checkEq(at(sel, 25, 25), 255, "Add 之后老区域未被破坏");
    checkEq(at(sel, 5, 5), 0, "Add 之后没碰过的区域仍未选中");

    // ④ 退化情况：Replace 一个全 0 蒙版 == 取消选择
    QImage empty(kCanvas, QImage::Format_Grayscale8);
    empty.fill(0);
    sel.applyMask(empty, Selection::Op::Replace);

    // applyMask 会把状态置为 active，需要调用方（工具）自己归一化。
    // 这里验证"归一化"这道工序确实能被检测出来 ——
    // 工具用 SelectionTool::isMaskEmpty() 判断后调 clear()。
    bool allZero = true;
    for (int y = 0; y < sel.mask().height() && allZero; ++y) {
        const uchar *l = sel.mask().constScanLine(y);
        for (int x = 0; x < sel.mask().width(); ++x) {
            if (l[x] != 0) { allZero = false; break; }
        }
    }
    check(allZero, "★ Replace 全 0 蒙版后整份蒙版归零（工具据此归一化为 clear）");

    sel.clear();
    check(!sel.isActive(), "clear 之后回到「无选区 == 全选」的状态");
}

int main()
{
    std::printf("===== core/Selection + engine/Filters 选区自检 =====\n\n");

    testPrepareCanvas();
    testSelectRect();
    testBooleanOps();
    testAntialias();
    testInvert();
    testCloneAndClear();
    testFeather();
    testToolSequence();

    std::printf("\n===== 通过 %d 项，失败 %d 项 =====\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
