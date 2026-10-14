#include "engine/Curves.h"

#include <QObject>
#include <QVector>
#include <QtMath>
#include <algorithm>

namespace zl {

// ===========================================================================
//  通道名
// ===========================================================================

QString curveChannelName(CurveChannel ch)
{
    switch (ch) {
    case CurveChannel::Rgb:   return QObject::tr("RGB");
    case CurveChannel::Red:   return QObject::tr("Red");
    case CurveChannel::Green: return QObject::tr("Green");
    case CurveChannel::Blue:  return QObject::tr("Blue");
    case CurveChannel::Count: break;
    }
    return QObject::tr("RGB");
}

// ===========================================================================
//  Curve
// ===========================================================================

Curve::Curve()
{
    // 恒等曲线：从左下到右上的一条对角线
    m_points = { CurvePoint(0.0, 0.0), CurvePoint(1.0, 1.0) };
}

Curve::Curve(const QList<CurvePoint> &points)
    : m_points(points)
{
    normalize();
}

void Curve::setPoints(const QList<CurvePoint> &points)
{
    m_points = points;
    normalize();
}

void Curve::normalize()
{
    // ── 1. 按 x 排序 ──
    // 用户拖拽点的时候可能把顺序打乱（尤其是从别处导入的点列表），
    // 样条算法要求 x 严格递增，所以这里必须先整理。
    std::sort(m_points.begin(), m_points.end(),
              [](const CurvePoint &a, const CurvePoint &b) { return a.x < b.x; });

    // ── 2. 合并 x 过于接近的点 ──
    // x 相同会导致差商计算时分母为 0。用 1e-6 作为阈值 ——
    // 比 1/255 ≈ 0.0039 小得多，不会误伤正常相邻的点。
    QList<CurvePoint> cleaned;
    cleaned.reserve(m_points.size());
    for (const CurvePoint &pt : m_points) {
        if (!cleaned.isEmpty() && qAbs(cleaned.last().x - pt.x) < 1e-6) {
            // 保留后出现的那个（代表用户最新意图）
            cleaned.last() = pt;
        } else {
            cleaned.append(pt);
        }
    }
    m_points = cleaned;

    // ── 3. 确保端点存在 ──
    // 曲线必须覆盖 [0,1] 全域，否则 LUT 的首尾会出现未定义的映射。
    if (m_points.isEmpty()) {
        m_points = { CurvePoint(0.0, 0.0), CurvePoint(1.0, 1.0) };
        return;
    }

    if (m_points.first().x > 1e-6) {
        // 起点缺失：补一个 (0, 首点的 y)。
        // 用首点的 y 而不是 0，是为了让曲线从左侧平滑进入，而不是
        // 在左边缘突然跳一下。
        m_points.prepend(CurvePoint(0.0, m_points.first().y));
    } else {
        m_points.first().x = 0.0;
    }

    if (m_points.last().x < 1.0 - 1e-6) {
        m_points.append(CurvePoint(1.0, m_points.last().y));
    } else {
        m_points.last().x = 1.0;
    }

    // ── 4. 钳制 y 到 [0,1] ──
    // 允许用户把点拖出画布，但数值上必须合法，否则 LUT 会溢出。
    for (CurvePoint &pt : m_points)
        pt.y = qBound(0.0, pt.y, 1.0);
}

int Curve::addPoint(const CurvePoint &pt)
{
    CurvePoint p = pt;
    p.x = qBound(0.0, p.x, 1.0);
    p.y = qBound(0.0, p.y, 1.0);

    // 查找是否已有同 x 的点
    for (int i = 0; i < m_points.size(); ++i) {
        if (qAbs(m_points[i].x - p.x) < 1e-4) {
            m_points[i].y = p.y;
            return i;
        }
    }

    // 插入到正确位置以保持 x 递增
    int insertAt = m_points.size();
    for (int i = 0; i < m_points.size(); ++i) {
        if (m_points[i].x > p.x) {
            insertAt = i;
            break;
        }
    }

    m_points.insert(insertAt, p);
    return insertAt;
}

bool Curve::movePoint(int index, const CurvePoint &pt)
{
    if (index < 0 || index >= m_points.size())
        return false;

    const bool isFirst = (index == 0);
    const bool isLast  = (index == m_points.size() - 1);

    CurvePoint newPt = m_points[index];

    if (isFirst) {
        // 端点：x 锁死在 0，只能上下移动
        newPt.x = 0.0;
        newPt.y = qBound(0.0, pt.y, 1.0);
    } else if (isLast) {
        newPt.x = 1.0;
        newPt.y = qBound(0.0, pt.y, 1.0);
    } else {
        // 中间点：x 被限制在左右邻居之间，不允许越过。
        // 留出 1e-5 的安全间隙，防止与邻居重合导致差商除零。
        //
        // 为什么要限制而不是允许越过？因为允许越过就必须重排序，
        // 而拖动过程中索引会变，鼠标追踪会变得非常混乱。
        // 限制住之后，每个中间点始终只在自己的一小段区间里移动，逻辑简单可靠。
        const qreal leftX  = m_points[index - 1].x;
        const qreal rightX = m_points[index + 1].x;
        newPt.x = qBound(leftX + 1e-5, pt.x, rightX - 1e-5);
        newPt.y = qBound(0.0, pt.y, 1.0);
    }

    // 判断是否真的有变化，避免无意义的信号发射与重绘
    const bool changed = !qFuzzyCompare(newPt.x, m_points[index].x)
                      || !qFuzzyCompare(newPt.y, m_points[index].y);

    m_points[index] = newPt;
    return changed;
}

bool Curve::removePoint(int index)
{
    // 只剩两个点（端点）时不能再删
    if (m_points.size() <= 2)
        return false;
    if (index <= 0 || index >= m_points.size() - 1)
        return false;   // 端点不可删除

    m_points.removeAt(index);
    return true;
}

int Curve::findNearestPoint(const CurvePoint &pos, qreal maxDistance) const
{
    int bestIndex = -1;
    qreal bestDist = maxDistance;

    for (int i = 0; i < m_points.size(); ++i) {
        const qreal dx = m_points[i].x - pos.x;
        const qreal dy = m_points[i].y - pos.y;
        const qreal dist = qSqrt(dx * dx + dy * dy);
        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = i;
        }
    }

