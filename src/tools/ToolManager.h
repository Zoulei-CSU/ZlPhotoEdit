#pragma once

// ============================================================================
//  ToolManager.h —— 工具集合的管理者
// ----------------------------------------------------------------------------
//  职责：
//    · 持有全部工具对象（生命周期管理）
//    · 按 id 或快捷键查找工具
//    · 维护"当前活动工具"，并在切换时通知 CanvasView
//    · 把 QSettings 的加载/保存转发给每个工具的 ToolOptions
//
//  ★ 为什么单独搞一个类，而不放在 MainWindow 里 ★
//
//    工具的生命周期比 MainWindow 的一些面板要复杂：
//      · 工具箱面板需要遍历全部工具来生成按钮
//      · 快捷键处理需要按 Key 查工具
//      · 状态栏需要读当前工具的名字
//      · 切换文档时要把新文档推给所有工具
//    如果这些逻辑都散在 MainWindow 里，MainWindow 会膨胀成几千行，
//    且"某个工具忘了 setDocument"这类 bug 很难发现 ——
//    集中在一处则一眼可见（changeDocument 里统一遍历）。
//
//  ★ 依赖方向 ★
//    本类在 tools/ 层，不知道 MainWindow 与 CanvasView 的存在。
//    "切换工具时通知画布"通过信号 Express，由 MainWindow 连接。
//    这样 tools/ 层仍然不需要 include 任何 ui/ 的东西。
// ============================================================================

#include <QObject>
#include <QString>
#include <QList>
#include <QHash>

namespace zl {

class Document;
class CommandStack;
class AbstractTool;

class ToolManager : public QObject
{
    Q_OBJECT

public:
    // stack 与 doc 都不持有所有权。
    // 两者都可以为 nullptr（程序启动早期、或文档全部关闭时）。
    explicit ToolManager(CommandStack *stack, QObject *parent = nullptr);
    ~ToolManager() override;

    // ── 注册工具 ──
    //
    // 所有权：ToolManager 接管（工具以 this 为 parent，或由析构函数统一删除）。
    // 注册顺序 = 工具箱面板中的显示顺序。
    void addTool(AbstractTool *tool);

    // 清空全部工具（析构时自动调用）
    void clear();

    // ── 查询 ──

    const QList<AbstractTool *> &tools() const { return m_tools; }

    AbstractTool *findById(const QString &id) const;

    // 按快捷键查找。返回 nullptr 表示该键没有绑定工具。
    // ★ 注意修饰键 ★：只接受"无修饰键"的按键（B、E、M…），
    //   因为带 Ctrl/Alt 的组合是菜单快捷键的领域。
    AbstractTool *findByShortcut(int key) const;

    // ── 活动工具 ──

    AbstractTool *activeTool() const { return m_activeTool; }

    // 切换活动工具。
    // 传 nullptr 表示"没有活动工具"（画布仍可平移缩放）。
    // 若 tool 与当前相同则不做任何事（避免重复的 commit/cancel）。
    void setActiveTool(AbstractTool *tool);

    // 按 id 切换，找到才切换。返回是否成功。
    bool setActiveTool(const QString &id);

    bool setActiveToolByShortcut(int key);

    // ── 文档与撤销栈 ──

    // ★ 统一把新文档推给所有工具 ★
    //   这是"约定 7"（所有面板/工具提供 setDocument）的落地点。
    //   集中在这里的好处：新增工具只要注册进来了，
    //   就自动获得文档切换的支持，不可能遗漏。
    void setDocument(Document *doc);
    Document *document() const { return m_document; }

    void setCommandStack(CommandStack *stack);

    // ── 持久化 ──

    // 加载所有工具的参数。
    // 必须在 setDocument 之前调用（有些工具在设置参数时会重建缓存，
    // 那时如果还没有文档，会做一次无用功）。
    void loadAllOptions();

    // 保存所有工具的参数 + 记住当前活动的工具 id
    void saveAllOptions() const;

    // ── 操作收尾 ──

    // 提交当前工具的未完成操作（例如窗口即将关闭时）
    void commitActiveTool();

    // 取消当前工具的未完成操作
    void cancelActiveTool();

signals:
    // 活动工具发生变化。ToolOptionsBar 与 ToolBoxPanel 监听它。
    void activeToolChanged(AbstractTool *tool);

    // 工具请求的状态栏消息（转发自具体工具）
    void statusMessage(const QString &message, int timeoutMs);

private:
    // 连接一个工具的信号到本类的转发信号上
    void connectToolSignals(AbstractTool *tool);

    QList<AbstractTool *> m_tools;
    QHash<QString, AbstractTool *> m_byId;

    AbstractTool *m_activeTool = nullptr;
    Document     *m_document   = nullptr;
    CommandStack *m_stack      = nullptr;
};

} // namespace zl
