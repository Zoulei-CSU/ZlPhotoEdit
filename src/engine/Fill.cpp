#include "engine/Fill.h"
#include "engine/Brush.h"      // composeFill 直接转调 composeStroke

#include <QBitArray>
#include <QVector>
#include <QColor>
#include <QtMath>

namespace zl {

// ===========================================================================
//  容差：色差度量
// ===========================================================================

int colorDistance(QRgb a, QRgb b)
{
    // ★ 读的是原始分量，不做任何预乘/反预乘转换 ★
    //
    //   调用方传进来的已经是预乘位图的原始值（见 fillDistance 的说明），
    //   这里若再"反预乘一下"，就会与种子色的处理不一致 ——
    //   种子色是原样读出来的，两边都转才是对的，只转一边必然错。
    const int dr = qRed(a)   - qRed(b);
    const int dg = qGreen(a) - qGreen(b);
    const int db = qBlue(a)  - qBlue(b);

    // 除以 3 再开方 → 归一化到 0-255。
    // 用 double 而不是 float：12M 像素的循环里这点精度差异不花钱。
    return int(qRound(std::sqrt(double(dr * dr + dg * dg + db * db) / 3.0)));
}

int fillDistance(QRgb a, QRgb b)
{
    // ★ 取 max 而不是"把 alpha 也平方加进去再除以 4" ★
    //
    //   两者都能表达"alpha 也要参与判据"，但取 max 有一个很实际的
    //   好处：**容差滑块的语义在 RGB 与 alpha 两个维度上完全一致**。
    //   "容差 32" 就是"RGB 色差 ≤ 32 且 alpha 差 ≤ 32"，用户不需要理解
    //   "四个通道的欧氏距离"这种概念。
    //
    //   而且对照片（alpha 恒为 255）而言 alpha 差恒为 0，
    //   fillDistance 与 colorDistance **逐位相等** —— 不吃任何精度亏。
    const int alphaDiff = qAbs(qAlpha(a) - qAlpha(b));
    return qMax(colorDistance(a, b), alphaDiff);
}

// ===========================================================================
//  扫描线种子填充
// ===========================================================================

namespace {

// 扫描线栈里的一项：某一行上的一个**种子 x**。
//
// ★ 为什么只存一个点而不是"整条线段" ★
//   存线段的话，压栈时要先把线段算出来，而"算线段"这一步本身就要做
//   左右的容差扩展 —— 于是同一段像素可能在压栈和出栈时各扩展一次。
//   只存种子点，出栈时才扩展、扩展完立刻标记已访问，每个像素恰好
//   被扩展一次。
struct FillSeed
{
    int y = 0;
    int x = 0;
};

} // namespace

FillResult floodFill(const QImage &src, const FloodFillParams &params)
{
    FillResult result;

    if (src.isNull() || src.width() <= 0 || src.height() <= 0)
        return result;

    // 统一到 ARGB32_Premultiplied 再逐扫描线读。
    // 已经是对应格式时 convertToFormat 会直接返回原图（不拷贝）。
    QImage img = src;
    if (img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.isNull())
        return result;

    const int w = img.width();
    const int h = img.height();

    // ★ 起点越界：静默返回空结果 ★
    //   调用方（工具）在这一步之前已经做过画布范围检查，这里只是兜底 ——
    //   绝不允许越界读 scanLine。
    const QPoint seed = params.seed;
    if (seed.x() < 0 || seed.y() < 0 || seed.x() >= w || seed.y() >= h)
        return result;

    result.mask = QImage(w, h, QImage::Format_Grayscale8);
    if (result.mask.isNull())        // 内存不足
        return result;
    result.mask.fill(0);

    const int tol   = qBound(0, params.tolerance, 255);
    const int band  = params.antialias ? qBound(0, params.softBand, 255) : 0;
    // 扩展上限。钳到 255 是因为色差本身就在 0-255 之间，
    // tol + band 超过 255 时多出来的部分没有任何像素能落进去。
    const int limit = qMin(255, tol + band);

    const QRgb seedRgb =
        reinterpret_cast<const QRgb *>(img.constScanLine(seed.y()))[seed.x()];

    // ── 覆盖度 ──
    //
    //   d <= tol            → 255（完全填充）
    //   tol < d <= limit    → 线性衰减到 0（过渡带）
    //   d > limit           → 不在区域内
    //
    // ★ 判据始终以**种子色**为参照，不是与邻居比较 ★
    //   与邻居比较的话，区域范围会随遍历顺序变化（从左上进来和从右下
    //   进来得到的形状不同），而且会沿着渐变一路蔓延到天边。
    //   以种子色为参照则区域是"到种子的色差在容差内的连通块"，与顺序无关。
    const auto coverageOf = [&](QRgb px) -> int {
        const int d = fillDistance(px, seedRgb);
        if (d > limit)
            return -1;                      // -1 = 不在区域内
        if (d <= tol || band <= 0)
            return 255;
        return int(qRound(255.0 * (limit - d) / band));
    };

    const int pixelCount = w * h;

    // 区域位图。
    // ★ 为什么要单独存一份，而不是在扫描线里现算 ★
    //   扫描线扩展时同一个像素的容差判据要算两三次（本级标记、
    //   上下两行的探查）。而 fillDistance 里有开方，4000×3000 全图
    //   就是 1200 万次开方 —— 每次点击都付这个代价不值得。
    //   预计算一遍（位图只占 w*h/8 字节）后面就全是整数比较。
    QBitArray region(pixelCount, false);

    // 过渡带像素的覆盖度要在"确认属于本区域"之后才敢写，
    // 但重算一次 d 又要开方 —— 于是先把它记在一张字节表里。
    // 这张表在连续模式下只对"真正连通的那部分"生效。
    QVector<uchar> coverage(pixelCount, 0);
    int fullCount = 0;

    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const int cov = coverageOf(line[x]);
            if (cov < 0)
                continue;
            const int idx = y * w + x;
            region.setBit(idx);
            coverage[idx] = uchar(cov);
            if (cov > 0)
                ++fullCount;
        }
    }

    if (fullCount == 0)
        return result;                      // 一个像素都没落进区域

    // ── 写出覆盖度并统计包围盒 ──
    const auto writeCoverage = [&](int y, int x, int cov) {
        uchar *line = result.mask.scanLine(y);
        line[x] = uchar(cov);

        if (result.bounds.isEmpty()) {
            result.bounds = QRect(x, y, 1, 1);
        } else {
            if (x < result.bounds.left())   result.bounds.setLeft(x);
            if (x > result.bounds.right())  result.bounds.setRight(x);
            if (y < result.bounds.top())    result.bounds.setTop(y);
            if (y > result.bounds.bottom()) result.bounds.setBottom(y);
        }
    };

    // ── 非连续模式：区域位图直接就是结果 ──
    //   一次全图遍历，与连通性无关。实现上没有"扫描线"可言 ——
    //   这也正是它比连续模式快的原因。
    if (!params.contiguous) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int idx = y * w + x;
                if (region.testBit(idx) && coverage[idx] > 0)
                    writeCoverage(y, x, coverage[idx]);
            }
        }
        return result;
    }

    // ── 连续模式：扫描线种子填充 ──
    //
    //  visited 与 region **必须分开**：
    //    region 说的是"这个像素颜色合格"，
    //    visited 说的是"这个像素已经被某条线段认领过了"。
    //  只有 region 而没有 visited 的话，相邻线段会互相把对方重新压栈，
    //  栈会无限增长 —— 这是扫描线填充最经典的 bug。
    QBitArray visited(pixelCount, false);

    QVector<FillSeed> stack;
    stack.reserve(1024);

    const auto inRegion = [&](int x, int y) {
        const int idx = y * w + x;
        return region.testBit(idx) && !visited.testBit(idx);
    };

    stack.append(FillSeed{ seed.y(), seed.x() });

    while (!stack.isEmpty()) {
        const FillSeed s = stack.takeLast();
        const int y = s.y;

        if (!inRegion(s.x, y))
            continue;

        // 向左右扩展成一条线段
        int xl = s.x;
        while (xl - 1 >= 0 && inRegion(xl - 1, y))
            --xl;

        int xr = s.x;
        while (xr + 1 < w && inRegion(xr + 1, y))
            ++xr;

        // 标记 + 写覆盖度
        for (int x = xl; x <= xr; ++x) {
            const int idx = y * w + x;
            visited.setBit(idx);
            const int cov = coverage[idx];
            if (cov > 0)
                writeCoverage(y, x, cov);
        }

        // 上下两行各找"新线段的起点"，每个连续段只压一个种子。
        //
        // ★ 搜索范围**必须**裁剪到当前线段的 [xl, xr] ★
        //
        //   这是本算法正确性的关键，也是我第一版写错的地方：
        //   当时想着"上一行的线段可能比当前这条更宽，裁剪会把越界的那截
        //   漏掉"，于是从 xl 一路扫到行尾 —— 结果跨过了障碍物，
        //   把隔着一堵墙的另一块同色区域也当成了连通的，
        //   **连续模式静默退化成非连续模式**（测试里的"竖墙"与"甜甜圈"
        //   两组同时报红）。
        //
        //   正确性依据：行 y 上的线段 S 填完之后，行 y-1 上所有与 S
        //   4-邻接的像素，其 x 必然落在 [xl, xr] 内 —— 这是"邻接"的定义。
        //   更远的同色像素要么被障碍隔断（不该填），要么经由别的线段
        //   连通（会在处理那条线段时被发现）。
        //
        //   至于"更宽的线段"：种子压进去之后，出栈时那次左右扩展
        //   本来就会把它扩展到真实边界，不需要在扫描阶段操心。
        //   段与段之间我只跳过一次，同一段不会压两个种子。
        for (int dy = -1; dy <= 1; dy += 2) {
            const int ny = y + dy;
            if (ny < 0 || ny >= h)
                continue;

            int x = xl;
            while (x <= xr) {
                if (!inRegion(x, ny)) {
                    ++x;
                    continue;
                }
                stack.append(FillSeed{ ny, x });
                while (x <= xr && inRegion(x, ny))
                    ++x;
            }
        }
    }

    // ★ 覆盖度是在遍历过程中写的，不是预先写的 ★
    //   于是"颜色合格但与种子不连通"的那部分像素**从头到尾没人写过**，
    //   蒙版里天然就是 0 —— 不需要再来一趟全图清零。
    //   （若改成在预计算那一步就写蒙版，就必须补一趟清零，
    //     而漏掉它的症状是"连续模式填出了非连续的结果"，
    //     看起来像容差算错了，其实是漏了一次清理。）
    return result;
}

// ===========================================================================
//  合成
// ===========================================================================
QImage composeFill(const QImage &base, const QImage &mask,
                   const QColor &fillColor, const QImage &selectionMask)
{
    // 直接转调画笔那条合成路径。
    //
    // ★ 这个转调不是"图省事"，而是一条架构约束 ★
    //   "选区蒙版必须在颜色预乘之前乘进覆盖度"是画笔踩出来的坑
    //   （见 Brush.h 末尾）。填充与画笔的合成语义**完全一致**：
    //   都是一份覆盖度蒙版 + 一个颜色。若在这里另写一份，那个坑
    //   就要再踩一次，而且两边会慢慢分叉。
    //
    //   erase 传 false：填充永远是把颜色盖上去，没有"擦除"语义
    //   （"填充透明"由 fillColor 的 alpha=0 表达）。
    return composeStroke(base, mask, fillColor, /*erase=*/false, selectionMask);
}

} // namespace zl
