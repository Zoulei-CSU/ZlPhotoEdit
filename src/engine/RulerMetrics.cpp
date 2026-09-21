#include "RulerMetrics.h"

#include <QtMath>

#include <cmath>

namespace zl {
namespace rulermetrics {

// ★ 显式的单位表 ★
//   顺序 = 菜单顺序 = QSettings 里存的下标。它是一份**对外协议**：
//   要在中间插入新单位，改这里一处即可，存档位置不会跟着漂。
//   （用 static_cast 就没有这层保护 —— 见头文件里的说明。）
namespace {
constexpr Unit kUnitOrder[kUnitCount] = {
    Unit::Pixel,
    Unit::Centimeter,
    Unit::Millimeter,
    Unit::Inch,
    Unit::Point,
};
} // namespace

Unit unitAt(int index)
{
    if (index < 0 || index >= kUnitCount)
        return Unit::Pixel;
    return kUnitOrder[index];
}

int unitIndex(Unit unit)
{
    for (int i = 0; i < kUnitCount; ++i) {
        if (kUnitOrder[i] == unit)
            return i;
    }
    return 0;
}

qreal pixelsPerUnit(Unit unit, int dpi)
{
    // ★ 兜底 96 而不是 0 ★
    //   元数据缺失（打开的文件没写 DPI）时给一个非零默认值，
    //   否则下面每个除法都会放大到 inf/NaN，标尺直接画不出刻度。
    const qreal d = (dpi > 0) ? qreal(dpi) : 96.0;

    switch (unit) {
    case Unit::Pixel:      return 1.0;
    case Unit::Inch:       return d;
    case Unit::Centimeter: return d / 2.54;
    case Unit::Millimeter: return d / 25.4;
    case Unit::Point:      return d / 72.0;
    }
    return 1.0;
}

qreal niceStep(qreal minStep)
{
    if (!(minStep > 0.0) || !std::isfinite(minStep))
        return 1.0;

    // 把 minStep 写成 m × 10^e（m ∈ [1, 10)），再把 m 上取整到 1 / 2 / 5 / 10。
    const qreal e = std::pow(10.0, std::floor(std::log10(minStep)));
    const qreal m = minStep / e;

    qreal nice;
    if (m <= 1.0)      nice = 1.0;
    else if (m <= 2.0) nice = 2.0;
    else if (m <= 5.0) nice = 5.0;
    else               nice = 10.0;

    return nice * e;
}

Ticks chooseTicks(qreal zoom, Unit unit, int dpi, qreal minLabelSpacingPx)
{
    Ticks t;

    const qreal ppu = pixelsPerUnit(unit, dpi);
    const qreal z   = (zoom > 0.0) ? zoom : 1.0;

    // 屏幕上 1 个单位占多少像素。为 0 / 非有限时退回默认刻度，
    // 让标尺至少还是"一条有刻度的尺子"。
    const qreal unitPx = ppu * z;
    if (!(unitPx > 0.0) || !std::isfinite(unitPx)) {
        t.major = 1.0;
        t.minor = 0.2;
        return t;
    }

    t.major = niceStep(minLabelSpacingPx / unitPx);
    t.minor = t.major / 5.0;
    return t;
}

QString formatLabel(qreal valueInUnits, Unit unit, qreal major)
{
    // ★ "-0" 归一化 ★
    //   图像被平移到"0 在标尺右侧"时，第 0 格左边那格算出来是 -0.0，
    //   QString::number 会老老实实输出 "-0" / "-0.0"，
    //   看起来像标尺算错了（其实是同一件事的另一种写法）。
    if (qFuzzyIsNull(valueInUnits))
        valueInUnits = 0.0;

    if (unit == Unit::Pixel)
        return QString::number(qRound(valueInUnits));

    // 小数位数 = "把主刻度写成一个整数"所需的最少位数。
    //
    //   ★ 不能按"步长 < 1 就给 1 位"分档 ★
    //     英寸常有的 1/4 档（0.25）按分档只给 1 位 → 1.25 被写成 "1.3"，
    //     图上读出来就是错的，而且看起来完全正常。
    //   实测档位：0.25 → 2 位；0.5 / 0.2 / 0.1 → 1 位；0.05 / 0.02 / 0.01 → 2 位。
    int decimals = 0;
    for (; decimals < 6; ++decimals) {
        const qreal scaled = major * std::pow(10.0, decimals);
        if (std::fabs(scaled - std::round(scaled)) < 1e-6)
            break;
    }

    return QString::number(valueInUnits, 'f', decimals);
}

} // namespace rulermetrics
} // namespace zl
