#pragma once

// ============================================================================
//  StatusBarWidget.h —— 状态栏内容
// ----------------------------------------------------------------------------
//  单独封成一个类而不是在 MainWindow 里堆一堆 QLabel，理由：
//
//    · 状态栏要显示 6 个字段，每个都有自己的更新时机与刷新频率
//    · 光标移动时坐标与取样色要每帧更新（高频），而内存占用只在
//      操作完成后更新（低频）—— 分开管理比混在一起清晰
//    · 多标签化时整个状态栏可以一起复用（它已经支持 setDocument）
//
//  显示内容（从左到右）：
//    坐标 (x, y) │ RGB 取样色块 + 数值 │ 尺寸 │ 内存 │ ... │ 消息 │ 缩放比例
//
//  ★ 遵守约定 7：提供 setDocument() ★
// ============================================================================

#include <QWidget>
#include <QPointF>
#include <QString>

class QLabel;
class QComboBox;
class QTimer;

namespace zl {

class Document;
class CommandStack;

class StatusBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StatusBarWidget(QWidget *parent = nullptr);
    ~StatusBarWidget() override;

    // ── 文档绑定 ──

    // 切换文档。会断开旧文档的信号连接。
    void setDocument(Document *doc);

    // 撤销栈只用于显示"撤销占用内存"，不建立信号连接
    // （撤销栈变化太频繁，为此实时刷新状态栏不值得）。
    void setCommandStack(CommandStack *stack);

public slots:
    // ── 光标信息 ──
    void setCursorPos(const QPointF &imagePos);
    void setSampledColor(const QColor &color, const QPointF &imagePos);
    void clearCursorInfo();

    // ── 缩放 ──
    // 由 CanvasView 的 viewChanged 驱动。会同步更新下拉框的显示。
    void setZoom(qreal zoom);

    // ── 消息 ──
    // timeoutMs 为 0 表示常驻（直到被下一条替换）。
    void showMessage(const QString &message, int timeoutMs = 2000);

    // ── 刷新文档信息 ──
    void refreshDocumentInfo();

signals:
    // 用户在缩放框里输入/选择了一个新比例（单位：百分比，例如 150.0）
    void zoomLevelRequested(qreal level);

private slots:
    void onZoomComboChanged();
    void onMessageTimeout();

private:
    void buildUi();

    Document     *m_doc   = nullptr;
    CommandStack *m_stack = nullptr;

    QLabel *m_coordLabel   = nullptr;   // "123, 456"
    QLabel *m_colorSwatch  = nullptr;   // 取样色块
    QLabel *m_colorLabel   = nullptr;   // "255, 128, 0"
    QLabel *m_sizeLabel    = nullptr;   // "尺寸: 4000 × 3000"
    QLabel *m_memoryLabel  = nullptr;   // "内存: 45.8 MB (撤销 120.3 MB)"
    QLabel *m_messageLabel = nullptr;   // 临时消息

    QComboBox *m_zoomCombo = nullptr;

    // ★ 程序化更新下拉框时的重入保护 ★
    //   不屏蔽的话会形成：
    //     视图缩放 → setZoom → 改下拉框 → 发 zoomLevelRequested
    //     → MainWindow 设置视图缩放 → ... 的循环。
    //   虽然数值相同时最终会收敛，但中间的抖动会让下拉框闪烁。
    bool m_updatingZoomCombo = false;

    QTimer *m_messageTimer = nullptr;
};

} // namespace zl
