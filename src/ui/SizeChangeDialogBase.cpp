// ============================================================================
//  SizeChangeDialogBase.cpp —— 尺寸类对话框基类的实现
// ============================================================================

#include "ui/SizeChangeDialogBase.h"

#include "engine/Geometry.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtMath>

namespace zl {

SizeChangeDialogBase::SizeChangeDialogBase(const QSize &original,
                                           const QString &title,
                                           bool allowRatioLock,
                                           bool allowRelative,
                                           QWidget *parent)
    : QDialog(parent)
    , m_original(original)
    , m_target(original)
    , m_allowRatioLock(allowRatioLock)
    , m_allowRelative(allowRelative)
{
    setWindowTitle(title);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    // ★ 尺寸对话框不做原生对话框 ★
    //   与调整类对话框同理：原生外观与主窗口的中性灰界面割裂明显。
    setModal(true);
}

SizeChangeDialogBase::~SizeChangeDialogBase() = default;

bool SizeChangeDialogBase::isLockRatio() const
{
    return m_ratio && m_ratio->isChecked();
}

int SizeChangeDialogBase::widthInput() const
{
    return m_width ? m_width->value() : 0;
}

int SizeChangeDialogBase::heightInput() const
{
    return m_height ? m_height->value() : 0;
}

// ---------------------------------------------------------------------------
//  输入 ↔ 目标 的换算
// ---------------------------------------------------------------------------
//
//  这两个函数是"相对模式 + 单位"的全部复杂度所在。它们很平凡，但**必须**
//  只有这一处换算 —— 一旦别处也写一遍 `original + input` 或
//  `qRound(original * p / 100.0)`，就迟早会漏掉其中一种组合。
//
//  ★ 四格组合的完整定义（见头文件里的表）★
//
//       单位      模式      输入 n          → 目标像素
//       ─────────────────────────────────────────────────
//       像素      绝对      n               n
//       像素      相对      n               original + n
//       百分比    绝对      n               percentToPixels(n, original)
//       百分比    相对      n               percentToPixels(100 + n, original)
//
//    注意"相对 + 百分比"是把增量**先加回 100** 再走同一个换算函数，
//    而不是另写一套 `original * (100 + n) / 100`：两者看起来差不多，
//    但取整位置不同（先加后乘 vs 先乘后加），在 original 为奇数时
//    会差出一个像素 —— 而"差一个像素"正是这轮要防的那类 bug。
//
//  ★ 为什么最后还要 qBound 一道 ★
//    输入框的 range 已经按尺寸算好了（见 inputRangeFor），理论上不该越界。
//    但 setTargetSize()、长宽比联动算出的另一维、以及将来可能新增的
//    调用路径都会绕过 range —— 这一道钳制是"任何路径下 m_target 都合法"
//    这条不变式的最后保障，不是重复劳动。

int SizeChangeDialogBase::inputToTarget(int input, int original) const
{
    int value = 0;
    if (m_unit == Unit::Percent) {
        const int percent = (m_mode == Mode::Relative) ? (100 + input) : input;
        value = percentToPixels(percent, original);
    } else {
        value = (m_mode == Mode::Relative) ? (original + input) : input;
    }
    return qBound(1, value, kMaxCanvasDimension);
}

int SizeChangeDialogBase::targetToInput(int target, int original) const
{
    if (m_unit == Unit::Percent) {
        const int percent = pixelsToPercent(target, original);
        return (m_mode == Mode::Relative) ? (percent - 100) : percent;
    }
    return (m_mode == Mode::Relative) ? (target - original) : target;
}

// ---------------------------------------------------------------------------
//  单位 / 模式决定输入框的区间
// ---------------------------------------------------------------------------

void SizeChangeDialogBase::inputRangeFor(int original, int *lo, int *hi) const
{
    const int orig = qMax(1, original);

    if (m_unit == Unit::Percent) {
        // 目标像素恒在 [1, kMaxCanvasDimension] 内，把它换算成百分比的区间。
        //
        //  上界**向下取整**：这样"拉到头"的那个百分比换算回去仍然 ≤ 上限。
        //  若向上取整，用户把数字拨到最大反而会被回写成一个更小的值 ——
        //  一个"拨不到头"的滑块比一个窄区间更让人困惑。
        const int maxPct = qMax(1, (kMaxCanvasDimension * 100) / orig);

        //  下界取"最接近 1 像素的那个整数百分比"，且不小于 1%。
        //  1% 是这把尺子最细的一格：3000 像素宽的图，1% 就是 30 像素。
        //  想要 0.1% 这种精度请切回像素单位 —— 百分比本来就是粗略工具。
        const int minPct = qBound(1, (100 + orig / 2) / orig, maxPct);

        const int offset = (m_mode == Mode::Relative) ? 100 : 0;
        *lo = minPct - offset;
        *hi = maxPct - offset;
        return;
    }

    // 像素模式保持原有区间不变。
    //
    //  相对模式的负值故意留得很宽（-kMax），让 inputToTarget 去钳到 1 ——
    //  这样"用户输入 -500 → 输入框回写成真实生效的 -199"这条回写路径
    //  才有意义。若在 range 里就把它钳死，回写逻辑永远走不到，
    //  那条断言也就成了摆设（测试通过，但测的是 QSpinBox 而不是我们）。
    *lo = (m_mode == Mode::Relative) ? -kMaxCanvasDimension : 1;
    *hi = kMaxCanvasDimension;
}

void SizeChangeDialogBase::updateInputRanges()
{
    if (!m_width || !m_height)
        return;

    // ★ 改 range 必须阻断信号 ★
    //   setRange() 会把当前值钳进新区间，并因此**发出 valueChanged**。
    //   px 的 [1, 20000] 切到百分比的 [1, 500] 时，当前的 4000 会被钳成
    //   500 —— 那是一次"用户从未做出过的编辑"。值统一由紧随其后的
    //   writeInputs() 写入，这里只负责范围。
    const QSignalBlocker b1(m_width);
    const QSignalBlocker b2(m_height);

    int loW = 1, hiW = kMaxCanvasDimension;
    int loH = 1, hiH = kMaxCanvasDimension;
    inputRangeFor(m_original.width(),  &loW, &hiW);
    inputRangeFor(m_original.height(), &loH, &hiH);

    // ★ 两个维度分别算区间 ★
    //   宽高不等的图上，同一个百分比对应的像素数不同，能表达的范围也不同
    //   （4000 宽的图上 500% 就到上限了，3000 高的图上还能到 666%）。
    //   共用一个区间会让宽或高其中一边永远拨不到上限。
    m_width->setRange(loW, hiW);
    m_height->setRange(loH, hiH);

    const QString suffix = (m_unit == Unit::Percent) ? tr(" %") : tr(" px");
    m_width->setSuffix(suffix);
    m_height->setSuffix(suffix);
}

// ---------------------------------------------------------------------------
//  布局
// ---------------------------------------------------------------------------

void SizeChangeDialogBase::buildLayout(QWidget *extraBelowSize,
                                       QWidget *extraBottom)
{
    auto *main = new QVBoxLayout(this);
    main->setSpacing(10);

    // ── 尺寸输入 ──
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    // 让输入框所在列吸收多余宽度，标签与勾选框贴边
    grid->setColumnStretch(1, 1);

    auto *widthLabel = new QLabel(tr("Width(&W):"), this);
    m_width = new QSpinBox(this);
    m_width->setObjectName(QStringLiteral("spinWidth"));
    m_width->setSuffix(tr(" px"));
    m_width->setMinimumWidth(120);
    m_width->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    widthLabel->setBuddy(m_width);

    auto *heightLabel = new QLabel(tr("Height(&H):"), this);
    m_height = new QSpinBox(this);
    m_height->setObjectName(QStringLiteral("spinHeight"));
    m_height->setSuffix(tr(" px"));
    m_height->setMinimumWidth(120);
    m_height->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    heightLabel->setBuddy(m_height);

    grid->addWidget(widthLabel,  0, 0);
    grid->addWidget(m_width,     0, 1);
    grid->addWidget(heightLabel, 1, 0);
    grid->addWidget(m_height,    1, 1);

    // 两个勾选框叠在第三列。只存在一个时也保持贴顶 ——
    // 这样两个对话框在切换时，输入框的纵向位置完全一致。
    auto *checkColumn = new QVBoxLayout;
    checkColumn->setSpacing(6);
    checkColumn->setContentsMargins(8, 0, 0, 0);

    if (m_allowRatioLock) {
        m_ratio = new QCheckBox(tr("Keep Aspect Ratio(&R)"), this);
        m_ratio->setObjectName(QStringLiteral("checkRatio"));
        m_ratio->setChecked(false);
        m_ratio->setToolTip(tr("When checked, changing one size makes the other follow at the original ratio"));
        connect(m_ratio, &QCheckBox::toggled, this, &SizeChangeDialogBase::onRatioToggled);
        checkColumn->addWidget(m_ratio);
    }

    if (m_allowRelative) {
        m_relative = new QCheckBox(tr("Relative(&L)"), this);
        m_relative->setObjectName(QStringLiteral("checkRelative"));
        m_relative->setChecked(false);
        m_relative->setToolTip(tr("When checked, the numbers in the boxes are "
                                  "<b>offsets</b> (positive or negative), not "
                                  "target sizes"));
        connect(m_relative, &QCheckBox::toggled, this, &SizeChangeDialogBase::onRelativeToggled);
        checkColumn->addWidget(m_relative);
    }

    checkColumn->addStretch(1);
    grid->addLayout(checkColumn, 0, 2, 2, 1);
    grid->setColumnStretch(2, 0);

    // ── 单位 ──
    //
    //  ★ 为什么单占一行、而不是挤到输入框右边 ★
    //    QSpinBox 的右侧已经被两个勾选框占住（它们必须在同一列，否则两个
    //    对话框互相切换时输入框会整体左右跳动）。再往那一列里塞一个下拉框，
    //    这一行就谁都看不清了。单位单占一行、与上面的输入框左对齐，
    //    反而一眼能看出"它同时作用于宽度和高度两个数字"。
    auto *unitLabel = new QLabel(tr("Unit(&U):"), this);

    m_unitCombo = new QComboBox(this);
    m_unitCombo->setObjectName(QStringLiteral("comboUnit"));
    // 携带枚举值而不是靠下标 —— 与重采样下拉框同样的做法：将来往 Unit 里
    // 插一项（例如今后支持英寸/cm，见 docs/…第 6.2 节的单位规划）时，
    // 解析端不必跟着改下标。
    m_unitCombo->addItem(tr("Pixels (px)"),   int(Unit::Pixels));
    m_unitCombo->addItem(tr("Percent (%)"),   int(Unit::Percent));
    m_unitCombo->setCurrentIndex(0);
    m_unitCombo->setToolTip(tr(
        "Unit for the numbers in the input boxes.\n"
        "· Pixels: enter a pixel count directly, precise to 1 pixel (default).\n"
        "· Percent: a ratio relative to the **original size**; 100% keeps the original size.\n"
        "  When \"Relative\" is checked it means an incremental percentage (50% = grow by half).\n"
        "Percentages are integers, and on very small sizes a 1-pixel difference is indistinguishable;\n"
        "use pixels when you need precise control."));
    unitLabel->setBuddy(m_unitCombo);

    grid->addWidget(unitLabel,  2, 0);
    grid->addWidget(m_unitCombo, 2, 1);

    main->addLayout(grid);

    // ── 子类的附加控件 ──
    if (extraBelowSize)
        main->addWidget(extraBelowSize);

    // ── 信息面板 ──
    //
    //  用 QFrame 加边框而不是裸 QLabel：这块内容是"计算结果"而非"输入"，
    //  视觉上需要与上面的控件区分开。两个对话框共用同一块，切换时
    //  位置与高度不会跳动。
    auto *infoFrame = new QFrame(this);
    infoFrame->setObjectName(QStringLiteral("frameInfo"));
    infoFrame->setFrameShape(QFrame::StyledPanel);
    infoFrame->setFrameShadow(QFrame::Sunken);
    infoFrame->setMinimumHeight(56);

    auto *infoLayout = new QVBoxLayout(infoFrame);
    infoLayout->setContentsMargins(10, 8, 10, 8);

    m_info = new QLabel(infoFrame);
    m_info->setObjectName(QStringLiteral("labelInfo"));
    m_info->setTextFormat(Qt::RichText);
    m_info->setWordWrap(true);
    m_info->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    infoLayout->addWidget(m_info);

    main->addWidget(infoFrame);

    if (extraBottom)
        main->addWidget(extraBottom);

    main->addStretch(1);

    // ── 按钮 ──
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    m_okButton->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));

    auto *resetButton = buttons->addButton(tr("Reset(&E)"), QDialogButtonBox::ResetRole);
    // ★ 必须自己起名字 ★
    //   用 addButton(text, role) 加的自定义按钮，其 standardButton() 并没有
    //   被设成 QDialogButtonBox::Reset，于是 button(QDialogButtonBox::Reset)
    //   取回来是 nullptr（实测 Qt 5.15 如此）。自动化测试只能靠 objectName
    //   定位 —— 这也正是本工程"不给控件起名就等于不可测"那条约定的由来。
    resetButton->setObjectName(QStringLiteral("buttonReset"));
    connect(resetButton, &QPushButton::clicked, this, [this] {
        setTargetSize(m_original);
    });

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    main->addWidget(buttons);

    // ── 完成初始化 ──
    //
    //  顺序很重要：先设范围（否则下面 writeInputs 的值会被钳到默认的
    //  0..99），再回写数值，最后才开始响应信号。
    updateInputRanges();

    writeInputs();
    refreshInfo();
    onSizeEdited(m_target);

    // ── 接线 ──
    //
    // ★ 放在最后接 ★
    //   上面 writeInputs() 会写一次控件值；如果先接了线，那次写入就会
    //   触发一轮"用户编辑"流程，而那时 m_loading 还是 true ——
    //   虽然会被挡住，但等于把"初始化期间静默"这条约定又依赖了一遍。
    //   干脆先做完初始化再接线，让 m_loading 只用来保护复位路径。
    connect(m_width, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SizeChangeDialogBase::onWidthEdited);
    connect(m_height, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SizeChangeDialogBase::onHeightEdited);
    //  QOverload 是必须的：Qt5 的 currentIndexChanged 有 int 与 QString
    //  两个重载，不指明就二义。Qt6 只剩 int 一个，写法照样成立。
    connect(m_unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SizeChangeDialogBase::onUnitChanged);

    m_loading = false;
}

