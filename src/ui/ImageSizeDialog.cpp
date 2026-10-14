// ============================================================================
//  ImageSizeDialog.cpp —— "图像大小"对话框的实现
// ============================================================================

#include "ui/ImageSizeDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QtMath>

namespace zl {

namespace {

// 单图层像素数超过这个量就提示内存风险。
//
//  512MB ≈ 1.15 亿像素的 ARGB32。到这个量级时，一次"确定"就要
//  为撤销栈再压一份前后快照（压缩后也可能上百 MB），是真正会让
//  用户感到"点了确定之后卡住"的规模。
constexpr qint64 kMemoryWarnBytes = 512LL * 1024 * 1024;

// 百万像素。保留一位小数 —— 用户看的是量级，不是精度。
QString megaPixels(const QSize &sz)
{
    const double mp = double(sz.width()) * double(sz.height()) / 1.0e6;
    return QString::number(mp, 'f', 1);
}

// 人类可读的字节数，用于内存风险提示。
QString humanBytes(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return QObject::tr("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 1);
    return QObject::tr("%1 MB").arg(double(bytes) / (1024.0 * 1024), 0, 'f', 0);
}

} // namespace

ImageSizeDialog::ImageSizeDialog(const QSize &original,
                                 ResampleMethod initialMethod,
                                 int layerCount,
                                 QWidget *parent)
    : SizeChangeDialogBase(original, tr("Image Size"),
                           /*allowRatioLock=*/true,
                           /*allowRelative=*/false,
                           parent)
    , m_layerCount(qMax(1, layerCount))
{
    // ── 重采样方式 ──
    //
    // ★ 顺序固定为"从快到好" ★
    //   下拉框的顺序本身就是给用户的信息。不按 enum 的数值排，
    //   而在插入时显式指定顺序，这样将来往 enum 里插新算法也不会
    //   打乱展示顺序。
    auto *resampleArea = new QWidget(this);

    auto *row = new QHBoxLayout(resampleArea);
    row->setContentsMargins(0, 0, 0, 0);

    auto *label = new QLabel(tr("Resampling(&S):"), resampleArea);

    m_methodCombo = new QComboBox(resampleArea);
    m_methodCombo->setObjectName(QStringLiteral("comboResample"));

    struct Entry { ResampleMethod method; const char *tip; };
    const Entry entries[] = {
        { ResampleMethod::Nearest,
          QT_TR_NOOP("Fastest, but enlarging shows obvious blocks. Only use "
                     "it for pixel art and icons that must keep hard "
                     "edges.") },
        { ResampleMethod::Bilinear,
          QT_TR_NOOP("Fast, but downscaling photos introduces jaggies. Generally a preview-only algorithm.") },
        { ResampleMethod::Bicubic,
          QT_TR_NOOP("The balance of quality and speed; suits most photos. The default.") },
        { ResampleMethod::Lanczos,
          QT_TR_NOOP("The sharpest, but slight ringing may appear near "
                     "high-contrast edges. Suited to the first downscale "
                     "when detail matters.") },
    };

    int defaultIndex = 0;
    for (int i = 0; i < int(sizeof(entries) / sizeof(entries[0])); ++i) {
        m_methodCombo->addItem(resampleMethodName(entries[i].method),
                               int(entries[i].method));
        m_methodCombo->setItemData(i, tr(entries[i].tip), Qt::ToolTipRole);
        if (entries[i].method == initialMethod)
            defaultIndex = i;
    }
    m_methodCombo->setCurrentIndex(defaultIndex);
    m_methodCombo->setMinimumWidth(200);

    label->setBuddy(m_methodCombo);

    row->addWidget(label);
    row->addWidget(m_methodCombo, 1);

    auto *hint = new QLabel(tr("(takes effect only when you click OK)"), resampleArea);
    hint->setEnabled(false);
    row->addWidget(hint);

    // ── 交给基类完成整体布局 ──
    buildLayout(resampleArea);

    // ★ 长宽比默认锁定 ★
    //   改图像大小而破坏比例是少数需求，绝大多数时候用户是在等比缩放。
    //   默认锁定可以省掉一次勾选，也让"手滑改了一个数字导致图被拉扁"
    //   这种事故不会发生。放在 buildLayout() 之后调用 —— 此时勾选框
    //   才存在，且基类已完成初始化，勾选会正常刷新信息面板。
    ratioCheck()->setChecked(true);
}

ImageSizeDialog::~ImageSizeDialog() = default;

ResampleMethod ImageSizeDialog::method() const
{
    const int value = m_methodCombo->currentData().toInt();
    switch (value) {
    case int(ResampleMethod::Nearest):  return ResampleMethod::Nearest;
    case int(ResampleMethod::Bilinear): return ResampleMethod::Bilinear;
    case int(ResampleMethod::Lanczos):  return ResampleMethod::Lanczos;
    default:                            return ResampleMethod::Bicubic;
    }
}

QString ImageSizeDialog::infoText(const QSize &newSize) const
{
    const QSize oldSize = originalSize();

    // 两个方向各自的比例。
    //
    // ★ 为什么要分开算 ★
    //   输入框有了"百分比"单位之后，用户会**拿输入框里的数字去对信息面板
    //   里的数字**。原来是按最长边算一个总比例，在"解开长宽比锁定、只改
    //   一个方向"时就会对不上：宽度输入 50%，面板却写 100%（因为它看的是
    //   没变的最长边）。两个数各自都"没错"，但用户会认为程序算错了。
    //   所以两轴比例不同时，就把两轴分别写出来。
    const double pw = (oldSize.width()  > 0)
                    ? 100.0 * double(newSize.width())  / double(oldSize.width())  : 100.0;
    const double ph = (oldSize.height() > 0)
                    ? 100.0 * newSize.height() / double(oldSize.height()) : 100.0;

    // 放大提醒与配色以**较大的那一边**为准：只要有一个方向被放大过，
    // 画面就必然有地方变软，"另一边缩小了"不能抵消这件事。
    const double percent = qMax(pw, ph);

    QString html;
    html += tr("Current: %1 × %2 px (%3 MP)")
                .arg(oldSize.width()).arg(oldSize.height()).arg(megaPixels(oldSize));
    html += QStringLiteral("<br>");

    // 新尺寸用加粗：这是"将要发生什么"的答案，值得在视觉上压过当前值。
    html += tr("New size: <b>%1 × %2 px</b> (%3 MP)")
                .arg(newSize.width()).arg(newSize.height()).arg(megaPixels(newSize));

    // 百分比上色：放大红、缩小绿。
    //
    //  这里用的是**语义色而不是涨跌色** —— 中文语境里"红"在股票里表示涨，
    //  但在这个对话框里它表示"要变大/有风险"，两者不冲突：缩小时用绿色
    //  是"省资源"的好消息，放大时用橙色是"会变软"的提醒。
    const QString percentColor = (percent > 100.0) ? QStringLiteral("#c86400")
                                                   : QStringLiteral("#1f7a1f");
    // 两轴一致（锁定长宽比时的常态）只写一个数；不一致就写明是哪一维。
    if (qAbs(pw - ph) < 0.05) {
        html += QStringLiteral("　<span style='color:%1'><b>%2%</b></span>")
                    .arg(percentColor)
                    .arg(QString::number(pw, 'f', 1));
    } else {
        html += QStringLiteral("　<span style='color:%1'><b>%2</b></span>")
                    .arg(percentColor)
                    .arg(tr("W %1%  H %2%")
                             .arg(QString::number(pw, 'f', 1))
                             .arg(QString::number(ph, 'f', 1)));
    }

    // ── 风险提示 ──

    if (percent > 100.0) {
        // ★ 放大不是"更清晰" ★
        //   这是最需要主动说明的一条：大量用户认为把图放大 200% 会让它
        //   更清楚。重采样只能插值，无法凭空造出细节。
        html += QStringLiteral("<br><span style='color:#c86400'>")
              + tr("Enlarging: resampling cannot invent detail; the image "
                   "will soften. When you need more resolution, look for "
                   "the original source material.")
              + QStringLiteral("</span>");
    }

    const qint64 bytesPerLayer = qint64(newSize.width()) * newSize.height() * 4;
    const qint64 totalBytes = bytesPerLayer * m_layerCount;

    if (totalBytes >= kMemoryWarnBytes) {
        html += QStringLiteral("<br><span style='color:#b00000'>")
              + tr("Memory note: %1 layers need about %2 in total, and "
                   "undo will store another compressed snapshot before "
                   "and after, so operations may slow noticeably.")
                    .arg(m_layerCount).arg(humanBytes(totalBytes))
              + QStringLiteral("</span>");
    }

    return html;
}

} // namespace zl
