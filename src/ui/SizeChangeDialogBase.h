#pragma once

// ============================================================================
//  SizeChangeDialogBase.h —— 尺寸类对话框的公共基类
// ----------------------------------------------------------------------------
//  "图像大小"与"画布大小"两个对话框有大量共同之处，把它们抽到这里：
//
//    · 宽 / 高的成对数值输入（含范围、步长、单位后缀）
//    · 目标尺寸的**单一真相**（m_target）—— 输入框只是它的视图
//    · 长宽比锁定 / 相对模式（增量输入）/ 单位（像素 / 百分比）三种约束
//    · 一块信息面板（"当前 4000 × 3000 (12.0 MP) → 新 …"）
//    · 确定 / 取消
//
//  ★ 核心设计：状态存在 m_target 里，而不是从控件反推 ★
//
//    最容易写出 bug 的写法是"每次需要尺寸时去读两个 QSpinBox 的值"。
//    一旦引入约束（长宽比锁定、相对模式、下限钳制），就必须回答
//    "用户改的是宽还是高"，而这个信息在读值的那一刻已经丢了 ——
//    于是只能靠 _lastEdited 之类的标志位猜，信号回路也防不住。
//
//    改成：用户操作 → 先算出**新的目标尺寸** → 再回写两个输入框。
//    在任何时刻，m_target 都是唯一、完整、已经满足约束的状态；
//    输入框只是它的一个显示。回写时用 QSignalBlocker 阻断回路，
//    所以"一次用户操作 = 一次 sizeChanged()"，不会有第二次。
//
//  ★ 相对模式为什么放在基类 ★
//
//    "相对"不是画布大小独有的 —— 它改的是**输入框的数字含义**
//    （目标值 vs 增量），而输入框本身在基类里。如果把相对模式放到
//    子类，子类就得反过来操纵基类的私有控件，或者基类要暴露一堆
//    内部细节。让基类支持"可选的相对模式"，子类只要在构造时声明，
//    是这个设计下最干净的切法。
//
//  ★ 单位（像素 / 百分比）同理，而且更彻底 ★
//
//    单位、相对模式、长宽比锁定 —— 这三样都是**视图层的修饰**：
//    它们改变的是"同一个目标尺寸该怎么显示/该怎么由输入反推"，
//    而不是目标尺寸本身。所以三者的切换必须遵守同一条规矩：
//    **只换显示，不动 m_target**。用户在「像素 4000」时切到百分比，
//    看到 "100 %" 而"确定"按钮仍保持置灰 —— 因为尺寸压根没变。
//    如果这里偷懒直接把数字换算一下塞回去，用户就会在拨动一个
//    纯粹用于显示的开关时，把图真的改掉。
//
//  本文件属于 ui 层，可以自由使用 QtWidgets。
// ============================================================================

#include <QDialog>
#include <QSize>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace zl {

// 尺寸对话框允许的最大边长。
//
//  为什么需要一个上限：QImage 在多数格式下的边长上限是 32767，但真正的
//  约束是内存 —— 20000×20000 的 ARGB32 就是 1.6GB，多图层直接崩。
//  上限取 20000 是"常见工程上限"与"还能分配出来"之间的折中；真正拦不住
//  的组合（例如 20000×20000 且图层多）由 Document 的返回值与
//  下方的内存预警文字来提示，而不是靠这个常量硬挡。
constexpr int kMaxCanvasDimension = 20000;

class SizeChangeDialogBase : public QDialog
{
    Q_OBJECT

public:
    // 输入框里数字的含义。
    enum class Mode {
        Absolute,   // 目标尺寸（"宽度 4400"表示新画布宽 4400）
        Relative    // 增量（"宽度 400"表示新画布宽 = 原宽 + 400）
    };

