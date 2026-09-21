#include "tools/ToolManager.h"

#include "tools/AbstractTool.h"
#include "tools/ToolOptions.h"

#include <QSettings>
#include <QCoreApplication>
#include <QDebug>

namespace zl {

// ============================================================================
//  QSettings 的组织方式
// ============================================================================
//
//  键的层级：
//    ZlPhotoEdit/tool/brush/size
//    ZlPhotoEdit/tool/brush/color
//    ZlPhotoEdit/tool/bucket/tolerance
//    ZlPhotoEdit/tools/lastActive        ← 上次使用的工具 id
//
//  ★ 公司名与应用名必须在这里设置，否则 QSettings 会写到
//    "Unknown Organization" 这种不可预期的位置 ★
//    QApplication 构造时通常已经设过，但工具层不该依赖"别人设过了"这个假设。
static void ensureApplicationIdentity()
{
    if (QCoreApplication::organizationName().isEmpty())
        QCoreApplication::setOrganizationName(QStringLiteral("ZlPhotoEdit"));
    if (QCoreApplication::applicationName().isEmpty())
        QCoreApplication::setApplicationName(QStringLiteral("ZlPhotoEdit"));
}

// ============================================================================
//  构造 / 析构
// ============================================================================

ToolManager::ToolManager(CommandStack *stack, QObject *parent)
    : QObject(parent)
    , m_stack(stack)
{
    ensureApplicationIdentity();
}

ToolManager::~ToolManager()
{
    // 工具以 this 为 parent（在 addTool 里设置），所以 Qt 会自动删除它们。
    // 这里只清空容器，不做 delete —— 否则会双重释放。
    clear();
}

// ============================================================================
//  注册
// ============================================================================

void ToolManager::addTool(AbstractTool *tool)
{
    if (!tool)
        return;

    const QString toolId = tool->id();

    // ── id 唯一性检查 ──
    //  重复的 id 会导致 findById 返回错误的工具，
    //  且 QSettings 的分组会互相覆盖。宁可在开发期就暴露出来。
    if (!toolId.isEmpty() && m_byId.contains(toolId)) {
        qWarning() << "ToolManager: 工具 id 重复，已忽略。" << toolId;
        delete tool;
        return;
    }

    // ★ 所有权 ★
    //  如果工具已经有 parent（例如由 MainWindow 创建并传了 parent），
    //  这里就不再改 parent —— 强行改会破坏调用方的预期。
    //  否则把 parent 设为 this，让 ToolManager 接管生命周期。
    if (!tool->parent())
        tool->setParent(this);

    // 创建时就带上当前的文档与撤销栈，省得调用方还要记得调一次
    tool->setDocument(m_document);
    tool->setCommandStack(m_stack);

    connectToolSignals(tool);

    m_tools.append(tool);
    if (!toolId.isEmpty())
        m_byId.insert(toolId, tool);

    // ── 第一个注册的工具自动成为活动工具 ──
    //  没有这行的话，程序启动后会处于"没有任何工具可用"的状态，
    //  用户点画布毫无反应，会以为是程序坏了。
    if (!m_activeTool)
        setActiveTool(tool);
}

void ToolManager::connectToolSignals(AbstractTool *tool)
{
    // 把工具的状态消息转发到本类的同名信号上。
    // ToolManager 不直接连接状态栏 —— 那是 ui 层的职责。
    connect(tool, &AbstractTool::statusMessage,
            this, &ToolManager::statusMessage);
}

void ToolManager::clear()
{
    // m_activeTool 指向的也是 m_tools 里的对象，不需要单独处理
    m_activeTool = nullptr;
    m_byId.clear();

    // 工具是 this 的子对象，Qt 会在 parent 析构时统一删除。
    // 但如果 clear() 是在运行期被调用的（而非析构），
    // 需要主动删除，否则会产生"已从列表移除但仍活在内存里"的悬挂对象。
    const QList<AbstractTool *> copy = m_tools;
    m_tools.clear();

    for (AbstractTool *tool : copy) {
        if (tool && tool->parent() == this)
            delete tool;
        // parent 不是 this 的（由外部管理）不删 —— 那是调用方的责任
    }
}

// ============================================================================
//  查询
// ============================================================================

AbstractTool *ToolManager::findById(const QString &id) const
{
    return m_byId.value(id, nullptr);
}

AbstractTool *ToolManager::findByShortcut(int key) const
{
    if (key == 0)
        return nullptr;

    for (AbstractTool *tool : m_tools) {
        if (tool->shortcutKey() == key)
            return tool;
    }
    return nullptr;
}

// ============================================================================
//  活动工具
// ============================================================================

void ToolManager::setActiveTool(AbstractTool *tool)
{
    if (m_activeTool == tool)
        return;

    // ★ 旧工具必须收尾 ★
    //  顺序：先 commit（把正在进行的操作提交掉），再换指针。
    //  如果先换指针，commit 会作用到新工具上 —— 新工具没有正在进行的操作，
    //  于是旧工具的半成品就被静默丢弃了（用户画的笔会凭空消失）。
    if (m_activeTool) {
        m_activeTool->commit();
    }

    m_activeTool = tool;

    // 新工具拿到当前的文档与撤销栈
    if (m_activeTool) {
        m_activeTool->setDocument(m_document);
        m_activeTool->setCommandStack(m_stack);
    }

    emit activeToolChanged(m_activeTool);
}

bool ToolManager::setActiveTool(const QString &id)
{
    AbstractTool *tool = findById(id);
    if (!tool)
        return false;

    setActiveTool(tool);
    return true;
}

bool ToolManager::setActiveToolByShortcut(int key)
{
    AbstractTool *tool = findByShortcut(key);
    if (!tool)
        return false;

    setActiveTool(tool);
    return true;
}

// ============================================================================
//  文档与撤销栈
// ============================================================================

void ToolManager::setDocument(Document *doc)
{
    if (m_document == doc)
        return;

    m_document = doc;

    // ★ 统一推送给所有工具 ★
    //
    //  这一步是 ToolManager 存在的最重要理由。
    //  新增一个工具时，只要它被 addTool 注册了，就自动支持文档切换 ——
    //  不可能出现"某个工具忘了实现 setDocument"这种遗漏。
    for (AbstractTool *tool : m_tools) {
        if (tool)
            tool->setDocument(doc);
    }
}

void ToolManager::setCommandStack(CommandStack *stack)
{
    m_stack = stack;
    for (AbstractTool *tool : m_tools) {
        if (tool)
            tool->setCommandStack(stack);
    }
}

// ============================================================================
//  持久化
// ============================================================================

void ToolManager::loadAllOptions()
{
    ensureApplicationIdentity();

    QSettings settings;   // 用默认构造：读组织名/应用名

    for (AbstractTool *tool : m_tools) {
        if (!tool)
            continue;

        ToolOptions *opts = tool->options();
        if (!opts)
            continue;

        opts->load(settings);
    }

    // ── 恢复上次使用的工具 ──
    //  这一项让用户"上次用橡皮、关掉程序、再打开"时直接回到橡皮，
    //  省掉一次切换。是很小但很影响日常体验的细节。
    const QString lastId = settings.value(QStringLiteral("tools/lastActive")).toString();
    if (!lastId.isEmpty()) {
        // setActiveTool(QString) 在找不到时返回 false，此时保持
        // 第一个注册的工具为活动工具（addTool 里的默认行为）。
        Q_UNUSED(setActiveTool(lastId))
    }
}

void ToolManager::saveAllOptions() const
{
    ensureApplicationIdentity();

    QSettings settings;

    for (AbstractTool *tool : m_tools) {
        if (!tool)
            continue;

        ToolOptions *opts = tool->options();
        if (!opts)
            continue;

        opts->save(settings);
    }

    if (m_activeTool)
        settings.setValue(QStringLiteral("tools/lastActive"), m_activeTool->id());

    // ★ 必须同步 ★
    //  在程序退出路径上，QSettings 的析构会写盘，
    //  但如果退出流程里还有别的 QSettings 实例（或程序被强杀），
    //  不 sync 就可能有数据丢失。显式 sync 一次，代价是几十微秒。
    settings.sync();
}

// ============================================================================
//  操作收尾
// ============================================================================

void ToolManager::commitActiveTool()
{
    if (m_activeTool)
        m_activeTool->commit();
}

void ToolManager::cancelActiveTool()
{
    if (m_activeTool)
        m_activeTool->cancel();
}

} // namespace zl
