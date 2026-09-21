// ============================================================================
//  RotateImageDialog.cpp —— "任意角度旋转"对话框的实现
// ============================================================================

#include "ui/RotateImageDialog.h"

#include "engine/Geometry.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>

namespace zl {

namespace {

// 滑块以 0.1° 为一档：-1800..1800 表示 -180.0°..180.0°。
//
// ★ 为什么不直接用整数度 ★
//   整数度的滑块只有 361 档，且无法表达 0.1° —— 而"摆正一张扫描件"
//   这类最常见的用途恰恰需要 0.3°~1.5° 的微调。把滑块放大十倍，
//   拖动一格仍是可感知的 0.1°，键盘微调也够用。
constexpr int kSliderPerDegree = 10;
constexpr int kSliderMax       = 180 * kSliderPerDegree;

// 填充色下拉项的固定顺序（fillColor() 依赖它）
enum FillMode {
    FillTransparent = 0,
    FillBackground  = 1,
    FillWhite       = 2,
    FillBlack       = 3
};

} // namespace

RotateImageDialog::RotateImageDialog(const QSize &canvasSize,
                                     const QImage &previewSource,
                                     ResampleMethod method,
                                     const QColor &backgroundColor,
                                     QWidget *parent)
    : AdjustmentDialog(previewSource, tr("旋转图像"), parent)
    , m_canvasSize(canvasSize)
    , m_method(method)
    , m_backgroundColor(backgroundColor.isValid() ? backgroundColor : QColor(Qt::white))
{
    auto *paramArea = new QWidget(this);
    auto *grid = new QGridLayout(paramArea);
    grid->setContentsMargins(12, 12, 12, 4);
    grid->setHorizontalSpacing(8);

    // ── 第 1 行：角度 ──
    auto *angleLabel = new QLabel(tr("角度(&A)："), paramArea);

    m_angleSlider = new QSlider(Qt::Horizontal, paramArea);
    m_angleSlider->setObjectName(QStringLiteral("sliderAngle"));
    m_angleSlider->setRange(-kSliderMax, kSliderMax);
    m_angleSlider->setValue(0);
    m_angleSlider->setMinimumWidth(240);
    m_angleSlider->setPageStep(5 * kSliderPerDegree);   // PageUp/PageDown 走 5°
    m_angleSlider->setToolTip(tr("顺时针为正；拖动一格是 0.1°"));

    m_angleBox = new QDoubleSpinBox(paramArea);
    m_angleBox->setObjectName(QStringLiteral("spinAngle"));
    m_angleBox->setRange(-180.0, 180.0);
    m_angleBox->setDecimals(1);
    m_angleBox->setSingleStep(0.1);
    m_angleBox->setSuffix(tr(" °"));
    m_angleBox->setValue(0.0);
    m_angleBox->setToolTip(tr("顺时针为正。90 / 180 / -90 等整数倍走无损路径"));

    grid->addWidget(angleLabel,    0, 0);
    grid->addWidget(m_angleSlider, 0, 1);
    grid->addWidget(m_angleBox,    0, 2);

    // ── 第 2 行：信息面板 ──
    //
    // ★ 必须把"画布会变大"说出来 ★
    //   旋转后画布按包围盒扩大是**产品的既定行为**（不裁切内容），
    //   但用户看不到这条规则时会以为程序把他的图改大了、或者担心
    //   "多出来的地方会不会是黑的"。所以这里连填充面积占比一起报。
    m_infoLabel = new QLabel(paramArea);
    m_infoLabel->setTextFormat(Qt::RichText);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setFrameShape(QFrame::StyledPanel);
    m_infoLabel->setMargin(6);
    m_infoLabel->setMinimumWidth(380);

    grid->addWidget(m_infoLabel, 1, 0, 1, 3);

    // ── 第 3 行：扩展区域填充色 ──
    auto *fillLabel = new QLabel(tr("扩展区域(&E)："), paramArea);

    m_fillCombo = new QComboBox(paramArea);
    m_fillCombo->setObjectName(QStringLiteral("comboFill"));
    m_fillCombo->addItem(tr("透明（往下能看到背景）"));
    // 把实际颜色写进项文字 —— 只写"背景色"的话，用户得先猜背景色是什么，
    // 而这个对话框是模态的、没法同时去看工具栏上的色块。
    m_fillCombo->addItem(tr("背景色（%1）").arg(m_backgroundColor.name()));
    m_fillCombo->addItem(tr("白色（#ffffff）"));
    m_fillCombo->addItem(tr("黑色（#000000）"));
    m_fillCombo->setCurrentIndex(FillTransparent);

    grid->addWidget(fillLabel,   2, 0);
    grid->addWidget(m_fillCombo, 2, 1, 1, 2);
    grid->setColumnStretch(1, 1);

    // ── 接线 ──
    //
    // ★ 放在设初值之后 ★
    //   否则构造期的 setValue 会走进"用户编辑"流程。虽然
    //   AdjustmentDialog::notifyChanged() 有 m_loadingParams 挡着，
    //   但没必要依赖那层保护。
    connect(m_angleSlider, &QSlider::valueChanged, this, [this](int v) {
        // 挡住所同步的那一侧，保证"一次用户操作只产生一次预览请求"
        const QSignalBlocker block(m_angleBox);
        m_angleBox->setValue(v / qreal(kSliderPerDegree));
        onAngleChanged();
    });
    connect(m_angleBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
        const QSignalBlocker block(m_angleSlider);
        m_angleSlider->setValue(int(qRound(v * kSliderPerDegree)));
        onAngleChanged();
    });

    connect(m_fillCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RotateImageDialog::onFillModeChanged);