    // 输入框的计量单位。
    //
    //  百分比**始终相对原尺寸**，与 Mode 正交，四种组合的含义：
    //
    //  ┌────────┬──────────────────────┬──────────────────────────┐
    //  │        │ 绝对（目标）          │ 相对（增量）              │
    //  ├────────┼──────────────────────┼──────────────────────────┤
    //  │ 像素    │ 新宽 = n px          │ 新宽 = 原宽 + n px        │
    //  │ 百分比  │ 新宽 = 原宽 × n%     │ 新宽 = 原宽 × (100 + n)%  │
    //  └────────┴──────────────────────┴──────────────────────────┘
    //
    //  注意右下角那格："相对 + 百分比"里 n = 50 表示"在原有基础上加五成"，
    //  它与"绝对 + 150%"是同一个结果 —— 就像像素模式下"相对 + 400"与
    //  "绝对 4400"（原宽 4000）是同一个结果一样。两格只是同一目标值的
    //  两种记法，不存在两套算法。
    enum class Unit {
        Pixels,     // 像素（默认 —— 需要精确到像素时唯一的单位）
        Percent     // 百分比（相对原尺寸；整数，1% 起）
    };

    // original —— 打开对话框那一刻的画布尺寸。同时是长宽比的来源。
    // allowRatioLock —— 是否提供"保持长宽比"勾选框（图像大小用）。
    // allowRelative  —— 是否提供"相对"勾选框（画布大小用）。
    //                   两者可以同时为 false，也可以同时为 true。
    SizeChangeDialogBase(const QSize &original,
                         const QString &title,
                         bool allowRatioLock,
                         bool allowRelative,
                         QWidget *parent = nullptr);
    ~SizeChangeDialogBase() override;

    // ── 供调用方查询 ──

    QSize originalSize() const { return m_original; }

    // 当前的目标尺寸。**任何时刻都满足**：两个分量都在
    // [1, kMaxCanvasDimension] 内；若已锁定长宽比，则由**用户编辑驱动**
    // 的那一维决定另一维，使二者符合原始比例。
    //
    //  「锁定长宽比」只在编辑时生效，勾选/取消勾选本身不改动尺寸 ——
    //  见 .cpp 里 onRatioToggled() 的说明。
    QSize newSize() const { return m_target; }

    Mode mode() const { return m_mode; }
    Unit unit() const { return m_unit; }
    bool isLockRatio() const;

    // 输入框当前显示的数字（宽度/高度）。
    //
    //  只给测试与"回写是否稳定"这类断言用 —— 业务代码要的是 newSize()。
    //  之所以值得开放：单位与模式的四次组合里，"显示值 ⇄ 目标值"的往返
    //  是否稳定，是唯一能靠断言盯住的契约（见 ui_smoke 的单位那一组）。
    int widthInput()  const;
    int heightInput() const;

    // 目标尺寸是否偏离了原始尺寸。
    //
    // ★ 调用方必须用它来决定"要不要压入撤销栈" ★
    //   用户打开对话框、什么都没改就点确定，不应该在历史面板里留下一条
    //   记录 —— 一条"图像大小"记录背后可能是几百 MB 的图层快照。
    //   基类据此把"确定"按钮置灰，双保险。
    bool isChanged() const { return m_target != m_original; }

signals:
    // 目标尺寸或约束发生变化。
    // 子类用它刷新自己的信息面板 / 预览示意图 / 附加控件状态。
    void sizeChanged(const QSize &newSize);

protected:
    // 子类在构造函数**末尾**调用。
    //
    // extraBelowSize —— 插在尺寸输入行与信息面板之间的控件
    //                   （重采样方式、锚点九宫格、扩展颜色…）
    // extraBottom    —— 插在信息面板之下的控件（通常不用）
    //
    // 由基类负责整体布局，保证两个对话框的间距、按钮位置、信息面板
    // 外观一致 —— 而不是各调各的（那样迟早会出现"这个对话框的按钮
    // 比那个低两像素"的累积漂移）。
    void buildLayout(QWidget *extraBelowSize = nullptr,
                     QWidget *extraBottom = nullptr);