    return bestIndex;
}

bool Curve::isIdentity() const
{
    // 判据：每个点都在对角线上。
    // 用 1/512 作为容差（约 0.002）—— 比一个像素的精度（1/255≈0.004）宽松一点，
    // 避免因为浮点误差把"看起来是对角线"的曲线判成非恒等。
    for (const CurvePoint &pt : m_points) {
        if (qAbs(pt.y - pt.x) > 1.0 / 512.0)
            return false;
    }
    return true;
}

void Curve::reset()
{
    m_points = { CurvePoint(0.0, 0.0), CurvePoint(1.0, 1.0) };
}

// ===========================================================================
//  烘焙：Fritsch–Carlson 单调三次样条
// ===========================================================================

namespace {

// 计算每段区间的斜率（差商）delta[i] = (y[i+1]-y[i]) / (x[i+1]-x[i])
QVector<qreal> computeDeltas(const QList<CurvePoint> &pts)
{
    QVector<qreal> deltas;
    const int n = pts.size();
    if (n < 2)
        return deltas;

    deltas.reserve(n - 1);
    for (int i = 0; i < n - 1; ++i) {
        const qreal dx = pts[i + 1].x - pts[i].x;
        // 保护：normalize() 保证了 x 严格递增，但这里仍然防一手。
        // 除法产生 inf 或 NaN 会污染整条曲线，且症状极难定位。
        if (dx <= 1e-12) {
            deltas.append(0.0);
        } else {
            deltas.append((pts[i + 1].y - pts[i].y) / dx);
        }
    }
    return deltas;
}

// Fritsch–Carlson 切线计算
//
// 算法要点（这是本文件最关键的一段）：
//   1. 先给每个点一个初始切线 m[i] = (delta[i-1] + delta[i]) / 2
//   2. 端点用单边差分：m[0] = delta[0]，m[n-1] = delta[n-2]
//   3. 对每个区间检查单调性条件：
//        (a) 若 delta[i] == 0，则强制 m[i] = m[i+1] = 0（该段是平的）
//        (b) 否则令 alpha = m[i]/delta[i]，beta = m[i+1]/delta[i]，
//            若 alpha^2 + beta^2 > 9，则按比例缩放使两者落在圆 r=3 内
//      这个 r=3 的圆约束是 Fritsch–Carlson 的核心结论，
//      它保证三次 Hermite 插值在每个区间内单调。
QVector<qreal> computeTangents(const QList<CurvePoint> &pts)
{
    const int n = pts.size();
    // ★ 用 QVector 而不是 QList ★
    //   Qt5 的 QList<T> 没有 (size, value) 形式的构造函数，
    //   写成 QList<qreal> m(n, 0.0) 会报"没有重载函数接受 2 个参数"。
    //   Qt6 里 QVector 就是 QList 的别名，所以两种写法在 Qt6 下等价；
    //   写 QVector 则两版都能编，语义也明确表示"连续数组"。
    QVector<qreal> m(n, 0.0);
    if (n < 2)
        return m;
    if (n == 2) {
        // 只有两个点：切线就是这条线本身的斜率，样条退化为直线
        const qreal dx = pts[1].x - pts[0].x;
        const qreal slope = (dx > 1e-12) ? (pts[1].y - pts[0].y) / dx : 0.0;
        m[0] = m[1] = slope;
        return m;
    }

    const QVector<qreal> delta = computeDeltas(pts);

    // ── 步骤 1：中心差分作为初始切线 ──
    m[0] = delta.first();
    for (int i = 1; i < n - 1; ++i)
        m[i] = (delta[i - 1] + delta[i]) * 0.5;
    m[n - 1] = delta.last();

    // ── 步骤 2：逐区间施加单调性约束 ──
    for (int i = 0; i < n - 1; ++i) {
        const qreal d = delta[i];

        if (qFuzzyIsNull(d)) {
            // 该段水平：两端切线必须为 0，否则会在这一段内产生鼓包。
            m[i]     = 0.0;
            m[i + 1] = 0.0;
            continue;
        }

        // alpha 和 beta 是切线相对于区间斜率的大小
        qreal alpha = m[i]     / d;
        qreal beta  = m[i + 1] / d;

        // 负的 alpha/beta 意味着切线方向与区间趋势相反，
        // 那一定会造成过冲，直接归零。
        if (alpha < 0.0) {
            m[i] = 0.0;
            alpha = 0.0;
        }
        if (beta < 0.0) {
            m[i + 1] = 0.0;
            beta = 0.0;
        }

        // 圆约束：alpha^2 + beta^2 <= 9
        const qreal magnitude = alpha * alpha + beta * beta;
        if (magnitude > 9.0) {
            // 按比例缩放到半径 3 的圆上。这样保留了 alpha 与 beta 的比例关系，
            // 曲线形状的"倾向"不变，只是整体变得缓和。
            const qreal tau = 3.0 / qSqrt(magnitude);
            m[i]     = tau * alpha * d;
            m[i + 1] = tau * beta  * d;
        }
    }

    return m;
}

// 三次 Hermite 插值求值
//   给定区间 [x0, x1]，两端点值 y0/y1 与切线 m0/m1，
//   以及归一化位置 t ∈ [0,1]，返回插值结果。
//
//   公式：h00*y0 + h10*h*m0 + h01*y1 + h11*h*m1
//   其中 h00 = 2t³-3t²+1, h10 = t³-2t²+t, h01 = -2t³+3t², h11 = t³-t²
qreal hermite(qreal t, qreal y0, qreal y1,
              qreal h, qreal m0, qreal m1)
{
    const qreal t2 = t * t;
    const qreal t3 = t2 * t;

    const qreal h00 =  2.0 * t3 - 3.0 * t2 + 1.0;
    const qreal h10 =        t3 - 2.0 * t2 + t;
    const qreal h01 = -2.0 * t3 + 3.0 * t2;
    const qreal h11 =        t3 -       t2;

    return h00 * y0 + h10 * h * m0 + h01 * y1 + h11 * h * m1;
}

} // namespace