// ---------------------------------------------------------------------------
//  用户编辑
// ---------------------------------------------------------------------------

void SizeChangeDialogBase::onWidthEdited(int value)
{
    if (m_writing || m_loading)
        return;

    // ★ 先算出新的目标尺寸，再回写视图 ★
    //   这一步就是本文件开头说的"单一真相"：m_target 永远先被更新到
    //   合法状态，输入框只是它的显示。
    m_target.setWidth(inputToTarget(value, m_original.width()));

    // 锁定长宽比时，另一维由**用户没在编辑的那一维**决定。
    //
    //  注意这里用的是 m_original 的比例，而不是"上一次的 target 比例"。
    //  后者会让比例在反复编辑中缓慢漂移（每次都取整一次），
    //  最后用户会发现"勾着保持长宽比，改了几次宽度后比例变了"。
    if (isLockRatio() && m_original.width() > 0 && m_original.height() > 0) {
        const int h = qRound(qreal(m_target.width()) * m_original.height()
                             / m_original.width());
        m_target.setHeight(qBound(1, h, kMaxCanvasDimension));
    }

    writeInputs();
    refreshInfo();
    onSizeEdited(m_target);
    emit sizeChanged(m_target);
}

void SizeChangeDialogBase::onHeightEdited(int value)
{
    if (m_writing || m_loading)
        return;

    m_target.setHeight(inputToTarget(value, m_original.height()));

    if (isLockRatio() && m_original.width() > 0 && m_original.height() > 0) {
        const int w = qRound(qreal(m_target.height()) * m_original.width()
                             / m_original.height());
        m_target.setWidth(qBound(1, w, kMaxCanvasDimension));
    }

    writeInputs();
    refreshInfo();
    onSizeEdited(m_target);
    emit sizeChanged(m_target);
}