    // 子类实现：信息面板要显示的文字。
    //
    // ★ 返回的是**富文本**（QLabel 已设为 Qt::RichText）★
    //   因为这块内容不只是数字，还要按情况改颜色：尺寸变大变小的
    //   提示、被裁切的警告、发生智能适配平移的说明 —— 用统一的
    //   灰色文字报"内容将被裁掉 120 像素"是不负责任的。
    //   允许包含 <br> 换行。
    virtual QString infoText(const QSize &newSize) const = 0;

    // 子类可选实现：尺寸变化后的额外处理（刷新预览示意图等）。
    // 基类在写完 m_target、回写完输入框**之后**调用，所以此时
    // newSize() 已经是最终值，子类可以直接读。
    virtual void onSizeEdited(const QSize &newSize) { Q_UNUSED(newSize) }

    // 子类可用：在程序控制下改变尺寸（同样走约束与回写逻辑）。
    // 用来实现"点了某个按钮就把尺寸设成某个值"这类交互。
    void setTargetSize(const QSize &size);

    QSpinBox  *widthBox()  const { return m_width; }
    QSpinBox  *heightBox() const { return m_height; }
    QCheckBox *ratioCheck() const { return m_ratio; }
    QCheckBox *relativeCheck() const { return m_relative; }
    QComboBox *unitCombo() const { return m_unitCombo; }
    QLabel    *infoLabel() const { return m_info; }

    // 重新生成信息面板的文字并刷新"确定"按钮状态。
    //
    //  基类在尺寸变化后会自动调用；子类在自己**独有的**状态发生变化时
    //  也要调用它 —— 例如画布大小对话框切换了九宫格锚点，此时尺寸没变，
    //  但"内容将被裁掉多少"完全变了。
    void refreshInfo();

private slots:
    void onWidthEdited(int value);
    void onHeightEdited(int value);
    void onRatioToggled(bool on);
    void onRelativeToggled(bool on);
    void onUnitChanged(int index);

private:
    // 把目标尺寸写回输入框（阻断信号，不会引发新的编辑）。
    void writeInputs();

    // 按当前的 (单位, 模式) 设置两个输入框的 range 与 suffix。
    //  在每次切换单位/模式**之前**调用（先松范围再写值，否则新值会被
    //  旧范围钳掉），并由 buildLayout() 在初始化末尾调用一次。
    void updateInputRanges();

    // 某个维度在当前的 (单位, 模式) 下，输入框允许的区间。
    //  百分比的上下界是**按原尺寸算出来的**：3000 像素宽的图，输入框的
    //  上限就是 20000/3000 ≈ 666%（再多也会被 kMaxCanvasDimension 钳住，
    //  与其让用户输入一个必然被改写回的数字，不如一开始就拨不过去）。
    void inputRangeFor(int original, int *lo, int *hi) const;

    // 输入框的数字 → 目标尺寸的一个分量
    int inputToTarget(int input, int original) const;
    // 目标尺寸的一个分量 → 输入框的数字
    int targetToInput(int target, int original) const;

    QSize  m_original;
    QSize  m_target;
    Mode   m_mode = Mode::Absolute;
    Unit   m_unit = Unit::Pixels;

    bool    m_allowRatioLock = true;
    bool    m_allowRelative  = false;

    QSpinBox    *m_width     = nullptr;
    QSpinBox    *m_height    = nullptr;
    QCheckBox   *m_ratio     = nullptr;
    QCheckBox   *m_relative  = nullptr;
    QComboBox   *m_unitCombo = nullptr;
    QLabel      *m_info      = nullptr;
    QPushButton *m_okButton  = nullptr;

    // ★ 重入保护 ★
    //   writeInputs() 会改输入框的值；虽然用了 QSignalBlocker，但如果
    //   将来有人在 onWidthEdited 里加了别的控件写入，仍可能绕回来。
    //   用一个显式的标志位，"回写期间的用户编辑"一律忽略 —— 这比
    //   依赖"Qt 在值不变时不发信号"这种实现细节要稳。
    bool m_writing = false;

    // 构造与 buildLayout 期间为 true：此时控件还没建完，
    // 任何信号都不该触发刷新（会在半初始化状态上做计算）。
    bool m_loading = true;
};

} // namespace zl
