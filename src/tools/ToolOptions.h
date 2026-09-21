#pragma once

// ============================================================================
//  ToolOptions.h —— 工具参数体系
// ----------------------------------------------------------------------------
//  解决的问题：12 个工具各有自己的参数（画笔大小、容差、线宽、字体…）。
//  如果每个工具自己写一套参数界面，代码会重复且风格不统一。
//
//  统一机制：每个工具持有一个 ToolOptions 子类对象，
//  ToolOptionsBar 在工具切换时调用 createWidget() 重建选项条。
//
//  ★ 参数变化用信号通知，工具不持有控件指针 ★
//    如果工具直接持有 QSpinBox*，那么工具层（tools/）就被迫 include
//    QtWidgets 的头文件，同时产生"控件销毁后工具还拿着野指针"的风险。
//    用信号则天然避免这两个问题。
//
//  ★★ 分层纪律：枚举类型不放在这个头文件里 ★★
//
//    ToolOptions.h 必须 include QtWidgets（因为 createWidget 返回 QWidget*），
//    而 AbstractTool.h 又要 include ToolOptions.h。
//    如果各种参数枚举（选区模式、形状类型等）定义在这里，那么任何
//    include 了抽象工具头的文件都会间接拖进 QtWidgets ——
//    包括那些本该纯净的、只做数据计算的单元测试。
//
//    更严重的是：一旦有人为了"省事"而在 engine/ 里 include 了带本文件
//    的传递路径，engine 层"只依赖 QtGui"的架构铁律就被破坏了，
//    而且这种破坏是隐性的、编译期不会报错的。
//
//    所以：**所有参数枚举统一定义在 ToolParams.h（零 QtWidgets 依赖）**，
//    本文件只引用它们。加新枚举时请加到 ToolParams.h，不要加在这里。
// ============================================================================

#include "tools/ToolParams.h"

#include <QObject>
#include <QString>
#include <QColor>
#include <QSize>

class QWidget;
class QSettings;

namespace zl {

// ============================================================================
//  ToolOptions —— 参数对象基类
// ============================================================================
//  继承关系：ToolOptions 本身是 QObject，这样子类才能定义自己的信号
//  （参数变化信号）。代价是不能用 std::unique_ptr 的默认删除器拷贝语义，
//  但工具持有的本来就是裸指针或 unique_ptr，没有影响。
// ============================================================================
class ToolOptions : public QObject
{
    Q_OBJECT

public:
    explicit ToolOptions(QObject *parent = nullptr) : QObject(parent) {}
    ~ToolOptions() override = default;

    // ── 界面构建 ──
    //
    // 创建选项条控件。调用时机：工具被激活时，由 ToolOptionsBar 调用。
    //
    // ★ 所有权约定 ★
    //   返回的控件 parent 必须设为传入的 parent（或由调用方 setParent），
    //   也就是"调用方接管所有权"。这样工具切换时旧控件能被正确销毁。
    //   ToolOptions 对象本身**不持有**这个控件 —— 控件是短命的，
    //   参数对象是长命的（跨工具切换保留用户设置）。
    //
    // 返回 nullptr 表示本工具没有选项条。
    virtual QWidget *createWidget(QWidget *parent) = 0;

    // ── 持久化 ──
    //
    // 参数持久化的意义：用户上次把画笔调成 37 像素、硬度 60%，
    // 下次启动程序时应该还是这个值。不持久化的话每次都要重调，很烦。
    //
    // 实现约定：load/save 内部用 QSettings 的 beginGroup(id()) 分组，
    // 避免不同工具的参数键名冲突（例如画笔和橡皮都有 "size"）。
    virtual void load(QSettings &settings) = 0;
    virtual void save(QSettings &settings) const = 0;

    // 参数分组名（QSettings 的 group）。默认用工具 id。
    // 橡皮与画笔可以共用同一个分组，这样"从画笔切到橡皮"时尺寸不会突变。
    virtual QString settingsGroup() const { return m_settingsGroup; }
    void setSettingsGroup(const QString &group) { m_settingsGroup = group; }

    // ── 变化通知 ──
    //
    // 任何参数变化后都必须调用它。工具连接到这个信号后重建内部缓存
    // （例如画笔重建笔刷蒙版、形状工具重建预览）。
    void notifyChanged() { emit changed(); }

signals:
    // 参数发生变化。**泛化的**信号 —— 不区分改了哪个参数。
    //
    // 为什么不做成"每个参数一个信号"：
    //   画笔有 5 个参数，形状工具有 7 个，加起来几十个信号，全是样板代码。
    //   而工具的响应动作几乎总是相同的 —— "重建缓存"或"什么都不做"。
    //   一个泛化信号完全够用，且新增参数时不需要加信号。
    void changed();

private:
    QString m_settingsGroup;
};

// ============================================================================
//  各工具的选项类 —— 声明
// ============================================================================
//  实现放在 ToolOptions.cpp 里（因为要用到 QtWidgets 的布局与控件）。
//  这里只给声明，让工具头文件不必包含任何 QtWidgets 内容 ——
//  工具头文件只需要 "class ToolOptions;" 的前置声明即可。
//
//  ★ 各参数对象之间用信号槽互通 ★
//    例如"吸管取色"要更新"画笔颜色"，实现方式是 MainWindow 把
//    吸管的 colorPicked 信号连到画笔选项的 setColor 槽，
//    而不是让吸管直接持有画笔的参数对象（那会造成工具间的耦合）。
// ============================================================================

// ── 画笔 / 橡皮 ──────────────────────────────────────────────────────────
// 参数：大小(1-500)、硬度(0-100%)、不透明度(0-100%)、流量(1-100%)、颜色
class BrushOptions : public ToolOptions
{
    Q_OBJECT

public:
    enum class Kind { Brush, Eraser };