CurveLut Curve::bake(bool smooth) const
{
    CurveLut lut{};

    // 点数不足（理论上不会发生，normalize 保证了至少 2 个点），
    // 退化为恒等曲线而不是崩溃。
    if (m_points.size() < 2) {
        for (int i = 0; i < 256; ++i)
            lut[size_t(i)] = quint8(i);
        return lut;
    }

    // ── 线性插值分支 ──
    // 用于"硬折线"效果，同时也是验证样条实现的对照基准：
    // 两种模式在所有控制点上的取值必须完全相同。
    if (!smooth) {
        int seg = 0;
        for (int i = 0; i < 256; ++i) {
            const qreal x = i / 255.0;

            // 找到 x 所在的区间
            while (seg < m_points.size() - 2 && x > m_points[seg + 1].x)
                ++seg;

            const CurvePoint &p0 = m_points[seg];
            const CurvePoint &p1 = m_points[seg + 1];

            const qreal dx = p1.x - p0.x;
            qreal y;
            if (dx <= 1e-12) {
                y = p1.y;
            } else {
                const qreal t = qBound(0.0, (x - p0.x) / dx, 1.0);
                y = p0.y + t * (p1.y - p0.y);
            }

            lut[size_t(i)] = quint8(qBound(0, int(qRound(y * 255.0)), 255));
        }
        return lut;
    }

    // ── 单调三次样条分支 ──

    // 计算全部切线。注意这一步只做一次（n 个点，n 通常小于 20），
    // 而后面 256 次求值复用这些切线 —— 这正是 LUT 烘焙高效的原因。
    const QVector<qreal> tangents = computeTangents(m_points);

    int seg = 0;
    for (int i = 0; i < 256; ++i) {
        const qreal x = i / 255.0;

        // 定位区间。因为 x 是单调递增的，用游标而不是每次二分查找，
        // 整体复杂度从 O(256 log n) 降到 O(256 + n)。
        while (seg < m_points.size() - 2 && x > m_points[seg + 1].x)
            ++seg;

        const CurvePoint &p0 = m_points[seg];
        const CurvePoint &p1 = m_points[seg + 1];

        const qreal dx = p1.x - p0.x;

        qreal y;
        if (dx <= 1e-12) {
            // 退化区间：直接取右端点值
            y = p1.y;
        } else {
            const qreal t = qBound(0.0, (x - p0.x) / dx, 1.0);
            y = hermite(t, p0.y, p1.y, dx, tangents[seg], tangents[seg + 1]);
        }

        // 最后一道防线：把结果钳制到 [0,1] 再映射到 8 位。
        // 理论上单调样条不会越界，但浮点误差可能让它跑到 1.0000001，
        // 不钳制就会变成 256 → 截断为 0，一处亮斑突然变黑。
        lut[size_t(i)] = quint8(qBound(0, int(qRound(y * 255.0)), 255));
    }

    return lut;
}

