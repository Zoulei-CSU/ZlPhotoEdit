// ============================================================================
//  ui_smoke_test.cpp —— 调整对话框的离屏冒烟测试
// ----------------------------------------------------------------------------
//  为什么需要它：
//
//    阶段四新增的全是界面代码，而界面代码最难自动验证 —— 没有显示器，
//    也没法"点一下试试"。但它恰恰有一批**不需要人点**就能测到的东西：
//
//      ① 构造是否崩溃（对话框在构造函数里就要建控件、算直方图）
//      ② 参数为初值时 isChanged() 必须是 false
//         —— 否则用户"打开对话框看看就点确定"会往历史里塞一条空记录
//      ③ 参数为初值时 apply() 必须逐位不变
//         —— 否则"什么都没调"也会改画质，这是最伤信任的 bug
//      ④ 滑块 ⇄ 数字框的双向同步（那个 QSignalBlocker 连得对不对）
//      ⑤ 参数变化确实发出了 previewNeeded（预览链路是通的）
//      ⑥ 切换通道 / 应用预设后，下拉框状态与参数状态是否一致
//      ⑦ 曲线控件的绘制路径不崩（含空直方图、折线模式、极小尺寸）
//
//  ★ 怎么在没有显示器的环境里跑 ★
//    Qt 自带 qoffscreen 平台插件：窗口不显示，但控件真的会被创建、
//    布局、并且 QWidget::grab() 会真的走一遍 paintEvent 把内容渲染到
//    离屏缓冲。于是"绘制代码里空指针/除零/越界"这类崩溃都能被抓出来。
//    这比"启动主窗口看看进程还活着"强得多。
//
//  ★ 为什么不遵守"测试只链接 Core + Gui"的约定 ★
//    那条约定是给 engine/core 层当守门人的（防止有人往 engine 里塞
//    QWidget 依赖）。本测试的**唯一目的**就是测 ui 层，所以必须链接
//    Qt5Widgets，而且需要先跑 moc 生成元对象代码。
//    其他测试仍然只链接 Core + Gui —— 见 run_tests.sh 里的分工。
//
//  ★ 怎么在不暴露私有成员的前提下改参数 ★
//    用 findChildren<QSlider*>() / findChildren<QComboBox*>() 从外部拿到
//    控件并驱动它。这比"给对话框加一堆 setter 只为测试"更可取：走的是
//    用户真实的操作路径（滑块 valueChanged → 联动数字框 → 请求预览），
//    顺带把信号接线一起验证了。
// ============================================================================

#include "ui/AdjustmentDialog.h"
#include "ui/BrightnessContrastDialog.h"
#include "ui/HueSaturationDialog.h"
#include "ui/CurvesDialog.h"
#include "ui/CurveEditor.h"
#include "ui/LevelsDialog.h"
#include "ui/LevelsWidget.h"
#include "ui/SizeChangeDialogBase.h"
#include "ui/ImageSizeDialog.h"
#include "ui/CanvasSizeDialog.h"
#include "ui/RotateImageDialog.h"

#include "engine/Adjust.h"
#include "engine/Geometry.h"
#include "engine/Curves.h"
#include "engine/Lut.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QToolButton>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVector>
#include <QtGlobal>

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

static void checkEq(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

// 读原始存储值（预乘格式下就是内存里的真实值），理由见 adjust_test.cpp
static QRgb rawPixel(const QImage &img, int x, int y)
{
    const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
    return line[x];
}

// 像素级相等断言，失败时把两个值和格式都打出来。
// ★ 为什么不能只写 `check(a == b)` ★
//   像素比较失败时最需要知道的是"差多少" —— 是整体偏 1（舍入）、
//   还是某个通道完全不对（逻辑错），还是格式不同导致读法不同。
//   只报"失败"会让人从头再测一遍。
static void checkPixelSame(const QImage &got, const QImage &want, int x, int y,
                           const char *what)
{
    const QRgb a = rawPixel(got, x, y);
    const QRgb b = rawPixel(want, x, y);
    if (a == b) {
        ++g_passed;
        return;
    }

    ++g_failed;
    std::printf("  [失败] %s\n", what);
    std::printf("         期望 #%08X（%d,%d,%d,%d）\n",
                unsigned(b), qRed(b), qGreen(b), qBlue(b), qAlpha(b));
    std::printf("         实际 #%08X（%d,%d,%d,%d）  格式 %d vs %d\n",
                unsigned(a), qRed(a), qGreen(a), qBlue(a), qAlpha(a),
                int(got.format()), int(want.format()));
}

// 造一张"有不透明像素也有半透明像素"的源图。
// 半透明像素是必须的：它能验证调整过程没有破坏 alpha，
// 也能验证预乘往返没被 UI 层绕过。
//
// ★ 为什么直接写扫描线而不是 setPixel() ★
//   QImage::setPixel() 在 Format_ARGB32_Premultiplied 上会**先把传入的
//   QRgb 预乘**再写入。所以 setPixel(x, y, qRgba(128,128,128,128)) 得到的
//   内存值是 (64,64,64,128)（非预乘的 50% 灰），而不是"50% 透明的白"。
//   这类"写入时悄悄做了转换"的行为让测试数据到底长什么样变得依赖 Qt
//   的内部约定，读代码的人极容易误会。
//   直接写扫描线则所见即所得：内存里是什么就是什么。
//
// ★ 彩色像素（R≠G≠B）是必须的 ★
//   灰度图上"三通道分别查表"与"只查一个通道再广播"的结果完全相同，
//   所以只用灰像素的测试**根本发现不了**后者这个错误。
static QImage makeSource()
{
    QImage img(32, 32, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 32; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < 32; ++x)
            line[x] = qRgba(128, 96, 64, 255);   // 不透明，三通道互不相等
    }

    // 50% 透明（预乘空间直接写入：分量 64/64/64，alpha 128）
    reinterpret_cast<QRgb *>(img.scanLine(0))[0] = qRgba(64, 64, 64, 128);
    return img;
}

// 构造一张"主体在中间、两端各有极少量噪点"的图，专门用来验证自动色阶。
//
// ★ 为什么不能直接用 makeSource() 或均匀渐变色阶图 ★
//
//   自动色阶要处理的核心场景就是"画面里混着极少数纯黑/纯白噪点"：
//   按最小值/最大值取黑白场的话，一个噪点就把范围撑满，等于没调整；
//   按分位数取就能忽略它们。
//
//   而 makeSource() 是整幅单色 —— 两端分位数落在同一个桶里，
//   引擎会安全兜底成"不调整"（见 Adjust.cpp）；均匀渐变色阶图则
//   每个亮度值都有像素，两端的分位数同样落在 0 和 255 上。
//   两者都测不出自动色阶真正在做的事。
static QImage makeSpikyForAuto()
{
    QImage img(128, 8, QImage::Format_ARGB32_Premultiplied);
    int idx = 0;
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x, ++idx) {
            int v;
            if (idx == 0)
                v = 0;                            // 一个纯黑噪点
            else if (idx == 1)
                v = 255;                          // 一个纯白噪点
            else
                v = 60 + ((idx - 2) % 120);       // 主体落在 60..179
            line[x] = qRgba(v, v, v, 255);
        }
    }
    return img;
}

// 把对话框上所有滑块都推到一个非零值，模拟"用户动手调了"
static int pushSliders(QWidget *w, int value)
{
    const QList<QSlider *> sliders = w->findChildren<QSlider *>();
    for (QSlider *s : sliders)
        s->setValue(value);
    return sliders.size();
}