void SizeChangeDialogBase::onRatioToggled(bool /*on*/)
{
    if (m_loading)
        return;

    // ★ 勾选/取消勾选都**不改动**当前尺寸 ★
    //   用户只是改变"以后编辑时另一个要不要跟随"。若在这里顺手把高度
    //   对齐到比例，用户的数字会在一次无关操作中被改掉 —— 那是很难
    //   解释的行为。约束从下一次编辑开始生效。
    refreshInfo();
    onSizeEdited(m_target);
    emit sizeChanged(m_target);
}

void SizeChangeDialogBase::onRelativeToggled(bool on)
{
    if (m_loading)
        return;

    m_mode = on ? Mode::Relative : Mode::Absolute;

    // ★ 切换模式必须换算输入框的数字，而不是原样保留 ★
    //   "宽 4400"（绝对）与"宽 4400"（相对）含义完全不同：
    //   后者表示新宽 = 原宽 + 4400 = 8400。原样保留会让用户在勾选
    //   「相对」的瞬间看到目标尺寸暴涨，而自己什么都没输入。
    //
    //   真相是 m_target，它**不变** —— 变的只是它的显示方式。
    //   这正好是本文件"状态存 m_target"那个设计的直接收益。
    //
    //   单位也要一起重生（相对模式的百分比区间是 [minPct-100, maxPct-100]，
    //   与绝对模式不同），所以走的是同一个 updateInputRanges()。
    updateInputRanges();

    writeInputs();
    refreshInfo();
    onSizeEdited(m_target);
    emit sizeChanged(m_target);
}