// ===========================================================================
//  CurvesSettings
// ===========================================================================

CurvesSettings::CurvesSettings() = default;

const Curve &CurvesSettings::curve(CurveChannel ch) const
{
    // 越界时返回主通道的曲线，而不是抛异常 —— 这是渲染路径，
    // 宁可结果不对也不能崩。
    const int idx = int(ch);
    if (idx < 0 || idx >= int(CurveChannel::Count))
        return m_curves[0];
    return m_curves[idx];
}

Curve &CurvesSettings::curveRef(CurveChannel ch)
{
    const int idx = int(ch);
    if (idx < 0 || idx >= int(CurveChannel::Count))
        return m_curves[0];
    return m_curves[idx];
}

void CurvesSettings::setCurve(CurveChannel ch, const Curve &curve)
{
    const int idx = int(ch);
    if (idx < 0 || idx >= int(CurveChannel::Count))
        return;
    m_curves[idx] = curve;
}

bool CurvesSettings::isIdentity() const
{
    for (int i = 0; i < int(CurveChannel::Count); ++i) {
        if (!m_curves[i].isIdentity())
            return false;
    }
    return true;
}

void CurvesSettings::reset()
{
    for (int i = 0; i < int(CurveChannel::Count); ++i)
        m_curves[i].reset();
}