static bool slidersMatchSpinBoxes(QWidget *w)
{
    const QList<QSlider *> sliders = w->findChildren<QSlider *>();
    const QList<QSpinBox *> boxes = w->findChildren<QSpinBox *>();
    if (sliders.size() != boxes.size())
        return false;

    for (int i = 0; i < sliders.size(); ++i) {
        if (sliders[i]->value() != boxes[i]->value())
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  1. 亮度/对比度
// ---------------------------------------------------------------------------
static void testBrightnessContrast()
{
    std::printf("\n[1] 亮度/对比度对话框\n");

    const QImage src = makeSource();
    BrightnessContrastDialog dlg(src);

    // ── 初始状态 ──
    check(!dlg.isChanged(), "默认参数视为未修改（不会压空命令）");

    const QImage same = dlg.apply(src);
    checkEq(same.size().width(), src.size().width(), "apply 不改变尺寸");
    checkPixelSame(same, src, 5, 5, "默认参数下 apply 逐位不变（不打开也要不动画质）");
    check(qAlpha(rawPixel(same, 0, 0)) == 128, "默认参数下 alpha 不变");

    // ── 参数变化 → 请求预览 ──
    int previews = 0;
    QObject::connect(&dlg, &AdjustmentDialog::previewNeeded,
                     [&previews]() { ++previews; });

    checkEq(pushSliders(&dlg, 50), 2, "有两个滑块（亮度、对比度）");
    check(dlg.isChanged(), "滑块离开 0 后视为已修改");
    check(previews > 0, "参数变化会请求预览（预览链路是通的）");

    // ── 双向同步 ──
    check(slidersMatchSpinBoxes(&dlg), "滑块与数字框双向同步");

    // ── 结果确实变了，且 alpha 没被破坏 ──
    const QImage brighter = dlg.apply(src);
    check(rawPixel(brighter, 5, 5) != rawPixel(src, 5, 5), "亮度 +50 改变了像素");
    check(qRed(rawPixel(brighter, 5, 5)) > 128, "不透明像素被提亮");
    checkEq(qAlpha(rawPixel(brighter, 0, 0)), 128, "调整过程不破坏半透明像素的 alpha");

    // ── 预览开关 ──
    check(dlg.isPreviewEnabled(), "预览默认开启");
    check(dlg.findChild<QCheckBox *>() != nullptr, "存在「预览」勾选框");

    // ── 复位 ──
    dlg.resetParams();
    check(!dlg.isChanged(), "复位后回到未修改");
    checkPixelSame(dlg.apply(src), src, 5, 5, "复位后 apply 又逐位不变");
}

// ---------------------------------------------------------------------------
//  2. 色相 / 饱和度 / 明度
// ---------------------------------------------------------------------------
static void testHueSaturation()
{
    std::printf("\n[2] 色相/饱和度对话框\n");

    const QImage src = makeSource();
    HueSaturationDialog dlg(src);

    check(!dlg.isChanged(), "默认参数视为未修改");

    const QImage same = dlg.apply(src);
    checkEq(same.size().width(), src.size().width(), "apply 不改变尺寸");

    int previews = 0;
    QObject::connect(&dlg, &AdjustmentDialog::previewNeeded,
                     [&previews]() { ++previews; });

    checkEq(pushSliders(&dlg, 40), 3, "有三个滑块（色相、饱和度、明度）");
    check(dlg.isChanged(), "参数变化后视为已修改");
    check(previews > 0, "参数变化会请求预览");
    check(slidersMatchSpinBoxes(&dlg), "滑块与数字框双向同步");

    const QImage adjusted = dlg.apply(src);
    check(rawPixel(adjusted, 5, 5) != rawPixel(src, 5, 5), "色相 +40° 改变了像素");
    checkEq(qAlpha(rawPixel(adjusted, 0, 0)), 128, "不破坏 alpha");

    // ── 复位必须把所有三个参数都归零 ──
    dlg.resetParams();
    check(!dlg.isChanged(), "复位后回到未修改");

    const QList<QSlider *> sliders = dlg.findChildren<QSlider *>();
    bool allZero = true;
    for (QSlider *s : sliders)
        if (s->value() != 0)
            allZero = false;
    check(allZero, "复位把三个滑块都归零（不是只归零一部分）");
}

// ---------------------------------------------------------------------------
//  3. 曲线对话框
// ---------------------------------------------------------------------------
static void testCurvesDialog()
{
    std::printf("\n[3] 曲线对话框\n");

    const QImage src = makeSource();
    CurvesDialog dlg(src);

    check(!dlg.isChanged(), "默认设置视为未修改");

    const QImage same = dlg.apply(src);
    checkEq(same.size().width(), src.size().width(), "apply 不改变尺寸");
    check(rawPixel(same, 5, 5) == rawPixel(src, 5, 5),
          "恒等曲线下 apply 逐位不变");

    int previews = 0;
    QObject::connect(&dlg, &AdjustmentDialog::previewNeeded,
                     [&previews]() { ++previews; });

    const QList<QComboBox *> combos = dlg.findChildren<QComboBox *>();
    checkEq(combos.size(), 2, "有「通道」与「预设」两个下拉框");
    if (combos.size() < 2)
        return;

    QComboBox *channel = combos[0];
    QComboBox *preset  = combos[1];

    checkEq(channel->count(), 4, "通道下拉有 4 项（RGB / 红 / 绿 / 蓝）");
    checkEq(preset->count(), 1 + builtinPresets().size(),
            "预设下拉 = 1 个占位项 + 全部内置预设");

    // ── 只切换通道不算修改 ──
    channel->setCurrentIndex(1);   // 红通道
    check(!dlg.isChanged(), "只切换通道不算修改（不该压命令）");

    // ── 应用预设 ──
    preset->setCurrentIndex(2);    // 第 1 个真实预设（"提亮"）
    check(dlg.isChanged(), "应用预设后视为已修改");
    checkEq(preset->currentIndex(), 0,
            "应用预设后下拉框复位为占位项（避免显示与实际不符）");
    check(previews > 0, "应用预设会请求预览");

    // ── 预设只作用于当前通道 ──
    const QImage adjusted = dlg.apply(src);
    const QRgb a = rawPixel(adjusted, 5, 5);
    const QRgb o = rawPixel(src, 5, 5);
    check(qRed(a) > qRed(o), "「提亮」预设作用在红通道：红色变大");
    checkEq(qGreen(a), qGreen(o), "预设不影响其他通道：绿色不变");
    checkEq(qBlue(a), qBlue(o), "预设不影响其他通道：蓝色不变");
    checkEq(qAlpha(rawPixel(adjusted, 0, 0)), 128, "曲线调整不破坏 alpha");

    // ── 复位 ──
    dlg.resetParams();
    check(!dlg.isChanged(), "复位后回到未修改");
    checkEq(channel->currentIndex(), 0, "复位把通道拨回 RGB");
    checkPixelSame(dlg.apply(src), src, 5, 5, "复位后 apply 又逐位不变");
}

// ---------------------------------------------------------------------------
//  4. 曲线控件（构造、往返、绘制）
// ---------------------------------------------------------------------------
static void testCurveEditor()
{
    std::printf("\n[4] 曲线控件\n");

    CurveEditor ed;
    check(ed.curve().isIdentity(), "新建控件的曲线是恒等");
    check(ed.isSmooth(), "默认是平滑模式");

    // ── setCurve / curve 往返 ──
    QList<CurvePoint> pts;
    pts.append(CurvePoint(0.0, 0.0));
    pts.append(CurvePoint(0.5, 0.7));
    pts.append(CurvePoint(1.0, 1.0));
    ed.setCurve(Curve(pts));

    const Curve back = ed.curve();
    checkEq(back.points().size(), 3, "三个控制点往返不丢失");
    check(!back.isIdentity(), "往返后仍是非恒等曲线");

    if (back.points().size() == 3) {
        // 控件内部是 0..255 整数，往返会有 ≤1/510 的量化误差
        check(std::abs(back.points()[1].x - 0.5) < 0.005, "中点 x 往返一致");
        check(std::abs(back.points()[1].y - 0.7) < 0.005, "中点 y 往返一致");
        check(back.points()[0].x < 1e-6, "起点仍在 x=0");
        check(back.points()[2].x > 1.0 - 1e-6, "终点仍在 x=1");
    }

    // ── 绘制路径 ──
    //   grab() 会真的走一遍 paintEvent，把内容渲染到离屏缓冲。
    //   "空指针 / 除零 / 越界"这类崩溃在这里就会暴露。
    ed.setHistogram(QVector<int>(256, 1));
    ed.resize(240, 240);
    check(!ed.grab().isNull(), "带直方图可正常绘制");

    ed.setHistogram(QVector<int>());          // 空直方图
    check(!ed.grab().isNull(), "空直方图也能绘制（不能因此崩溃）");

    // 长度不对的直方图也要挡住（防御性）
    ed.setHistogram(QVector<int>(10, 3));
    check(!ed.grab().isNull(), "长度非 256 的直方图被安全忽略");

    ed.setHistogram(QVector<int>(256, 0));    // 全零直方图（空图层）
    check(!ed.grab().isNull(), "全零直方图也能绘制");

    ed.setSmooth(false);
    check(!ed.isSmooth(), "可切换到折线模式");
    check(!ed.grab().isNull(), "折线模式可正常绘制");
    ed.setSmooth(true);

    // ── 每一个内置预设都要能画（含递减的"反相"）──
    const QList<CurvePreset> presets = builtinPresets();
    int drawn = 0;
    for (int i = 0; i < presets.size(); ++i) {
        ed.setCurve(Curve(presets[i].points));
        if (!ed.grab().isNull())
            ++drawn;
    }
    checkEq(drawn, presets.size(), "全部内置预设都能正常绘制");

    // ── 极小尺寸：绘图区会被算成空矩形，所有绘制分支必须挡住 ──
    ed.resize(8, 8);
    check(!ed.grab().isNull(), "控件尺寸过小时不崩（空绘图区有保护）");

    // ── 空曲线（无控制点）也要能画 ──
    ed.setCurve(Curve(QList<CurvePoint>()));
    check(!ed.grab().isNull(), "空控制点列表也能绘制（退化为恒等）");
    check(ed.curve().isIdentity(), "空控制点列表退化为恒等曲线");
}

// ---------------------------------------------------------------------------
//  5. 色阶对话框
// ---------------------------------------------------------------------------
//
//  ★ 用 makeSource() 的彩色像素 ★
//    (128, 96, 64) 三通道互不相等，因此这组断言同时验证了
//    "色阶作用在三通道上"与"没有把彩色像素压成灰色"。
static void testLevelsDialog()
{
    std::printf("\n[5] 色阶对话框\n");

    const QImage src = makeSource();
    LevelsDialog dlg(src);

    check(!dlg.isChanged(), "默认参数视为未修改（不会压空命令）");

    const QImage same = dlg.apply(src);
    checkEq(same.size().width(), src.size().width(), "apply 不改变尺寸");
    check(rawPixel(same, 5, 5) == rawPixel(src, 5, 5),
          "恒等色阶下 apply 逐位不变");

    int previews = 0;
    QObject::connect(&dlg, &AdjustmentDialog::previewNeeded,
                     [&previews]() { ++previews; });

    // ── 控件定位 ──
    // ★ 按 objectName 找，不按 findChildren 的顺序 ★
    //   顺序定位在有人调整创建顺序时会静默断言到错的对象上。
    auto *inBlack    = dlg.findChild<QSpinBox *>(QStringLiteral("spinInBlack"));
    auto *inWhite    = dlg.findChild<QSpinBox *>(QStringLiteral("spinInWhite"));
    auto *outBlack   = dlg.findChild<QSpinBox *>(QStringLiteral("spinOutBlack"));
    auto *outWhite   = dlg.findChild<QSpinBox *>(QStringLiteral("spinOutWhite"));
    auto *gammaBox   = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("spinGamma"));
    auto *autoButton = dlg.findChild<QPushButton *>(QStringLiteral("buttonAuto"));
    auto *levelsW    = dlg.findChild<LevelsWidget *>(QStringLiteral("levelsWidget"));

    check(inBlack && inWhite && outBlack && outWhite && gammaBox
          && autoButton && levelsW,
          "直方图控件、五个数值框与「自动」按钮都能按名字找到");
    if (!inBlack || !inWhite || !outBlack || !outWhite || !gammaBox
        || !autoButton || !levelsW)
        return;

    checkEq(inBlack->value(), 0,   "输入黑场初值为 0");
    checkEq(inWhite->value(), 255, "输入白场初值为 255");
    checkEq(outBlack->value(), 0,  "输出黑场初值为 0");
    checkEq(outWhite->value(), 255, "输出白场初值为 255");
    check(std::abs(gammaBox->value() - 1.0) < 1e-9, "灰度系数初值为 1.0");

    // ── 收窄输入白场 ──
    inWhite->setValue(200);
    check(dlg.isChanged(), "改变输入白场后视为已修改");
    check(previews > 0, "参数变化会请求预览（预览链路是通的）");
    checkEq(levelsW->params().inWhite, 200, "数值框的改动写回了直方图控件");

    // ── 效果：白场 200 把亮部拉伸 ──
    //   源图 (128,96,64) 按 (v-0)/200 映射 → (163,122,82)。
    //   关键是三个通道各按自己的输入查表，彩色次序 R>G>B 必须保持。
    const QImage stretched = dlg.apply(src);
    const QRgb st = rawPixel(stretched, 5, 5);
    const QRgb so = rawPixel(src, 5, 5);
    check(qRed(st) > qRed(so), "输入白场 200 提亮了红色通道");
    check(qRed(st) > qGreen(st) && qGreen(st) > qBlue(st),
          "提亮后仍保持 R>G>B 的彩色次序（没有发生通道广播）");
    check(qAlpha(rawPixel(stretched, 0, 0)) == 128, "色阶调整不破坏 alpha");
    check(qRed(rawPixel(stretched, 0, 0)) > 64,
          "半透明像素按非预乘值查表（预乘往返正确，颜色被提亮）");

    // ── gamma：大于 1 让中间调更亮 ──
    gammaBox->setValue(2.0);
    check(std::abs(levelsW->params().gamma - 2.0) < 1e-9,
          "灰度系数写回了直方图控件");
    const QImage brightened = dlg.apply(src);
    check(qRed(rawPixel(brightened, 5, 5)) > qRed(st),
          "gamma 2.0 比线性映射更亮（中间调上移）");

    // ── 黑白场交叉会被界面挡回来 ──
    inBlack->setValue(210);
    check(inWhite->value() > inBlack->value(),
          "输入黑场越过白场时白场被顶到其后（不产生交叉参数）");
    outBlack->setValue(250);
    check(outWhite->value() > outBlack->value(),
          "输出黑场越过白场时同样被挡住");

    // ── 自动：整幅单色时引擎刻意"不调整"（安全兜底）──
    //
    //   makeSource() 整幅只有一个亮度值，两端分位数落在同一个桶里，
    //   computeAutoLevels() 此时返回"不变"而不是一对交叉参数。
    //   这里验证"按钮不越权"，至于自动色阶真正起作用的场景，
    //   由下面那张带噪点的图来覆盖。
    {
        const double gammaBefore = gammaBox->value();
        autoButton->click();

        checkEq(inBlack->value(), 0,
                "整幅单色时「自动」退化为不变（引擎的安全兜底）");
        checkEq(inWhite->value(), 255, "同上，白场保持 255");
        check(std::abs(gammaBox->value() - gammaBefore) < 1e-9,
              "「自动」不改动灰度系数（只接管输入黑白场）");
        checkEq(levelsW->params().outBlack, outBlack->value(),
                "「自动」不改动输出色阶");
    }

    // ── 自动：在"主体居中 + 两端孤立噪点"的图上验证它确实在工作 ──
    {
        LevelsDialog spikyDlg(makeSpikyForAuto());
        auto *rb    = spikyDlg.findChild<QSpinBox *>(QStringLiteral("spinInBlack"));
        auto *rw    = spikyDlg.findChild<QSpinBox *>(QStringLiteral("spinInWhite"));
        auto *rg    = spikyDlg.findChild<QDoubleSpinBox *>(QStringLiteral("spinGamma"));
        auto *rob   = spikyDlg.findChild<QSpinBox *>(QStringLiteral("spinOutBlack"));
        auto *rauto = spikyDlg.findChild<QPushButton *>(QStringLiteral("buttonAuto"));

        check(rb && rw && rg && rob && rauto,
              "带噪点图的对话框控件也能按名字找到");
        if (rb && rw && rg && rob && rauto) {
            // 先手工设好 gamma 与输出黑场，验证「自动」不会把它们清掉 ——
            // 这是本按钮最核心的契约（整份覆盖是典型的"按了自动画面反而变了"）
            rg->setValue(1.8);
            rob->setValue(12);

            rauto->click();

            check(rb->value() > 0,
                  "「自动」按分位数把输入黑场抬离 0（忽略了那个纯黑噪点）");
            check(rw->value() < 255,
                  "「自动」把输入白场压离 255（忽略了那个纯白噪点）");
            check(rw->value() > rb->value(),
                  "「自动」结果满足黑场 < 白场");
            check(std::abs(rg->value() - 1.8) < 1e-9,
                  "「自动」保留用户手工设的 gamma 1.8（不整份覆盖参数）");
            checkEq(rob->value(), 12, "「自动」保留用户设的输出黑场 12");
        }
    }

    // ── 复位 ──
    dlg.resetParams();
    check(!dlg.isChanged(), "复位后回到未修改");
    checkEq(inBlack->value(), 0,   "复位把输入黑场拨回 0");
    checkEq(inWhite->value(), 255, "复位把输入白场拨回 255");
    checkEq(outBlack->value(), 0,  "复位把输出黑场拨回 0");
    checkEq(outWhite->value(), 255, "复位把输出白场拨回 255");
    check(std::abs(gammaBox->value() - 1.0) < 1e-9, "复位把灰度系数拨回 1.0");
    checkPixelSame(dlg.apply(src), src, 5, 5, "复位后 apply 又逐位不变");
}

// ---------------------------------------------------------------------------
//  6. 色阶控件（参数往返、手柄拖动、键盘微调、绘制）
// ---------------------------------------------------------------------------
static void testLevelsWidget()
{
    std::printf("\n[6] 色阶控件\n");

    // ── 参数往返 ──
    {
        LevelsWidget w;
        check(w.isIdentity(), "新建控件的参数是恒等");

        adjust::LevelsParams in;
        in.inBlack  = 20;
        in.inWhite  = 230;
        in.gamma    = 1.2;
        in.outBlack = 10;
        in.outWhite = 240;
        w.setParams(in);

        const adjust::LevelsParams back = w.params();
        checkEq(back.inBlack, 20,  "参数往返：输入黑场");
        checkEq(back.inWhite, 230, "参数往返：输入白场");
        checkEq(back.outBlack, 10, "参数往返：输出黑场");
        checkEq(back.outWhite, 240, "参数往返：输出白场");
        check(std::abs(back.gamma - 1.2) < 1e-9, "参数往返：灰度系数");
        check(!w.isIdentity(), "非恒等参数被识别为已修改");

        w.setParams(adjust::LevelsParams());
        check(w.isIdentity(), "设回默认参数后识别为恒等");
    }

    // ── 拖动中灰手柄（真实鼠标事件）──
    //
    // ★ 位置不硬编码在断言里，而是由控件几何推出 ★
    //   控件 440×182、margin 10 时：
    //     手柄行 y ∈ [121, 132]（x 取 127 即可）
    //     色阶 0..255 映射到 x ∈ [10, 429]
    //     gamma = 1 时中灰落在色阶 128 → x = 10 + round(419*128/255) = 220
    {
        LevelsWidget w;
        w.resize(440, 182);

        int changes = 0;
        QObject::connect(&w, &LevelsWidget::levelsChanged,
                         [&changes]() { ++changes; });

        const int rowY  = 127;
        const int grayX = 220;

        QMouseEvent press(QEvent::MouseButtonPress, QPointF(grayX, rowY),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &press);

        // ★ 自洽性断言 ★
        //   "由 gamma 正查位置"与"由位置反解 gamma"必须互逆：
        //   在正查出来的位置上按下去，gamma 应当基本不变。
        //   两者一旦有一处写错（例如把 1/gamma 当 gamma 用），
        //   手柄会每按一下就跳一格，而这条断言会立刻失败。
        check(std::abs(w.params().gamma - 1.0) < 0.05,
              "在中灰手柄原位按下几乎不改变 gamma（正查与反解互逆）");

        QMouseEvent move(QEvent::MouseMove, QPointF(180, rowY),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &move);

        check(w.params().gamma > 1.0,
              "中灰手柄左移让中间调变亮（gamma > 1）");
        check(w.params().gamma <= 10.0,
              "gamma 被钳制在引擎的合法范围内（不超过 10）");
        checkEq(w.params().inBlack, 0,   "拖动中灰手柄不改动输入黑场");
        checkEq(w.params().inWhite, 255, "拖动中灰手柄不改动输入白场");
        check(changes > 0, "拖动会发出 levelsChanged（预览链路接得上）");
        check(!w.isIdentity(), "拖动后参数不再是恒等");

        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(180, rowY),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &release);
    }

    // ── 键盘微调 ──
    //   没有点过任何手柄时默认操作输入黑场 —— 这让"Tab 进控件后按方向键"
    //   有明确行为，而不是静默什么都不发生。
    {
        LevelsWidget w;
        w.resize(440, 182);

        int changes = 0;
        QObject::connect(&w, &LevelsWidget::levelsChanged,
                         [&changes]() { ++changes; });

        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(&w, &right);
        checkEq(w.params().inBlack, 1, "右方向键把输入黑场 +1");

        QKeyEvent shiftRight(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
        QApplication::sendEvent(&w, &shiftRight);
        checkEq(w.params().inBlack, 11, "Shift + 右方向键步长为 10");

        QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QApplication::sendEvent(&w, &left);
        checkEq(w.params().inBlack, 10, "左方向键把输入黑场 -1");

        check(changes > 0, "键盘微调也会请求预览");
    }

    // ── 绘制（含各种直方图与尺寸）──
    {
        LevelsWidget w;
        w.resize(440, 182);
        check(!w.grab().isNull(), "空直方图下能正常绘制");

        QVector<int> hist(256, 0);
        hist[0] = 500;
        hist[128] = 4000;   // 模拟"大片中间调 + 一堆纯黑"的真实分布
        hist[255] = 900;
        w.setHistogram(hist);
        check(!w.grab().isNull(), "有直方图时能正常绘制");

        // 长度异常必须被挡住（否则 256 项的下标访问会越界）
        w.setHistogram(QVector<int>(8, 3));
        check(!w.grab().isNull(), "直方图长度不是 256 时不崩（有长度校验）");

        // 全零直方图：归一化会除以 0，必须走保护分支
        w.setHistogram(QVector<int>(256, 0));
        check(!w.grab().isNull(), "全零直方图不崩（归一化有除零保护）");

        // ★ 最小尺寸是硬约束 ★
        //   高度压到小于各行之和时手柄行会退化成 0 高、点都点不到，
        //   所以控件拒绝被压小。这里验证这条保护确实生效。
        w.resize(8, 8);
        check(w.width() >= 200 && w.height() >= 182,
              "控件拒绝被压到最小尺寸以下（否则手柄行会退化到点不到）");
        check(!w.grab().isNull(), "最小尺寸下能正常绘制");
    }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  [7] 图像大小对话框
// ---------------------------------------------------------------------------

static void testImageSizeDialog()
{
    std::printf("\n[7] 图像大小对话框\n");

    const QSize original(4000, 3000);
    ImageSizeDialog dlg(original, ResampleMethod::Bicubic, /*layerCount=*/3);

    check(!dlg.isChanged(), "默认参数视为未修改（不会压空命令）");
    check(dlg.newSize() == original, "初值就是原尺寸");

    auto *wBox    = dlg.findChild<QSpinBox *>(QStringLiteral("spinWidth"));
    auto *hBox    = dlg.findChild<QSpinBox *>(QStringLiteral("spinHeight"));
    auto *ratio   = dlg.findChild<QCheckBox *>(QStringLiteral("checkRatio"));
    auto *combo   = dlg.findChild<QComboBox *>(QStringLiteral("comboResample"));
    auto *info    = dlg.findChild<QLabel *>(QStringLiteral("labelInfo"));
    auto *buttons = dlg.findChild<QDialogButtonBox *>();

    check(wBox && hBox && ratio && combo && info && buttons,
          "宽高输入框、长宽比勾选框、重采样下拉、信息面板都能按名字找到");
    if (!wBox || !hBox || !ratio || !combo || !info || !buttons)
        return;

    // ★ "确定"在尺寸未变时必须置灰 ★
    //   尺寸类操作会往撤销栈压入**所有图层的**前后位图，一步可能就是
    //   几百 MB。用户打开对话框看一眼就点确定，绝不该留下这样一条记录。
    auto *okButton = buttons->button(QDialogButtonBox::Ok);
    check(okButton && !okButton->isEnabled(),
          "★ 尺寸未变时「确定」置灰（防止留下几百 MB 的空撤销记录）");

    check(ratio->isChecked(), "长宽比默认锁定（避免手滑把图拉扁）");

    // ── 改宽度，高度按比例跟随 ──
    //   4000×3000 是 4:3，宽度改成 2000 应得高度 1500。
    wBox->setValue(2000);
    checkEq(hBox->value(), 1500, "锁定长宽比时高度自动跟随（4:3）");
    check(dlg.newSize() == QSize(2000, 1500), "newSize() 与控件同步");
    check(dlg.isChanged(), "改尺寸后视为已修改");
    check(okButton->isEnabled(), "尺寸变化后「确定」重新可用");

    // ── 取消锁定后两维互不影响 ──
    ratio->setChecked(false);
    wBox->setValue(1200);
    checkEq(hBox->value(), 1500, "取消锁定后改宽度不再影响高度");
    check(dlg.newSize() == QSize(1200, 1500), "非等比尺寸被正确保留");

    // ── 上限与下限钳制 ──
    wBox->setValue(0);
    checkEq(wBox->value(), 1, "宽度下限钳制到 1 px");
    checkEq(dlg.newSize().width(), 1, "newSize() 也不会出现 0");

    wBox->setValue(999999);
    check(dlg.newSize().width() <= kMaxCanvasDimension,
          "宽度上限被钳制在 kMaxCanvasDimension 内（不会去分配一个必然失败的位图）");

    // ── 放大时的风险提示 ──
    wBox->setValue(8000);
    hBox->setValue(6000);
    check(info->text().contains(QStringLiteral("放大")),
          "放大时明确提示重采样无法凭空产生细节");

    // ── 复位 ──
    auto *resetButton = dlg.findChild<QPushButton *>(QStringLiteral("buttonReset"));
    check(resetButton != nullptr, "「复位」按钮能按 objectName 找到");
    if (resetButton) {
        resetButton->click();
        check(dlg.newSize() == original, "复位后回到原尺寸");
        check(!dlg.isChanged(), "复位后视为未修改");
        check(!okButton->isEnabled(), "复位后「确定」重新置灰");
    }

    // ── 单位：像素 / 百分比 ──
    //
    //  ★ 这一组守的是"单位只是显示方式"★
    //    单位、相对模式、长宽比锁定都是**视图修饰**：切换它们不该动
    //    目标尺寸一个像素。最容易写错的实现是"切换时顺手把数字换算
    //    一下塞回去" —— 那样用户只是拨了个显示开关，图却被真的改了，
    //    撤销栈里还会多出一条他自己都不明白怎么来的记录。
    {
        auto *unit = dlg.findChild<QComboBox *>(QStringLiteral("comboUnit"));
        check(unit != nullptr, "单位下拉框能按 objectName 找到");

        if (unit) {
            const int pxIndex      = unit->findData(int(SizeChangeDialogBase::Unit::Pixels));
            const int percentIndex = unit->findData(int(SizeChangeDialogBase::Unit::Percent));
            check(pxIndex >= 0 && percentIndex >= 0,
                  "★ 下拉框每一项都携带自己的 Unit 枚举值（不靠下标对号）");

            checkEq(int(dlg.unit()), int(SizeChangeDialogBase::Unit::Pixels),
                    "默认单位是像素");
            check(wBox->suffix() == QStringLiteral(" px"), "像素模式的后缀是 px");

            // ── 切到百分比：尺寸不动 ──
            ratio->setChecked(true);   // 上面解除过，这里重新锁上
            unit->setCurrentIndex(percentIndex);

            checkEq(int(dlg.unit()), int(SizeChangeDialogBase::Unit::Percent),
                    "切换后 unit() 跟随");
            check(dlg.newSize() == original,
                  "★ 切换单位不改变目标尺寸（单位只是显示方式）");
            check(!dlg.isChanged(), "切换单位后仍视为未修改");
            check(!okButton->isEnabled(),
                  "★ 切换单位不会让「确定」变可用（否则会压入一条几百 MB 的空记录）");
            checkEq(wBox->value(), 100, "原尺寸在百分比下显示 100");
            checkEq(hBox->value(), 100, "高度同样显示 100");
            check(wBox->suffix() == QStringLiteral(" %"), "百分比模式的后缀是 %");

            // ── 百分比与长宽比联动 ──
            wBox->setValue(50);
            check(dlg.newSize() == QSize(2000, 1500),
                  "★ 百分比 + 锁定长宽比：50% 得到 2000×1500");
            checkEq(hBox->value(), 50, "高度百分比自动跟随");

            // ── 上限按**各维度分别**计算 ──
            //   4000 宽的图上 500% 就到 20000 了，3000 高的图上还能到 666%。
            //   若两个维度共用一个区间，宽或高必有一边永远拨不到上限。
            checkEq(wBox->maximum(), 500, "宽度上限 = 20000/4000 = 500%");
            checkEq(hBox->maximum(), 666, "★ 高度上限 = 20000/3000 = 666%（两维区间独立）");
            checkEq(wBox->minimum(), 1, "绝对模式的下限是 1%");

            wBox->setValue(9999);
            checkEq(wBox->value(), 500,
                    "★ 输入超过上限被 range 挡住（不会出现'输入后立刻被改写'的怪相）");
            checkEq(dlg.newSize().width(), 20000, "500% 恰好是上限 20000 像素");

            // ── 回写稳定：33% 不能显示成 34% ──
            wBox->setValue(33);
            checkEq(dlg.newSize().width(), 1320, "4000 的 33% = 1320 px");
            checkEq(wBox->value(), 33, "★ 百分比回写稳定（33% 不会漂成 34%）");
            checkEq(hBox->value(), 33, "等比时高度百分比同步");

            // ── 解开长宽比后两维独立，信息面板要分维报告 ──
            ratio->setChecked(false);
            hBox->setValue(25);
            check(dlg.newSize() == QSize(1320, 750),
                  "解除锁定后改为 1320×750");
            checkEq(hBox->value(), 25, "高度百分比回写稳定");

            //  信息面板原来只报"最长边"的比例，会出现"输入框写 25%、
            //  面板写 33.0%"这种"两个数各自都对但看着像算错了"的组合。
            {
                const QString infoStr = info->text();
                check(infoStr.contains(QStringLiteral("宽 33.0%"))
                      && infoStr.contains(QStringLiteral("高 25.0%")),
                      "★ 两轴比例不同时信息面板分维写出（不再只报最长边）");
            }

            // ── 复位：百分比下回到 100% ──
            if (resetButton) {
                resetButton->click();
                checkEq(wBox->value(), 100, "复位后百分比回到 100");
                checkEq(hBox->value(), 100, "高度也回到 100");
                check(!dlg.isChanged(), "复位后视为未修改");

                // 撤销复位的影响，继续后面的用例
                wBox->setValue(50);
            }

            // ── 切回像素：显示目标像素值，后缀跟着走 ──
            unit->setCurrentIndex(pxIndex);
            check(wBox->suffix() == QStringLiteral(" px"), "切回像素后后缀跟着变");
            checkEq(wBox->value(), 2000, "切回像素后显示 2000（百分之 50 的目标）");
            checkEq(dlg.newSize().width(), 2000,
                    "★ 来回切换单位一次，目标尺寸始终没变");
            check(!wBox->text().endsWith(QStringLiteral("%")),
                  "像素模式下输入框里不应残留百分号");

            resetButton->click();   // 收尾：回到原尺寸
        }
    }

    // ── 重采样方式 ──
    check(dlg.method() == ResampleMethod::Bicubic, "重采样默认两次立方");

    const int lanczosIndex = combo->findData(int(ResampleMethod::Lanczos));
    check(lanczosIndex >= 0, "下拉框里有 Lanczos 项");
    if (lanczosIndex >= 0) {
        combo->setCurrentIndex(lanczosIndex);
        check(dlg.method() == ResampleMethod::Lanczos, "切换下拉框后 method() 跟随");
    }

    // 每一项都要能被解析回正确的枚举 —— 只测一项会漏掉"顺序错位"这类错误
    checkEq(combo->count(), 4, "重采样下拉框恰好四项");
    int parsedOk = 0;
    for (int i = 0; i < combo->count(); ++i) {
        combo->setCurrentIndex(i);
        if (int(dlg.method()) == combo->itemData(i).toInt())
            ++parsedOk;
    }
    checkEq(parsedOk, combo->count(), "★ 每一项都能解析回它自己携带的枚举值");

    // ── 信息面板的内容 ──
    check(info->text().contains(QStringLiteral("4000"))
          && info->text().contains(QStringLiteral("3000")),
          "信息面板显示当前尺寸");
    check(info->text().contains(QStringLiteral("MP")), "信息面板显示百万像素量级");

    // ── 超大尺寸的内存提醒 ──
    //   4 图层 × 20000×20000 × 4 字节 = 6.4GB，远超提醒阈值。
    //   这条路径平时点不到，但它是"用户点了确定之后程序卡死"的唯一预警。
    {
        ImageSizeDialog huge(QSize(20000, 20000), ResampleMethod::Bicubic, 4);
        auto *hugeInfo = huge.findChild<QLabel *>(QStringLiteral("labelInfo"));
        check(hugeInfo && hugeInfo->text().contains(QStringLiteral("内存提醒")),
              "超大尺寸时给出内存提醒");
        check(!huge.grab().isNull(), "超大尺寸下对话框仍能正常绘制");
    }

    check(!dlg.grab().isNull(), "对话框能完整绘制（不崩）");
}

// ---------------------------------------------------------------------------
//  [8] 画布大小对话框
// ---------------------------------------------------------------------------

static void testCanvasSizeDialog()
{
    std::printf("\n[8] 画布大小对话框\n");

    const QSize original(200, 200);
    // 内容放在右下角：锚点选左上的话，按纯锚点算会被裁掉 ——
    // 正好用来验证智能适配确实介入了。
    const QRect content(150, 150, 40, 40);

    CanvasSizeDialog dlg(original, content, Anchor::Center);

    check(!dlg.isChanged(), "默认参数视为未修改");
    check(dlg.newSize() == original, "初值就是原尺寸");
    checkEq(int(dlg.anchor()), int(Anchor::Center), "默认锚点居中");
    checkEq(dlg.fillColor().alpha(), 0, "默认扩展颜色为透明");

    auto *wBox        = dlg.findChild<QSpinBox *>(QStringLiteral("spinWidth"));
    auto *hBox        = dlg.findChild<QSpinBox *>(QStringLiteral("spinHeight"));
    auto *rel         = dlg.findChild<QCheckBox *>(QStringLiteral("checkRelative"));
    auto *info        = dlg.findChild<QLabel *>(QStringLiteral("labelInfo"));
    auto *colorButton = dlg.findChild<QToolButton *>(QStringLiteral("buttonFillColor"));
    auto *preview     = dlg.findChild<QWidget *>(QStringLiteral("canvasPreview"));
    auto *buttons     = dlg.findChild<QDialogButtonBox *>();

    check(wBox && hBox && rel && info && colorButton && preview && buttons,
          "尺寸框、相对勾选框、扩展颜色按钮、示意图都能按名字找到");
    if (!wBox || !hBox || !rel || !info || !colorButton || !preview || !buttons)
        return;

    // ★ 画布大小不该有"保持长宽比" ★
    //   拉伸画布不改变内容，比例在这里没有意义。多给一个勾选框只会让
    //   用户以为"改画布会拉伸画面"。
    check(!dlg.findChild<QCheckBox *>(QStringLiteral("checkRatio")),
          "画布大小没有「保持长宽比」（拉伸画布不改变内容，比例无意义）");

    auto *okButton = buttons->button(QDialogButtonBox::Ok);
    check(okButton && !okButton->isEnabled(), "尺寸未变时「确定」置灰");

    // ── 九宫格锚点 ──
    const char *anchorNames[9] = {
        "anchorButton0", "anchorButton1", "anchorButton2",
        "anchorButton3", "anchorButton4", "anchorButton5",
        "anchorButton6", "anchorButton7", "anchorButton8"
    };
    int missing = 0;
    for (int i = 0; i < 9; ++i)
        if (!dlg.findChild<QToolButton *>(QString::fromLatin1(anchorNames[i])))
            ++missing;
    checkEq(missing, 0, "九个锚点按钮都能按名字找到");

    auto *brButton = dlg.findChild<QToolButton *>(QStringLiteral("anchorButton8"));
    if (brButton) {
        brButton->click();
        checkEq(int(dlg.anchor()), int(Anchor::BottomRight),
                "点击右下角按钮后 anchor() 跟随");
    }

    // ── 相对模式：输入框表示增量 ──
    rel->setChecked(true);
    checkEq(wBox->value(), 0, "切到相对模式后宽度输入框显示增量 0");
    checkEq(dlg.newSize().width(), 200, "相对模式下目标尺寸仍是原尺寸");

    wBox->setValue(50);
    checkEq(dlg.newSize().width(), 250, "相对模式下 +50 表示新宽度 = 原宽 + 50");
    checkEq(wBox->value(), 50, "增量被正确回写");

    rel->setChecked(false);
    checkEq(wBox->value(), 250, "切回绝对模式后输入框显示目标值");
    check(dlg.newSize() == QSize(250, 200),
          "★ 切换相对/绝对不改变目标尺寸，只改变显示方式");

    // ── 相对模式下负增量过大 → 目标钳到 1 ──
    rel->setChecked(true);
    wBox->setValue(-500);
    checkEq(dlg.newSize().width(), 1, "负增量过大时目标宽度钳到 1 px");
    checkEq(wBox->value(), -199, "★ 增量被回写成真实生效的值（200 → 1 即 -199）");
    rel->setChecked(false);
    checkEq(wBox->value(), 1, "切回绝对模式后显示钳制后的目标值");

    // ── 缩小 + 智能适配：与引擎公式对账 ──
    auto *tlButton = dlg.findChild<QToolButton *>(QStringLiteral("anchorButton0"));
    if (tlButton)
        tlButton->click();
    checkEq(int(dlg.anchor()), int(Anchor::TopLeft), "锚点切到左上");

    wBox->setValue(100);
    hBox->setValue(100);
    check(dlg.newSize() == QSize(100, 100), "尺寸设置为 100×100");

    const QSize small(100, 100);
    const QPoint expected =
        canvasResizeOffset(content, original, small, Anchor::TopLeft, true);

    check(dlg.contentOffset() == expected,
          "★★ 对话框的偏移与 Document 执行时用的是同一个函数（预览不会说谎）");
    check(dlg.contentOffset() != anchorOffset(Anchor::TopLeft, original, small),
          "对照：与纯锚点结果不同，说明智能适配确实介入了");
    check(info->text().contains(QStringLiteral("智能适配")),
          "★ 信息面板明确告知内容被自动平移了");
    check(!info->text().contains(QStringLiteral("裁切")),
          "内容能救回来时不报裁切警告（狼来了会让人忽略真正的警告）");

    // ── 真的裁切时必须报出来 ──
    wBox->setValue(30);
    hBox->setValue(30);
    check(dlg.newSize() == QSize(30, 30), "尺寸设置为 30×30");
    check(info->text().contains(QStringLiteral("裁切")),
          "★ 内容装不下时明确报出裁切量");

    // ── 复位 ──
    auto *resetButton = dlg.findChild<QPushButton *>(QStringLiteral("buttonReset"));
    check(resetButton != nullptr, "「复位」按钮能按 objectName 找到");
    if (resetButton) {
        resetButton->click();
        check(dlg.newSize() == original, "复位后回到原尺寸");
        check(!dlg.isChanged(), "复位后视为未修改");
        check(!okButton->isEnabled(), "复位后「确定」重新置灰");
    }

    // ── 单位：像素 / 百分比，并与「相对」正交组合 ──
    //
    //  画布大小是唯一同时具备"相对模式"与"单位"的对话框，也就是四种组合
    //  全都走得通的那一个。最值得盯的是**相对 + 百分比**：
    //  数字 50 表示"在原有基础上加大五成"，而不是"目标 50%"。
    {
        auto *unit = dlg.findChild<QComboBox *>(QStringLiteral("comboUnit"));
        check(unit != nullptr, "画布大小同样有单位下拉框");

        if (unit) {
            const int pxIndex      = unit->findData(int(SizeChangeDialogBase::Unit::Pixels));
            const int percentIndex = unit->findData(int(SizeChangeDialogBase::Unit::Percent));
            check(pxIndex >= 0 && percentIndex >= 0,
                  "单位下拉框每一项都携带自己的 Unit 枚举值");

            unit->setCurrentIndex(percentIndex);
            checkEq(wBox->value(), 100, "200×200 的画布在百分比下显示 100");
            check(dlg.newSize() == original, "★ 切到百分比不改目标尺寸");
            checkEq(wBox->maximum(), 10000, "上限 = 20000/200 = 10000%");
            checkEq(wBox->minimum(), 1, "绝对模式下限 1%");

            wBox->setValue(150);
            checkEq(dlg.newSize().width(), 300, "绝对 150% → 300 px");
            checkEq(wBox->value(), 150, "百分比回写稳定");

            // ── 相对 + 百分比 ──
            rel->setChecked(true);
            checkEq(dlg.newSize().width(), 300, "★ 勾「相对」不改变目标尺寸");
            checkEq(wBox->value(), 50,
                    "★ 同一个目标（300 px / 原 200）在相对模式下显示 +50");
            checkEq(wBox->minimum(), -99,
                    "★ 相对模式的下限 = 1% - 100 = -99（目标至少要 1%）");

            wBox->setValue(-50);
            checkEq(dlg.newSize().width(), 100, "相对 -50% → 100 px（减半）");
            checkEq(wBox->value(), -50, "增量百分比回写稳定");

            wBox->setValue(0);
            checkEq(dlg.newSize().width(), 200, "相对 0% → 尺寸不变（这就是「相对」的默认值）");

            wBox->setValue(-999);
            checkEq(wBox->value(), -99, "★ 拨不过下限（不会产生 0 像素的画布）");
            checkEq(dlg.newSize().width(), 2,
                    "★ 1% 的 200 px 就是 2 px —— 百分比的粒度到此为止，要 1 px 得用像素单位");

            check(okButton->isEnabled(), "尺寸变化后「确定」重新可用");

            // ── 切回像素 ──
            rel->setChecked(false);
            unit->setCurrentIndex(pxIndex);
            checkEq(wBox->value(), dlg.newSize().width(),
                    "★ 切回像素后显示目标像素值（百分比 ⇄ 像素来回一次不丢尺寸）");
            check(wBox->suffix() == QStringLiteral(" px"), "后缀跟着单位走");

            if (resetButton) {
                resetButton->click();
                checkEq(dlg.newSize().width(), 200, "复位回到原尺寸（收尾，不影响后续用例）");
            }
        }
    }

    // ── 内容为空的画布（全透明）──
    {
        CanvasSizeDialog emptyDoc(QSize(100, 100), QRect(), Anchor::Center);
        auto *emptyInfo = emptyDoc.findChild<QLabel *>(QStringLiteral("labelInfo"));
        check(emptyInfo && emptyInfo->text().contains(QStringLiteral("没有可见内容")),
              "全透明画布明确说明不会有东西被裁掉");
        check(!emptyDoc.grab().isNull(), "空内容时示意图也能绘制（不崩）");
    }

    // ── 放大：走过去透明底棋盘格的绘制分支 ──
    {
        CanvasSizeDialog grow(QSize(60, 60), QRect(10, 10, 40, 40), Anchor::Center);
        auto *gw = grow.findChild<QSpinBox *>(QStringLiteral("spinWidth"));
        auto *gh = grow.findChild<QSpinBox *>(QStringLiteral("spinHeight"));
        auto *gInfo = grow.findChild<QLabel *>(QStringLiteral("labelInfo"));
        if (gw && gh) {
            gw->setValue(180);
            gh->setValue(140);
            check(grow.newSize() == QSize(180, 140), "放大到 180×140");
            check(gInfo && gInfo->text().contains(QStringLiteral("扩大")),
                  "信息面板报告画布扩大了");
            check(!gInfo->text().contains(QStringLiteral("裁切")),
                  "放大时不可能发生裁切");
            check(!grow.grab().isNull(), "放大（透明底棋盘格）时示意图能绘制");
        }
    }

    check(!dlg.grab().isNull(), "对话框（含示意图）能完整绘制");

    // ── ★★ 渲染断言：示意图到底画了什么 ★★ ──
    //
    //  上面那些断言全是"关于数学"的（contentOffset() 与引擎一致、文案对不对）。
    //  但用户看到的是**像素**。示意图自己有一条独立于引擎的绘制管线，
    //  其中任何一步用错坐标，数学全对而画面全错 —— 而且不崩、不报错。
    //
    //  实测踩过：内容色块用了 **图像坐标**的矩形去建路径（少了到控件坐标的
    //  映射），于是一幅 4000×3000 的照片会把"内容"铺满整个示意图、
    //  "被裁内容"红块溢出到画布之外。虚线却是对的（它用了映射值），
    //  所以画面看起来"有点像对的"。只有量像素才能发现。
    //
    //  做法与 view_test 的"渲染占位"同源：纯色可判定 + grab() + 量包围盒。
    {
        // 真实场景：内容是整幅旧画布（不透明照片），左上锚点放大 30%。
        // 放大不可能裁掉任何内容 → 画面上不应出现任何"被裁内容"红块。
        const QSize base(400, 300);
        CanvasSizeDialog shot(base, QRect(QPoint(0, 0), base), Anchor::TopLeft);

        auto *sw = shot.findChild<QSpinBox *>(QStringLiteral("spinWidth"));
        auto *sh = shot.findChild<QSpinBox *>(QStringLiteral("spinHeight"));
        auto *sp = shot.findChild<QWidget *>(QStringLiteral("canvasPreview"));

        check(sw && sh && sp, "渲染断言所需的控件都能按名字找到");

        if (sw && sh && sp) {
            sw->setValue(520);
            sh->setValue(390);
            check(shot.newSize() == QSize(520, 390), "放大到 520×390");

            // CanvasPreviewWidget 不重写 resizeEvent，所以未 show() 时
            // resize() 直接改几何即可，绘制用的是绘制时刻的 width()/height()。
            sp->resize(400, 168);

            const QImage img =
                sp->grab().toImage().convertToFormat(QImage::Format_RGB32);

            int deepRed = 0;    // "被裁内容"的深红
            int blue = 0;       // "内容"的蓝
            int dash = 0;       // "原画布范围"的虚线（(90,90,90)）
            int bx0 = img.width(), bx1 = -1;
            int by0 = img.height(), by1 = -1;
            int kx0 = img.width(), kx1 = -1;
            int ky0 = img.height(), ky1 = -1;

            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    const QRgb c = rawPixel(img, x, y);
                    // 深红：(200,40,40,150) 合成到面板灰上约 (216,122,122)
                    if (qRed(c) > 180 && qGreen(c) < 150 && qBlue(c) < 150)
                        ++deepRed;
                    // 蓝：内容色 (42,110,180,120) 合成后 B 明显大于 R
                    if (qBlue(c) > qRed(c) + 20) {
                        ++blue;
                        bx0 = qMin(bx0, x); bx1 = qMax(bx1, x);
                        by0 = qMin(by0, y); by1 = qMax(by1, y);
                    }
                    // 虚线：中性灰 (90,90,90)，三通道几乎相等
                    if (std::abs(qRed(c) - 90) < 20
                        && std::abs(qGreen(c) - 90) < 20
                        && std::abs(qBlue(c) - 90) < 20)
                        ++dash;
                    // 新画布边框（palette Text，深色）；内容描边是深蓝，
                    // G=80 不会被这里误收
                    if (qRed(c) < 80 && qGreen(c) < 80 && qBlue(c) < 80) {
                        kx0 = qMin(kx0, x); kx1 = qMax(kx1, x);
                        ky0 = qMin(ky0, y); ky1 = qMax(ky1, y);
                    }
                }
            }

            checkEq(deepRed, 0,
                    "★ 放大不可能裁内容 —— 示意图里不应出现任何深红块");
            check(blue > 0, "示意图里应当画出了内容色块");
            check(kx1 > kx0, "示意图里应当画出了新画布边框");

            // ★ 虚线不能被内容色块盖住 ★
            //   本用例里内容包围盒恰好等于整幅旧画布，虚线与内容块边界重合。
            //   若先画虚线再画内容，整条虚线会被蓝色实心块埋掉 ——
            //   图例写着"虚线 = 原画布范围"，图上却一条都找不到。
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "可见虚线像素 %d 个", dash);
                check(dash > 20,
                      "★ 虚线（原画布范围）不被内容色块遮挡", buf);
            }

            if (blue > 0 && kx1 > kx0) {
                // 内容 = 旧画布 = 新画布的左上 400/520 ≈ 77%。
                // 若少了坐标映射，内容会铺满整块画布 → 右边距几乎为 0。
                const int gapRight  = kx1 - bx1;
                const int gapBottom = ky1 - by1;

                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "内容右边缘 %d / 画布右边缘 %d（间距 %d px）",
                              bx1, kx1, gapRight);
                check(gapRight > 20,
                      "★★ 内容只占「旧画布」范围，新画布右侧应留出空白", buf);

                std::snprintf(buf, sizeof(buf),
                              "内容下边缘 %d / 画布下边缘 %d（间距 %d px）",
                              by1, ky1, gapBottom);
                check(gapBottom > 20,
                      "★★ 内容只占「旧画布」范围，新画布下方应留出空白", buf);
            }
        }
    }
}

