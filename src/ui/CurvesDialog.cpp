// ============================================================================
//  CurvesDialog.cpp
// ============================================================================

#include "ui/CurvesDialog.h"
#include "ui/CurveEditor.h"

#include "engine/Histogram.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace zl {

CurvesDialog::CurvesDialog(const QImage &source, QWidget *parent)
    : AdjustmentDialog(source, tr("曲线"), parent)
{
    // ── 左：曲线编辑器 ──
    m_editor = new CurveEditor(this);
    m_editor->setMinimumSize(280, 280);
    m_editor->setCurve(m_settings.curve(CurveChannel::Rgb));
    m_editor->setCurveColor(channelColor(CurveChannel::Rgb));

    // 背景直方图：打开对话框时算一次（约 30ms），之后按通道缓存。
    m_editor->setHistogram(histogramFor(CurveChannel::Rgb));

    connect(m_editor, &CurveEditor::curveChanged, this, &CurvesDialog::onEditorChanged);

    // ── 右：通道 / 预设 / 选项 ──
    auto *side = new QWidget(this);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);

    m_channelBox = new QComboBox(this);
    for (int i = 0; i < int(CurveChannel::Count); ++i)
        m_channelBox->addItem(curveChannelName(CurveChannel(i)));
    m_channelBox->setCurrentIndex(int(CurveChannel::Rgb));
    connect(m_channelBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CurvesDialog::onChannelChanged);
    form->addRow(tr("通道"), m_channelBox);

    m_presetBox = new QComboBox(this);
    // 第 0 项是占位项，不是预设 —— 用户选中某个预设后我们把它拨回这里。
    // ★ 为什么拨回去 ★
    //   预设一旦应用，曲线控制点就被"写死"成了那个形状，此后用户还会
    //   继续拖点微调。若下拉框仍显示"增强对比度"，用户会以为当前曲线
    //   就是标准预设，而实际已经不是了 —— 那是个会误导人的显示。
    m_presetBox->addItem(tr("预置…"));
    const QList<CurvePreset> presets = builtinPresets();
    for (int i = 0; i < presets.size(); ++i)
        m_presetBox->addItem(presets[i].name);
    connect(m_presetBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CurvesDialog::onPresetActivated);
    form->addRow(tr("预设"), m_presetBox);

    sideLayout->addLayout(form);

    m_smoothCheck = new QCheckBox(tr("平滑曲线"), this);
    m_smoothCheck->setChecked(true);
    m_smoothCheck->setToolTip(tr("关闭后按控制点直接连线（硬折线）。\n"
                                 "做色阶分离这类效果时需要它。"));
    connect(m_smoothCheck, &QCheckBox::toggled, this, &CurvesDialog::onSmoothToggled);
    sideLayout->addWidget(m_smoothCheck);

    m_hintLabel = new QLabel(tr(
        "左键空白处：新增控制点并拖动\n"
        "拖动控制点：改变映射\n"
        "右键控制点：删除（端点不可删）\n"
        "方向键：微调选中点（按住 Shift 步长 10）\n"
        "Delete：删除选中点"), this);
    m_hintLabel->setStyleSheet(QStringLiteral("color: #666666;"));
    sideLayout->addWidget(m_hintLabel);

    sideLayout->addStretch(1);

    // ── 组装 ──
    auto *params = new QWidget(this);
    auto *layout = new QHBoxLayout(params);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor, 1);
    layout->addWidget(side, 0);

    buildLayout(params);
    resize(620, 440);
}

// ---------------------------------------------------------------------------
//  纯函数：参数 → 图像
// ---------------------------------------------------------------------------

QImage CurvesDialog::apply(const QImage &src) const
{
    if (src.isNull())
        return src;

    // 四条曲线 → 三张合成 LUT → 查表。全部由 engine 完成。
    // 本对话框从不自己碰像素，因此"预览"与"最终结果"必然一致。
    return applyCurves(src, m_settings);
}

