#include "compat/GlobalHotkey.h"

#include <QCoreApplication>
#include <QDebug>

#ifdef Q_OS_WIN
//  ★ 包含 windows.h 之前的两条惯例 ★
//    WIN32_LEAN_AND_MEAN：砍掉 winsock1 之类的老古董，减少与 Qt 头文件的冲突面
//    NOMINMAX：不要定义 min/max 宏 —— 否则任何 `std::min(...)` /
//    `qMin(...)` 都会被宏替换成一段语法垃圾，报错位置还指向 Qt 头文件内部，
//    极难定位。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

// ★ 链接 user32 ★
//   RegisterHotKey / UnregisterHotKey / PostThreadMessage 都在 user32 里，
//   而 MSVC 的默认库不含它 —— Qt5Widgets.lib 是**导入库**，它的 user32
//   依赖在运行期由 Qt5Widgets.dll 自己满足，帮不到我们这些目标文件。
//
//   ★ 为什么用 #pragma comment，而不是在 .pro / CMakeLists.txt /
//     run_tests.sh 里各加一行 ★
//     · 那三处是**三份手工维护的清单**，而自检的链接行尤其容易漏
//       （本工程已经在这类"清单没同步"上栽过好几次：漏了只在链接期报
//       "无法解析的外部符号"，看起来像少编译了文件）。
//     · 写在这里，谁读这个文件就看到了"它依赖 Win32"—— 不可能漏。
//   MinGW 会忽略这条 pragma（并给一条警告）；本工程用 MSVC 构建。
#  ifdef _MSC_VER
#    pragma comment(lib, "user32.lib")
#  endif

// 老版本 SDK 没有这个标志（Win7 起才有）
#  ifndef MOD_NOREPEAT
#    define MOD_NOREPEAT 0x4000
#  endif
#endif

namespace zl {

#ifdef Q_OS_WIN

// ---------------------------------------------------------------------------
//  Qt::Key → Win32 虚拟键码（VK_*）
// ---------------------------------------------------------------------------
//  ★ 为什么是一张"有限支持"的表，而不是查遍全部按键 ★
//    全局热键的合理候选就那么几类：字母、数字、功能键、几个编辑键。
//    剩下的（多媒体键、输入法键、厂商自定义键）各家驱动不一样，
//    硬猜一个 VK 出来会让 RegisterHotKey 静默登记到一个**别的键**上 ——
//    用户按 A 没反应、按 B 反而截图了，这种错法比"不支持"糟糕得多。
//    所以不认识的一律返回 0，由调用方**响亮地拒绝**。
static quint32 virtualKeyFor(int key)
{
    // 字母与数字：Qt::Key_A..Z 就是 'A'..'Z'，数值上等于 VK 码；数字同理。
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return quint32(key);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return quint32(key);

    // 功能键：VK_F1 == 0x70，而 Qt::Key_F1 == 0x01000030 —— 一一对应（含 F13~F24）
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return 0x70u + quint32(key - Qt::Key_F1);

    switch (key) {
    case Qt::Key_Print:     return VK_SNAPSHOT;
    case Qt::Key_Insert:    return VK_INSERT;
    case Qt::Key_Delete:    return VK_DELETE;
    case Qt::Key_Home:      return VK_HOME;
    case Qt::Key_End:       return VK_END;
    case Qt::Key_PageUp:    return VK_PRIOR;
    case Qt::Key_PageDown:  return VK_NEXT;
    case Qt::Key_Left:      return VK_LEFT;
    case Qt::Key_Up:        return VK_UP;
    case Qt::Key_Right:     return VK_RIGHT;
    case Qt::Key_Down:      return VK_DOWN;
    case Qt::Key_Return:
    case Qt::Key_Enter:     return VK_RETURN;
    case Qt::Key_Escape:    return VK_ESCAPE;
    case Qt::Key_Space:     return VK_SPACE;
    case Qt::Key_Tab:       return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Pause:     return VK_PAUSE;
    default:                return 0;
    }
}

// Qt 的修饰键 → MOD_* 位标志
static UINT winModifiersFor(Qt::KeyboardModifiers mods)
{
    UINT m = 0;
    if (mods & Qt::ControlModifier)  m |= MOD_CONTROL;
    if (mods & Qt::AltModifier)      m |= MOD_ALT;
    if (mods & Qt::ShiftModifier)    m |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)     m |= MOD_WIN;
    return m;
}

#endif   // Q_OS_WIN

// ============================================================================
//  GlobalHotkey
// ============================================================================

GlobalHotkey::GlobalHotkey(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    // ★ 把本对象挂到 QCoreApplication 的原生事件过滤器上 ★
    //   这是"系统投来的消息"进入 Qt 世界的唯一入口。
    //   install 必须在 registerSequence 之前完成（这里在构造里做，
    //   而登记是后面的显式调用）—— 反过来的话，注册成功之后的第一次
    //   按键会落进真空：消息到了，但没人接。
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
#else
    m_lastError = tr("Global hotkeys are not supported on this platform (the current implementation is Windows-only).");
#endif
}

GlobalHotkey::~GlobalHotkey()
{
    unregisterAll();

#ifdef Q_OS_WIN
    // 松手要对称：过滤器挂在 QCoreApplication 上，而它比本对象活得久。
    // 不摘的话，本对象析构后 Qt 仍会把消息转给它 —— 典型的 use-after-free。
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
#endif
}