// ===========================================================================
//  [9] 任意角度旋转对话框
// ===========================================================================
//
//  这个对话框有两条"看起来像控件坏了、其实是正确行为"的规则，
//  最容易在重构中被改掉，所以专门钉住：
//
//    · 0° 与 90° 倍数时**填充色下拉必须置灰** —— 那条路径走整数坐标
//      重排，画布与内容等大，压根没有需要填充的区域。不置灰的话，
//      用户选了"黑色"却发现没变黑，会以为功能坏了。
//    · 复位必须把滑块与数字框一起归零，并且 isChanged() 要回到 false
//      —— 否则"打开就点确定"也会在历史面板里留下一条空记录。

static void testRotateImageDialog()
{
    std::printf("\n[9] 任意角度旋转对话框\n");

    const QSize docSize(400, 300);
    const QColor bgColor(0, 128, 255);      // 与"白色"选项刻意区分开

    // 预览源：apply() 只要求是一张 QImage。尺寸刻意与文档不同 ——
    // 真实的预览基准图就是降采样后的合成图。
    QImage src(200, 150, QImage::Format_ARGB32_Premultiplied);
    src.fill(qRgba(200, 60, 60, 255));

    RotateImageDialog dlg(docSize, src, ResampleMethod::Bicubic, bgColor);

    // ── 初值 ──
    checkEq(int(qRound(dlg.angle() * 10.0)), 0, "初始角度为 0");
    check(!dlg.isChanged(), "未改动时 isChanged() 为 false（确定按钮置灰）");
    check(dlg.newCanvasSize() == docSize, "角度为 0 时画布尺寸不变");

    check(dlg.angleSlider() && dlg.angleSpinBox()
              && dlg.fillComboBox() && dlg.infoLabel(),
          "角度滑块 / 数字框 / 填充色 / 信息面板都已创建");

    check(!dlg.fillComboBox()->isEnabled(),
          "★ 0°（90° 的整数倍）时填充色下拉置灰 —— 选了也不会生效");

    // ── 滑块 → 数字框 ──
    dlg.angleSlider()->setValue(300);                       // 30.0°
    checkEq(int(qRound(dlg.angle() * 10.0)), 300, "拖动滑块后角度跟上");
    check(std::fabs(dlg.angleSpinBox()->value() - 30.0) < 1e-6,
          "滑块的值同步到了数字框");
    check(dlg.isChanged(), "★ 角度非 0 后 isChanged() 变为 true（确定按钮点亮）");
    check(dlg.fillComboBox()->isEnabled(), "★ 任意角度时填充色下拉启用");

    // ── 数字框 → 滑块 ──
    dlg.angleSpinBox()->setValue(-45.0);
    checkEq(dlg.angleSlider()->value(), -450, "数字框的值同步到了滑块");
    checkEq(int(qRound(dlg.angle() * 10.0)), -450, "角度取数字框的值（-45.0°）");

    // ── 新画布尺寸与引擎同源 ──
    {
        const QSize expect = rotatedBoundsSize(docSize, -45.0);
        check(dlg.newCanvasSize() == expect,
              "★ 对话框报的新画布尺寸与引擎 rotatedBoundsSize() 完全一致");
        check(expect.width() > docSize.width() && expect.height() > docSize.height(),
              "45° 旋转后画布确实变大了");
    }

    // ── 信息面板 ──
    {
        const QString info = dlg.infoLabel()->text();
        check(info.contains(QStringLiteral("旋转后画布")),
              "信息面板报告了旋转后的画布尺寸");
        check(info.contains(QStringLiteral("填充")),
              "信息面板说明了四角需要填充的面积");
    }

    // ── apply()：尺寸与引擎一致 ──
    {
        const QImage out = dlg.apply(src);
        check(!out.isNull(), "apply() 返回了结果");
        check(out.size() == rotatedBoundsSize(src.size(), -45.0),
              "★ apply() 的结果尺寸 = 源的包围盒（预览与提交同一份算法）");
    }

    // ── 90° 时填充色重新置灰 ──
    dlg.angleSpinBox()->setValue(90.0);
    check(!dlg.fillComboBox()->isEnabled(),
          "★ 90° 时填充色下拉重新置灰（无损路径没有空像素可填）");
    check(dlg.newCanvasSize() == QSize(docSize.height(), docSize.width()),
          "90° 时新画布宽高互换（不扩大）");
    check(dlg.apply(src) == rotateQuarterTurns(src, 1),
          "★ 90° 时 apply() 与无损重排逐像素相同");

    // ── 填充色的四个选项 ──
    {
        QComboBox *combo = dlg.fillComboBox();
        checkEq(combo->count(), 4, "填充色有四个选项（透明 / 背景色 / 白 / 黑）");

        combo->setCurrentIndex(0);
        checkEq(dlg.fillColor().alpha(), 0, "「透明」解析为 alpha = 0");

        combo->setCurrentIndex(1);
        check(dlg.fillColor() == bgColor, "「背景色」解析为构造时传入的底色");

        combo->setCurrentIndex(2);
        check(dlg.fillColor() == QColor(Qt::white), "「白色」解析为白");

        combo->setCurrentIndex(3);
        check(dlg.fillColor() == QColor(Qt::black), "「黑色」解析为黑");
    }

    // ── 填充色真的进了 apply() 的结果 ──
    //
    //  ★ 只看 fillColor() 的返回值等于在测"我自己设的下拉框" ★
    //    这里去量旋转结果四角的**实际像素**，才算测到"用户看到的颜色"。
    {
        dlg.fillComboBox()->setCurrentIndex(3);             // 黑
        dlg.angleSpinBox()->setValue(30.0);

        const QImage out = dlg.apply(src);
        const QRgb corner = rawPixel(out, 0, 0);
        check(qRed(corner) < 40 && qGreen(corner) < 40 && qBlue(corner) < 40
                  && qAlpha(corner) == 255,
              "★ 选「黑色」后四角真的是不透明的黑");

        dlg.fillComboBox()->setCurrentIndex(0);             // 透明
        const QImage t = dlg.apply(src);
        checkEq(qAlpha(rawPixel(t, 0, 0)), 0, "改回「透明」后四角又变透明");
    }

    // ── 复位 ──
    {
        dlg.angleSpinBox()->setValue(30.0);
        check(dlg.isChanged(), "复位前处于已改动态");

        dlg.resetParams();

        checkEq(int(qRound(dlg.angle() * 10.0)), 0, "★ 复位把角度归零");
        checkEq(dlg.angleSlider()->value(), 0, "复位同时把滑块归零");
        check(dlg.fillColor() == QColor(Qt::transparent), "复位把填充色改回透明");
        check(!dlg.isChanged(), "★ 复位后 isChanged() 回到 false");
        check(dlg.newCanvasSize() == docSize, "复位后画布尺寸回到原值");
    }

    // ── 绘制不崩 ──
    check(!dlg.grab().isNull(), "对话框能完整绘制");
}

int main(int argc, char **argv)
{
    // ★ 必须在 QApplication 构造前设置 ★
    //   offscreen 插件让 Qt 在没有显示器的环境下也能创建窗口、
    //   计算布局、并把绘制内容渲染到离屏缓冲。
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 调整对话框 离屏冒烟测试\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    testBrightnessContrast();
    testHueSaturation();
    testCurvesDialog();
    testCurveEditor();
    testLevelsDialog();
    testLevelsWidget();
    testImageSizeDialog();
    testCanvasSizeDialog();
    testRotateImageDialog();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