bool CurvesDialog::isChanged() const
{
    return !m_settings.isIdentity();
}

void CurvesDialog::resetParams()
{
    m_settings.reset();

    // 通道也回到 RGB —— 否则用户点了"复位"，看到的却是某个单通道曲线
    // 被清空了，而 RGB 主曲线还留在原处，会以为复位没生效。
    m_channel = CurveChannel::Rgb;
    if (m_channelBox)
        m_channelBox->setCurrentIndex(int(CurveChannel::Rgb));

    if (m_smoothCheck)
        m_smoothCheck->setChecked(true);

    pushCurveToEditor();
}

// ---------------------------------------------------------------------------
//  通道切换
// ---------------------------------------------------------------------------

void CurvesDialog::onChannelChanged(int index)
{
    if (index < 0 || index >= int(CurveChannel::Count))
        return;

    m_channel = CurveChannel(index);

    pushCurveToEditor();
}

void CurvesDialog::pushCurveToEditor()
{
    if (!m_editor)
        return;

    // 切换通道时编辑器要把新通道的曲线与直方图一起换掉：
    // 只换曲线不换直方图的话，用户会在红色通道的曲线上看着亮度分布调，
    // 那属于误导。
    m_editor->setCurve(m_settings.curve(m_channel));
    m_editor->setCurveColor(channelColor(m_channel));
    m_editor->setHistogram(histogramFor(m_channel));
}

QColor CurvesDialog::channelColor(CurveChannel ch) const
{
    switch (ch) {
    case CurveChannel::Red:   return QColor(200, 60, 60);
    case CurveChannel::Green: return QColor(40, 150, 60);
    case CurveChannel::Blue:  return QColor(50, 90, 210);
    case CurveChannel::Rgb:
    case CurveChannel::Count:
    default:                  return QColor(40, 40, 40);
    }
}

const QVector<int> &CurvesDialog::histogramFor(CurveChannel ch)
{
    const int idx = qBound(0, int(ch), int(CurveChannel::Count) - 1);

    if (!m_histValid[idx]) {
        const QImage &src = source();
        m_histCache[idx] = (ch == CurveChannel::Rgb)
                               ? histogram::luminance(src)
                               : histogram::singleChannel(src, idx - 1);
        m_histValid[idx] = true;
    }

    return m_histCache[idx];
}

// ---------------------------------------------------------------------------
//  预设与选项
// ---------------------------------------------------------------------------

void CurvesDialog::onPresetActivated(int index)
{
    if (index <= 0 || !m_editor)
        return;   // 占位项

    // 预设列表很短（个位数），重新取一遍的成本可以忽略；
    // 这样也省去了一个"与下拉框索引保持同步"的状态，少一处可能不同步的地方。
    const QList<CurvePreset> presets = builtinPresets();
    if (index - 1 >= presets.size())
        return;

    // ★ 预设只作用于**当前通道** ★
    //   Photoshop 的预置也是这个行为。若一下子把 RGB 与三个单通道全改掉，
    //   用户在其他通道上已经调好的东西会被无声地覆盖掉。
    m_settings.setCurve(m_channel, Curve(presets[index - 1].points));

    pushCurveToEditor();

    // 拨回占位项，理由见构造函数里的说明
    m_presetBox->setCurrentIndex(0);

    notifyChanged();
}

void CurvesDialog::onSmoothToggled(bool on)
{
    if (m_editor)
        m_editor->setSmooth(on);

    // 平滑/折线切换会改变实际映射（折线的段内是线性的），
    // 所以必须重算预览 —— 否则用户会看到"切了模式但画面没变"。
    notifyChanged();
}

void CurvesDialog::onEditorChanged()
{
    if (!m_editor)
        return;

    // 编辑器 → m_settings 的实时回写。
    // apply() 读的就是 m_settings，因此这里的即时性直接决定了
    // 预览的即时性。
    m_settings.setCurve(m_channel, m_editor->curve());

    notifyChanged();
}

} // namespace zl