ChannelLuts CurvesSettings::bakeLuts() const
{
    ChannelLuts luts;

    // 主通道曲线（RGB）
    const CurveLut rgbLut = m_curves[int(CurveChannel::Rgb)].bake();

    // 各单通道曲线
    const CurveLut rLut = m_curves[int(CurveChannel::Red)].bake();
    const CurveLut gLut = m_curves[int(CurveChannel::Green)].bake();
    const CurveLut bLut = m_curves[int(CurveChannel::Blue)].bake();

    // ── 叠乘 ──
    // 像素先穿过主曲线，再穿过对应的单通道曲线：
    //   final(x) = channelCurve(rgbCurve(x))
    // 也就是"以主曲线为内层、单通道曲线为外层"做合成。
    //
    // 这里做一次 isIdentity 判断是值得的：如果用户没动主曲线（绝大多数
    // 只调单通道的场景），就不必做合成，直接使用单通道表。
    const bool rgbIsIdentity =
        m_curves[int(CurveChannel::Rgb)].isIdentity();

    auto combine = [rgbIsIdentity](const CurveLut &channel) -> CurveLut {
        if (rgbIsIdentity)
            return channel;
        return composeLut(channel, CurveLut{});   // 占位，下面覆盖
    };
    Q_UNUSED(combine)   // 上面这个 lambda 是文档性质的，实际用下面的显式写法

    if (rgbIsIdentity) {
        luts.r = rLut;
        luts.g = gLut;
        luts.b = bLut;
    } else {
        // composeLut(inner, outer)：先内层后外层。
        // 我们要的是"先过主曲线（内），再过通道曲线（外）"，所以
        //   inner = rgbLut, outer = channelLut
        // 注意 python 里的 composeLut 定义是 outer[inner[i]]，正好对应。
        luts.r = composeLut(rgbLut, rLut);
        luts.g = composeLut(rgbLut, gLut);
        luts.b = composeLut(rgbLut, bLut);
    }

    return luts;
}

// ===========================================================================
//  预设
// ===========================================================================

QList<CurvePreset> builtinPresets()
{
    QList<CurvePreset> presets;

    // ── 线性（默认）──
    presets.append({ QObject::tr("Linear"),
                     { CurvePoint(0.0, 0.0), CurvePoint(1.0, 1.0) } });

    // ── 提亮 / 压暗 ──
    presets.append({ QObject::tr("Brighten"),
                     { CurvePoint(0.0, 0.0), CurvePoint(0.5, 0.62), CurvePoint(1.0, 1.0) } });

    presets.append({ QObject::tr("Darker"),
                     { CurvePoint(0.0, 0.0), CurvePoint(0.5, 0.38), CurvePoint(1.0, 1.0) } });

    // ── 对比度增强（S 形）──
    // 暗部更暗、亮部更亮，中间调基本不动。
    // 用四个点让曲线在两端更平缓，避免高光和暗部被压死成纯黑纯白。
    presets.append({ QObject::tr("Increase Contrast"),
                     { CurvePoint(0.0, 0.0),
                       CurvePoint(0.25, 0.16),
                       CurvePoint(0.75, 0.84),
                       CurvePoint(1.0, 1.0) } });

    presets.append({ QObject::tr("Decrease Contrast"),
                     { CurvePoint(0.0, 0.0),
                       CurvePoint(0.25, 0.35),
                       CurvePoint(0.75, 0.65),
                       CurvePoint(1.0, 1.0) } });

    // ── 胶片感（轻微褪色 + 微 S）──
    // 黑场抬高到 0.06，模拟胶片的"雾化"；白场压到 0.95 避免死白。
    presets.append({ QObject::tr("Filmic"),
                     { CurvePoint(0.0, 0.06),
                       CurvePoint(0.3, 0.30),
                       CurvePoint(0.7, 0.74),
                       CurvePoint(1.0, 0.95) } });

    // ── 褪色 / 淡化 ──
    presets.append({ QObject::tr("Fade"),
                     { CurvePoint(0.0, 0.12),
                       CurvePoint(0.5, 0.52),
                       CurvePoint(1.0, 0.88) } });

    // ── 反转（负片）──
    presets.append({ QObject::tr("Invert"),
                     { CurvePoint(0.0, 1.0), CurvePoint(1.0, 0.0) } });

    return presets;
}

// ===========================================================================
//  便捷入口
// ===========================================================================

QImage applyCurves(const QImage &src, const CurvesSettings &settings)
{
    if (src.isNull())
        return QImage();

    // 全恒等时直接返回副本，省掉一整趟逐像素运算。
    // 这个快速路径很实在：用户打开曲线对话框后没做修改就点确定，
    // 或者预览时滑块回到原位，都会命中这里。
    if (settings.isIdentity())
        return src.copy();

    return applyLuts(src, settings.bakeLuts());
}

} // namespace zl