    // 初始化信息面板与"填充色是否可用"。
    // 此时基类的 m_loadingParams 仍为 true，因此不会白算一次预览。
    onAngleChanged();

    buildLayout(paramArea);
}

RotateImageDialog::~RotateImageDialog() = default;

// ---------------------------------------------------------------------------
//  参数查询
// ---------------------------------------------------------------------------
QColor RotateImageDialog::fillColor() const
{
    const int mode = m_fillCombo ? m_fillCombo->currentIndex() : FillTransparent;
    switch (mode) {
    case FillBackground: return m_backgroundColor;
    case FillWhite:      return QColor(Qt::white);
    case FillBlack:      return QColor(Qt::black);
    default:             return QColor(Qt::transparent);
    }
}

QSize RotateImageDialog::newCanvasSize() const
{
    if (m_canvasSize.isEmpty() || qFuzzyIsNull(m_angle))
        return m_canvasSize;

    // ★ 与 Document::rotateImageArbitrary 调的是同一个函数 ★
    //   对话框负责"说"，Document 负责"做"，说的和做的必须同源。
    return rotatedBoundsSize(m_canvasSize, m_angle);
}

QImage RotateImageDialog::apply(const QImage &src) const
{
    if (src.isNull())
        return QImage();

    if (qFuzzyIsNull(m_angle))
        return src.copy();

    return rotateImage(src, m_angle, m_method, fillColor());
}

bool RotateImageDialog::isChanged() const
{
    // ★ 只看角度，不看填充色 ★
    //   角度为 0 时旋转根本没发生，此时"改填充色"不构成一次可应用的改动 ——
    //   若把填充色也算进去，用户就会得到一条"什么都没变"的撤销记录。
    return !qFuzzyIsNull(m_angle);
}

void RotateImageDialog::resetParams()
{
    m_angleBox->setValue(0.0);                  // 同步滑块 + 刷新信息面板
    m_fillCombo->setCurrentIndex(FillTransparent);
}

// ---------------------------------------------------------------------------
//  槽
// ---------------------------------------------------------------------------
void RotateImageDialog::onAngleChanged()
{
    // ★ 角度以数字框为准 ★
    //   滑块只有 0.1° 的刻度，而数字框能接受键盘直接输入的任意一位小数，
    //   两者在四舍五入后可能差 0.05°。定一个真相，避免"读滑块得到 15.0、
    //   读数字框得到 15.04"这种要在多处判断口径的局面。
    m_angle = m_angleBox->value();

    m_infoLabel->setText(infoText());

    // ★ 90° 倍数时置灰填充色 ★
    //   那条路径走整数坐标重排，画布与内容等大、一个空像素都没有，
    //   填充色选了也不会生效。置灰比"选了没反应"诚实 ——
    //   后者会让用户以为填充色功能坏了。
    int turns = 0;
    const bool lossless = isRightAngle(m_angle, &turns);
    m_fillCombo->setEnabled(!lossless);
    m_fillCombo->setToolTip(lossless
        ? tr("90° 的整数倍是无损旋转，画布与内容等大，没有需要填充的区域")
        : tr("旋转后四角空出来的区域填充什么颜色"));

    notifyChanged();
}

void RotateImageDialog::onFillModeChanged(int /*index*/)
{
    // 填充色只在预览里体现，不改任何尺寸，所以只需请求重画
    notifyChanged();
}

// ---------------------------------------------------------------------------
//  信息面板
// ---------------------------------------------------------------------------
QString RotateImageDialog::infoText() const
{
    const QSize oldSize = m_canvasSize;
    const QSize newSize = newCanvasSize();

    QString html;
    html += tr("当前画布： %1 × %2 px").arg(oldSize.width()).arg(oldSize.height());
    html += QStringLiteral("<br>");

    if (qFuzzyIsNull(m_angle)) {
        html += tr("<b>未旋转</b>：画布与内容都不变。");
        html += QStringLiteral("<br>");
        html += tr("<span style='color:#666666'>输入角度后画布会自动扩大，"
                   "内容不会被裁切。</span>");
        return html;
    }

    int turns = 0;
    if (isRightAngle(m_angle, &turns)) {
        html += tr("<b>旋转后画布： %1 × %2 px</b>")
                    .arg(newSize.width()).arg(newSize.height());
        html += QStringLiteral("<br>");
        html += tr("<span style='color:#1f7a1f'>%1° 是 90° 的整数倍："
                   "整数坐标重排，<b>像素无损</b>，也没有需要填充的区域。</span>")
                    .arg(QString::number(m_angle, 'f', 1));
        return html;
    }

    // 填充区占比 = 1 − 内容面积 / 新画布面积。
    // 内容旋转后是斜的，四角必然空出来 —— 这个数字回答的是"图会变小多少"。
    const double oldArea = double(oldSize.width()) * double(oldSize.height());
    const double newArea = double(newSize.width()) * double(newSize.height());
    const double fillPercent = (newArea > 0.0)
                             ? 100.0 * (1.0 - oldArea / newArea)
                             : 0.0;

    html += tr("<b>旋转后画布： %1 × %2 px</b>")
                .arg(newSize.width()).arg(newSize.height());
    html += QStringLiteral("<br>");
    html += tr("<span style='color:#c86400'>画布自动扩大到能容纳旋转结果，"
               "内容不会被裁切；四角约 <b>%1%</b> 的面积需要填充。</span>")
                .arg(QString::number(fillPercent, 'f', 1));
    html += QStringLiteral("<br>");
    html += tr("<span style='color:#666666'>任意角度会重采样，<b>不可逆</b>："
               "转过去再转回来，像素回不到原样。</span>");
    return html;
}

} // namespace zl