    explicit BrushOptions(Kind kind, QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    // ── 参数访问 ──
    int    size() const { return m_size; }                 // 直径（像素）
    int    hardness() const { return m_hardness; }         // 0-100
    int    opacity() const { return m_opacity; }           // 0-100
    int    flow() const { return m_flow; }                 // 0-100
    QColor color() const { return m_color; }

    void setSize(int v);
    void setHardness(int v);
    void setOpacity(int v);
    void setFlow(int v);
    void setColor(const QColor &c);

    // 半径（浮点，供笔刷蒙版生成用）
    qreal radius() const { return m_size / 2.0; }

    // 设置值的上下限（画笔 1-500，橡皮可以更小）
    void setSizeRange(int min, int max);

signals:
    // 颜色变化单独发一个信号：因为吸管取色时只关心颜色，
    // 若只发泛化的 changed()，画笔会重建整个蒙版，浪费。
    void colorChanged(const QColor &color);

private:
    Kind   m_kind;
    int    m_size     = 20;
    int    m_hardness = 100;
    int    m_opacity  = 100;
    int    m_flow     = 100;
    QColor m_color    = Qt::black;
    int    m_minSize  = 1;
    int    m_maxSize  = 500;
};

// ── 油漆桶 ───────────────────────────────────────────────────────────────
// 参数：容差(0-255)、抗锯齿、连续/非连续、不透明度、颜色
class BucketOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit BucketOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    int  tolerance() const { return m_tolerance; }
    bool antialias() const { return m_antialias; }
    bool contiguous() const { return m_contiguous; }
    int  opacity() const { return m_opacity; }
    QColor color() const { return m_color; }

    void setTolerance(int v);
    void setAntialias(bool v);
    void setContiguous(bool v);
    void setOpacity(int v);
    void setColor(const QColor &c);

signals:
    void colorChanged(const QColor &color);

private:
    int    m_tolerance  = 32;
    bool   m_antialias  = true;
    bool   m_contiguous = true;
    int    m_opacity    = 100;
    QColor m_color      = Qt::black;
};

// ── 吸管 ─────────────────────────────────────────────────────────────────
// 参数：取样范围（点 / 3×3 平均 / 5×5 平均）
class PickerOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit PickerOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    PickRadius radius() const { return m_radius; }
    void setRadius(PickRadius r);

private:
    PickRadius m_radius = PickRadius::Point;
};

// ── 直线 ─────────────────────────────────────────────────────────────────
// 参数：线宽、线型、颜色、抗锯齿
class LineOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit LineOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    int         width() const { return m_width; }
    Qt::PenStyle style() const { return m_style; }
    QColor      color() const { return m_color; }
    bool        antialias() const { return m_antialias; }

    void setWidth(int v);
    void setStyle(Qt::PenStyle s);
    void setColor(const QColor &c);
    void setAntialias(bool v);

signals:
    void colorChanged(const QColor &color);

private:
    int          m_width     = 2;
    Qt::PenStyle m_style     = Qt::SolidLine;
    QColor       m_color     = Qt::black;
    bool         m_antialias = true;
};

// ── 矩形 / 椭圆 ──────────────────────────────────────────────────────────
// 参数：线宽、绘制模式（描边/填充/两者）、描边色、填充色、圆角半径
class ShapeOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit ShapeOptions(ShapeKind kind, QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    ShapeKind kind() const { return m_kind; }

    int       width() const { return m_width; }
    ShapeMode mode() const { return m_mode; }
    QColor    strokeColor() const { return m_strokeColor; }
    QColor    fillColor() const { return m_fillColor; }
    bool      antialias() const { return m_antialias; }
    int       cornerRadius() const { return m_cornerRadius; }

    void setWidth(int v);
    void setMode(ShapeMode m);
    void setStrokeColor(const QColor &c);
    void setFillColor(const QColor &c);
    void setAntialias(bool v);
    void setCornerRadius(int v);

signals:
    void strokeColorChanged(const QColor &color);
    void fillColorChanged(const QColor &color);

private:
    ShapeKind m_kind;
    int       m_width        = 2;
    ShapeMode m_mode         = ShapeMode::Stroke;
    QColor    m_strokeColor  = Qt::black;
    QColor    m_fillColor    = Qt::white;
    bool      m_antialias    = true;
    int       m_cornerRadius = 0;
};