bool GlobalHotkey::registerSequence(const QKeySequence &sequence, int id)
{
    m_lastError.clear();

#ifdef Q_OS_WIN

    if (!QCoreApplication::instance()) {
        m_lastError = tr("No QApplication instance - cannot register the global hotkey.");
        return false;
    }
    if (sequence.isEmpty()) {
        m_lastError = tr("The key sequence is empty.");
        return false;
    }

    // ★ QKeySequence 的取项 ★
    //   Qt5 的 operator[] 返回 int（键码 | 修饰位），Qt6 换成了 QKeyCombination。
    //   本工程当前按 Qt5.15 编译；将来若要上 Qt6，只需在这一行分版本
    //   （QKeyCombination 有 key() / keyboardModifiers()）。
    const int combined = int(sequence[0]);
    const Qt::KeyboardModifiers mods =
        Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask);

    const QString seqText = sequence.toString(QKeySequence::NativeText);

    const quint32 vk = virtualKeyFor(combined & ~Qt::KeyboardModifierMask);
    if (vk == 0) {
        m_lastError = tr("The main key of %1 is out of the supported "
                         "range - try another (letters / digits / "
                         "F1~F24 / cursor keys, etc.).").arg(seqText);
        return false;
    }

    const UINT winMods = winModifiersFor(mods);
    if (winMods == 0) {
        // ★ 单键的全局热键一律不收 ★
        //   登记一个裸的 'S' 意味着**全系统**那个键都被本程序吃掉：
        //   用户在任何一个程序里打字都不会出 S。这不是"功能"，是事故。
        //   只有极少数专门做这个的软件才那样干，而它们要用户明确确认。
        m_lastError = tr("The global hotkey %1 needs at least one modifier (Ctrl / Alt / Shift).")
                          .arg(seqText);
        return false;
    }

    // 同一个 id 重复登记 = 覆盖上一次（不做"两个都生效"—— 那只会让
    // 上层多出一个"忘记注销"的机会）
    unregister(id);

    // ★ MOD_NOREPEAT 必须带上 ★
    //   不带的后果：按住快捷键不放会**连续触发**（系统自动重复），
    //   一秒钟截出几十张图。这一点在"按一下没反应就多按一会儿"的用户
    //   身上特别容易触发。
    if (!RegisterHotKey(nullptr, id, winMods | MOD_NOREPEAT, vk)) {
        const DWORD err = ::GetLastError();

        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            // 实测里最常见的失败原因：被 QQ / 微信 / 截图工具 / 输入法占了
            m_lastError = tr("The hotkey %1 is already taken by another "
                             "program (a combination can only be "
                             "registered by one program system-wide).")
                              .arg(seqText);
        } else {
            m_lastError = tr("Failed to register the global hotkey %1 (system error code %2).")
                              .arg(seqText).arg(uint(err));
        }
        return false;
    }

    m_regs.insert(id, sequence);
    return true;

#else   // ── 非 Windows：编译得过，但明确地不支持 ──

    Q_UNUSED(sequence)
    Q_UNUSED(id)
    m_lastError = tr("Global hotkeys are not supported on this platform (the current implementation is Windows-only).");
    return false;

#endif
}

void GlobalHotkey::unregister(int id)
{
    if (!m_regs.contains(id))
        return;

#ifdef Q_OS_WIN
    ::UnregisterHotKey(nullptr, id);
#endif

    m_regs.remove(id);
}

void GlobalHotkey::unregisterAll()
{
    const QList<int> ids = m_regs.keys();
    for (int id : ids)
        unregister(id);
}

bool GlobalHotkey::isRegistered(int id) const
{
    return m_regs.contains(id);
}

bool GlobalHotkey::simulateActivation(int id)
{
#ifdef Q_OS_WIN
    // ★ 刻意不检查"是否登记过" ★
    //   未登记的 id 投进去，正好用来验证"没登记的 id 不该触发任何东西"
    //   （自检里的负向用例）。在这里先挡掉的话，那条用例就变成了
    //   "函数返回 false" —— 什么都没测到。
    return ::PostThreadMessageW(::GetCurrentThreadId(), WM_HOTKEY,
                                WPARAM(id), 0) != FALSE;
#else
    Q_UNUSED(id)
    return false;
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message,
                                     NativeEventResult *result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)

#ifdef Q_OS_WIN
    //  ★ 两类事件名都要看一眼 ★
    //    "windows_generic_MSG"   —— Qt 事件分发器取到的普通消息
    //    "windows_dispatcher_MSG" —— Qt 自己的内部窗口那一趟
    //    WM_HOTKEY 是投给**线程**的（其 hwnd 为空），Qt 把它归到哪一类
    //    没有明确承诺，也不保证将来不变 —— 所以这里不挑事件名。
    MSG *msg = static_cast<MSG *>(message);
    if (msg && msg->message == WM_HOTKEY) {
        const int id = int(msg->wParam);

        // 只认自己登记过的 id："别的程序"的热键消息不可能投到本线程，
        // 但同一进程里将来可能有第二个使用者 —— 由 id 分清。
        if (m_regs.contains(id)) {
            if (result)
                *result = 0;
            emit activated(id);
            return true;          // 已消费，别再往下发
        }
    }
#endif

    return false;
}

} // namespace zl