void SizeChangeDialogBase::onUnitChanged(int index)
{
    if (m_loading || !m_unitCombo)
        return;

    const Unit unit = static_cast<Unit>(m_unitCombo->itemData(index).toInt());
    if (unit == m_unit)
        return;

    m_unit = unit;

    // ★ 与切换「相对」同一条规矩：目标尺寸一个像素都不动 ★
    //
    //   单位是**显示方式**，不是尺寸本身。用户把「像素 2000」拨到百分比，
    //   看到的应该是 "50 %"（原宽 4000），而「确定」按钮仍然保持置灰 ——
    //   因为尺寸压根没变。若在这里顺手"换算一下再塞回去"，用户就会在
    //   拨动一个纯显示开关的时候把图真的改掉，而撤销栈里会多出一条
    //   他自己都不明白是怎么来的记录。
    updateInputRanges();   // 两种单位的区间完全不同，必须先松范围
    writeInputs();
    refreshInfo();
    onSizeEdited(m_target);
    emit sizeChanged(m_target);
}

void SizeChangeDialogBase::setTargetSize(const QSize &size)
{
    m_target.setWidth(qBound(1, size.width(),  kMaxCanvasDimension));
    m_target.setHeight(qBound(1, size.height(), kMaxCanvasDimension));

    writeInputs();
    refreshInfo();
    onSizeEdited(m_target);
    emit sizeChanged(m_target);
}