// ── 文字 ─────────────────────────────────────────────────────────────────
// 参数：字体、字号、颜色、粗体/斜体/下划线、对齐、行距、字间距
class TextOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit TextOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    QString family() const { return m_family; }
    int     pointSize() const { return m_pointSize; }
    QColor  color() const { return m_color; }
    bool    bold() const { return m_bold; }
    bool    italic() const { return m_italic; }
    bool    underline() const { return m_underline; }
    TextAlign align() const { return m_align; }
    qreal   lineHeight() const { return m_lineHeight; }
    qreal   letterSpacing() const { return m_letterSpacing; }

    void setFamily(const QString &f);
    void setPointSize(int v);
    void setColor(const QColor &c);
    void setBold(bool v);
    void setItalic(bool v);
    void setUnderline(bool v);
    void setAlign(TextAlign a);
    void setLineHeight(qreal v);
    void setLetterSpacing(qreal v);

    // 组装出当前配置对应的 QFont。
    // ★ 这个函数是唯一允许把 PointSize 换算成像素的地方 ★
    //   对话框里显示的是磅值，而渲染到图层上需要像素值，
    //   换算涉及 DPI，散落各处极易不一致。
    QFont toFont(qreal dpi) const;

    // 直接设置整个 QFont（界面上用户从字体对话框选完后用）
    void setFromFont(const QFont &font);

signals:
    void colorChanged(const QColor &color);

private:
    QString   m_family        = QStringLiteral("Microsoft YaHei");
    int       m_pointSize     = 24;
    QColor    m_color         = Qt::black;
    bool      m_bold          = false;
    bool      m_italic        = false;
    bool      m_underline     = false;
    TextAlign m_align         = TextAlign::Left;
    qreal     m_lineHeight    = 1.2;   // 行距倍数
    qreal     m_letterSpacing = 0.0;   // 字间距（像素）
};

// ── 选区（矩形 / 椭圆）───────────────────────────────────────────────────
// 参数：模式（新建/添加/减去/交集）、羽化半径、抗锯齿
class SelectionOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit SelectionOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    SelectionMode mode() const { return m_mode; }
    qreal         featherRadius() const { return m_featherRadius; }
    bool          antialias() const { return m_antialias; }

    void setMode(SelectionMode m);
    void setFeatherRadius(qreal v);
    void setAntialias(bool v);

    // 把本选项的枚举转成 core/Selection.h 的运算枚举。
    // 放在这里而不是让工具自己转，是为了让"选项条上的四个按钮"
    // 与"选区运算"的映射只有一处定义。
    // 注意返回类型用 int —— 避免本头文件 include core/Selection.h
    // 造成的不必要依赖（调用方自行 static_cast）。
    static int toSelectionOp(SelectionMode mode);

    // 由修饰键推断临时模式（Photoshop 的约定）：
    //   Shift → 添加，Alt → 减去，Shift+Alt → 交集
    // 工具的 mousePress 在没手动切模式时调用它。
    static SelectionMode modeFromModifiers(bool shift, bool alt, SelectionMode fallback);

private:
    SelectionMode m_mode          = SelectionMode::Replace;
    qreal         m_featherRadius = 0.0;
    bool          m_antialias     = true;
};

// ── 移动 ─────────────────────────────────────────────────────────────────
// 参数：是否自动选择图层、是否显示变换框
// 注意：这个工具的参数确实不修改任何像素算法，因此放在这里也无妨 ——
//       但为了统一，仍走同一个基类。
class MoveOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit MoveOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    bool autoSelectLayer() const { return m_autoSelectLayer; }
    bool showTransformBox() const { return m_showTransformBox; }

    void setAutoSelectLayer(bool v);
    void setShowTransformBox(bool v);

private:
    bool m_autoSelectLayer  = true;
    bool m_showTransformBox = false;
};

// ── 裁剪 ─────────────────────────────────────────────────────────────────
// 参数：预设比例、是否删除裁剪区域外的像素、网格叠加
class CropOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit CropOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    AspectPreset preset() const { return m_preset; }
    bool         deleteOutside() const { return m_deleteOutside; }
    CropGrid     grid() const { return m_grid; }

    void setPreset(AspectPreset p);
    void setDeleteOutside(bool v);
    void setGrid(CropGrid g);

    // 预设比例对应的宽高比。返回 0 表示"自由"（不约束）。
    static qreal presetRatio(AspectPreset p);

private:
    AspectPreset m_preset        = AspectPreset::Free;
    bool         m_deleteOutside = true;
    CropGrid     m_grid          = CropGrid::Thirds;
};

// ── 缩放 ─────────────────────────────────────────────────────────────────
// 参数：放大 / 缩小 切换
class ZoomOptions : public ToolOptions
{
    Q_OBJECT

public:
    explicit ZoomOptions(QObject *parent = nullptr);

    QWidget *createWidget(QWidget *parent) override;
    void load(QSettings &settings) override;
    void save(QSettings &settings) const override;

    bool zoomIn() const { return m_zoomIn; }
    void setZoomIn(bool v);

private:
    bool m_zoomIn = true;
};

} // namespace zl
