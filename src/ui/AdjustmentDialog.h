#pragma once

// ============================================================================
//  AdjustmentDialog.h —— 调整类对话框的公共基类
// ----------------------------------------------------------------------------
//  "调整"指亮度对比度、色相饱和度、曲线这一类"参数 → 整层像素映射"的操作。
//  它们有三个共同点，也正是本基类要统一掉的部分：
//
//    1. **可实时预览** —— 拖动滑块/曲线时画面立刻跟着变
//    2. **可取消** —— 取消后文档必须一点痕迹都不留
//    3. **只产出一条撤销记录** —— 不管用户拖了多少次，历史面板里只有一条
//
//  ★ 关键设计：对话框不认识 Document ★
//
//    子类只需实现一个纯函数 apply(src) —— 「把当前参数作用到这张图上」。
//    对话框完全不知道文档、撤销栈、画布的存在，因此：
//      · 预览与最终提交走**同一份代码**，二者结果必然一致
//      · 对话框可以在没有文档的环境下单独测试
//      · 拿它去做批处理（对一批图套同一个参数）是免费的
//
//    主窗口负责另外三件事（见 MainWindow::runAdjustment）：
//      · 把 apply() 作用在**降采样副本**上做快速预览
//      · 确定时把 apply() 作用在**全尺寸原图**上并压入 ImageCommand
//      · 取消时什么都不做（预览从来没碰过文档，天然无需还原）
// ============================================================================

#include <QDialog>
#include <QImage>
#include <QString>

class QCheckBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QSlider;

namespace zl {

class AdjustmentDialog : public QDialog
{
    Q_OBJECT

public:
    // source —— 打开对话框那一刻的图层位图（全尺寸）。
    //           背景直方图的统计对象；也用于判断"参数是否回到初值"。
    AdjustmentDialog(const QImage &source, const QString &title, QWidget *parent = nullptr);
    ~AdjustmentDialog() override;

    // ── 子类必须实现 ──

    // ★ 核心接口 ★ 把当前参数作用到 src 上，返回新图像。
    //   必须是纯函数：不改任何成员、不碰全局状态、可重复调用。
    //   MainWindow 会用它算"降采样预览"与"全尺寸最终结果"。
    virtual QImage apply(const QImage &src) const = 0;

    // 参数是否偏离初始值。
    //
    // ★ 为什么需要它 ★
    //   用户打开对话框看了一眼、什么都没动就点确定 —— 这时不应该在
    //   历史面板里留下一条记录。否则历史列表会被这类空记录灌满，
    //   真正的操作反而找不到。（一条"亮度/对比度"记录背后可能就是
    //   96MB 的压缩快照，代价不只是难看。）
    virtual bool isChanged() const = 0;

    // 把参数恢复为初始值。"复位"按钮调用它。
    virtual void resetParams() = 0;

    // ── 供 MainWindow 查询 ──

    const QImage &source() const { return m_source; }

    bool isPreviewEnabled() const;

signals:
    // 参数变化，请求重新生成预览。
    // 对话框本身不处理它 —— 由 MainWindow 接到槽里。
    void previewNeeded();

protected:
    // 子类在构造函数末尾调用：把参数区交给基类，由基类补上
    // "预览勾选框 + 确定/取消/复位"这一行，并完成整体布局。
    void buildLayout(QWidget *paramArea);

    // 建一行「标签 + 滑块 + 数字框」，返回滑块。
    //
    // 三个简单调整对话框的全部控件都由它生成 —— 保证外观、步长、
    // 数值框宽度完全一致，而不是每个对话框各调各的（那样迟早会出现
    // "这个对话框的滑块按一下动 1、那个动 5"的不一致）。
    QSlider *addSliderRow(QGridLayout *grid, int row,
                          const QString &label, int lo, int hi, int value,
                          const QString &suffix = QString());

    // 参数变化时由子类调用：发 previewNeeded()。
    // 构造期间（参数初始化）自动静默，不会白白算一遍预览。
    void notifyChanged();

private slots:
    void onResetClicked();
    void onPreviewToggled(bool on);

private:
    QImage       m_source;
    QCheckBox   *m_previewCheck = nullptr;
    QPushButton *m_resetButton  = nullptr;

    // 构造期间为 true。子类在构造函数里给控件设初值时也会触发
    // valueChanged，此时既不该重算预览（参数还没建完），
    // 也不该把"复位"按钮点亮。buildLayout() 会把它置回 false。
    bool m_loadingParams = true;
};

} // namespace zl