// ---------------------------------------------------------------------------
//  回写与刷新
// ---------------------------------------------------------------------------

void SizeChangeDialogBase::writeInputs()
{
    // ★ 显式重入标志 + QSignalBlocker，双保险 ★
    //   QSignalBlocker 挡住的是"信号被发出"，但 valueChanged 之外
    //   还可能有别的路径回到这里（例如将来给输入框加 validator）。
    //   m_writing 保证的是"回写期间不做任何用户编辑处理"，与信号机制
    //   无关 —— 这两件事的职责本来就不一样。
    m_writing = true;

    {
        const QSignalBlocker b1(m_width);
        const QSignalBlocker b2(m_height);

        m_width->setValue(targetToInput(m_target.width(),  m_original.width()));
        m_height->setValue(targetToInput(m_target.height(), m_original.height()));
    }

    m_writing = false;
}

void SizeChangeDialogBase::refreshInfo()
{
    if (m_info)
        m_info->setText(infoText(m_target));

    // ★ "确定"按钮在尺寸没变时置灰 ★
    //
    //  不是纯粹的防呆：尺寸类操作会往撤销栈压入**所有图层的**前后位图，
    //  一步可能就是几百 MB。用户打开对话框看一眼就点确定，不该留下
    //  一条 96MB 的记录。所以在这里挡一道，MainWindow 那边再挡一道 ——
    //  UI 层不允许出现"点了确定但什么都没发生还不解释"的状态。
    if (m_okButton) {
        m_okButton->setEnabled(isChanged());
        m_okButton->setToolTip(isChanged()
            ? tr("Apply the new size")
            : tr("The size has not changed - nothing to apply"));
    }
}

} // namespace zl
