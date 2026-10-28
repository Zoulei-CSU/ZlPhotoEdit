#include "core/UiLanguage.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

#include <memory>

namespace zl {
namespace uilang {

// codeFromLocaleName 的定义在文件后半 —— 刻意放在匿名 namespace 之外，
// 因为它是对外接口（测试要调）。deriveFromLocale 在它之前，先声明一下；
// 这句必须写在匿名 namespace 外，否则声明成内部链接、与外面的定义打架。
QString codeFromLocaleName(const QString &name);

namespace {

// ---------------------------------------------------------------------------
//  源码文本的语言
// ---------------------------------------------------------------------------
//  ★ 2026-10-13 起是 "en"（阶段 2 英文化第一批：菜单系统）★
//
//    从 "zh_CN" 翻到 "en" 的那一刻，整个决策链自动翻转：
//      · "en"     从"翻译目标"变成"源码本身"（不再需要 .qm）；
//      · "zh_CN"  从"源码"变成"唯一需要 .qm 的语言"。
//    这正是把语言决策集中到一个文件的原因 —— 英文化是一场持续多批的
//    大改，但"程序认为自己是什么语言"只有这一行。
//
//    ★ 半英文化状态是安全的 ★
//    源码里尚未英文化的字符串（中文）在两种模式下都显示中文：
//    中文模式有 .qm 翻译的走翻译、没有的回落中文源码；英文模式全部
//    回落源码。所以翻转这个值不会制造"一半英文一半中文"的窗口，
//    只会让英文模式随着一批批英文化逐渐变全。
constexpr const char *kSourceLanguage = "en";

// QSettings 键名。发布后不要改（改了等于重置用户的选择）。
//   "ui/" 分组与 tools/ 平级，装的是"应用级偏好"而非某个工具的参数。
constexpr const char *kSettingsKey = "ui/language";

// "自动（跟随系统）"的哨兵值：存进 QSettings 表示"按 locale 推断"，
// 也是语言菜单的第一项与默认状态。它不是语言，永远不进可用列表。
constexpr const char *kAutoLanguage = "auto";

// ---------------------------------------------------------------------------
//  构建期内嵌的翻译语言
// ---------------------------------------------------------------------------
//  ★ 与构建侧的清单必须一一对应（三处）★
//      · ZlPhotoEdit.pro   的 I18N_LANG_CODES
//      · CMakeLists.txt    的 ZLPE_TRANSLATIONS
//      · 这里
//    漏一个的后果不是编译错误，而是"菜单里少一种语言 / 多一种死语言"——
//    双构建 0 错 0 警，只有真的去点菜单才会发现。所以这三处要靠人对账，
//    而对账的时机就是"新增一种语言"那一刻（极低频，可接受）。
//
//  ★ 为什么列在这里而不是运行时扫描 ":/i18n/" ★
//    运行时扫 qrc 目录听着更"自动"，但它把"构建有没有成功内嵌"
//    变成了运行时的隐藏状态：测试程序（不编 i18n.qrc）与真机行为
//    会不一致，而语言菜单的有无正是一件该被测出来的事。
//    声明式列表让两者行为一致：菜单结构在测试里可断言，
//    真正的 .qm 缺失由装载失败 + qWarning 兜底（见 installTranslator）。
const QStringList kEmbeddedLanguages = {
    QStringLiteral("zh_CN"),
    QStringLiteral("zh_TW"),
    QStringLiteral("fr"),
    QStringLiteral("es"),
    QStringLiteral("ja"),
    QStringLiteral("ru"),
};

QString deriveFromLocale()
{
    // ★ 先走 uiLanguages()，再回落 language() ★
    //    macOS 上 QLocale::system() 读的是 CFLocaleCopyCurrent()，在系统的
    //    "首选语言"列表里有多条时，它经常给出 en_US 而不是排第一的语言 ——
    //    表现为系统明明是简体中文、程序却判成英文。uiLanguages() 按用户
    //    偏好顺序返回整个列表，取第一个认得的才是"跟随系统"的本意。
    //    其它平台（Win/Linux）uiLanguages() 也正确实现，两条路一致。
    const QStringList preferred = QLocale::system().uiLanguages();
    for (const QString &name : preferred) {
        const QString code = codeFromLocaleName(name);
        if (!code.isEmpty())
            return code;
    }

    // uiLanguages() 为空（极旧平台 / 被裁掉的构建）时回落到 language()。
    // 中文按字形分流：繁体（台/港/澳）→ zh_TW，其余（简体）→ zh_CN。
    const QLocale sys = QLocale::system();
    if (sys.language() == QLocale::Chinese) {
        if (sys.script() == QLocale::TraditionalChineseScript)
            return QLatin1String("zh_TW");
        return QLatin1String("zh_CN");
    }
    if (sys.language() == QLocale::French)
        return QLatin1String("fr");
    if (sys.language() == QLocale::Spanish)
        return QLatin1String("es");
    if (sys.language() == QLocale::Japanese)
        return QLatin1String("ja");
    if (sys.language() == QLocale::Russian)
        return QLatin1String("ru");
    // 其余语言（德语/意大利语/...）没有翻译 —— 英文兜底，
    // 与 currentLanguage() 第 ③ 步同一个来源（kSourceLanguage）。
    return QLatin1String("en");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  locale 名 → 语言代码
// ---------------------------------------------------------------------------
//  ★ 定义放在匿名 namespace **外面** ★
//    它在 UiLanguage.h 里有声明，必须是对外可见的符号 —— 放进匿名 namespace
//    就成了内部链接，测试程序那边链接时看不到定义（LNK2019）。
//    而 deriveFromLocale 只是内部用，留在匿名 namespace 里没问题。

QString codeFromLocaleName(const QString &name)
{
    const QString lower = name.toLower();
    // 形式可能是 "zh-hans-cn" / "zh_cn" / "zh" —— 取第一个分隔符前的一段
    QString lang = lower.section(QLatin1Char('-'), 0, 0);
    lang = lang.section(QLatin1Char('_'), 0, 0);

    if (lang == QLatin1String("zh")) {
        // 带 hant 的才是繁体；未标注字形的（macOS 的 "zh"）按简体，
        // 与系统"中文（简体）"为默认的实际情况一致。
        if (lower.contains(QLatin1String("hant")))
            return QLatin1String("zh_TW");
        return QLatin1String("zh_CN");
    }
    if (lang == QLatin1String("fr"))
        return QLatin1String("fr");
    if (lang == QLatin1String("es"))
        return QLatin1String("es");
    if (lang == QLatin1String("ja"))
        return QLatin1String("ja");
    if (lang == QLatin1String("ru"))
        return QLatin1String("ru");
    if (lang == QLatin1String("en"))
        return QLatin1String("en");
    return QString();
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------

QString sourceLanguage()
{
    return QLatin1String(kSourceLanguage);
}

QString autoLanguage()
{
    return QLatin1String(kAutoLanguage);
}

QStringList availableLanguages()
{
    // 源码语言永远可用（它不需要翻译文件），且排在第一位 ——
    // 菜单里"当前语言"之外的默认项就是它。
    QStringList result;
    const QString src = sourceLanguage();
    result << src;

    for (const QString &code : kEmbeddedLanguages) {
        if (code == src)
            continue;                       // 与源码语言重复，跳过
        if (!result.contains(code))
            result << code;
    }
    return result;
}

QString languagePreference()
{
    // 没存过 = 自动。显式存 "auto" 与"没存过"等价 —— 菜单的勾选态
    // 因此不必区分这两者，用户选"自动"后设置里也会留下痕迹。
    //
    // 存了别的不认识的值（例如用户手改了设置文件）也原样返回，
    // 由 currentLanguage() 落到"自动"分支 —— 垃圾偏好按自动处理，
    // 好过把垃圾当成一个语言去装载。
    QSettings settings;
    const QString saved =
        settings.value(QLatin1String(kSettingsKey)).toString();
    if (saved.isEmpty())
        return QLatin1String(kAutoLanguage);
    return saved;
}

QString currentLanguage()
{
    const QStringList avail = availableLanguages();

    // ① 显式选过的语言优先（"auto" 不参与可用表判断，走 ②）
    const QString pref = languagePreference();
    if (pref != QLatin1String(kAutoLanguage) && avail.contains(pref))
        return pref;

    // ② 自动 → 按 locale 推断；存的已不可用（例如装过测试版又卸了
    //    翻译）也同样落在这里，行为一致。
    const QString guessed = deriveFromLocale();
    if (avail.contains(guessed))
        return guessed;

    // ③ 兜底：源码语言永远可用，不存在"没有界面语言"这种状态。
    return sourceLanguage();
}

// ---------------------------------------------------------------------------
//  写入
// ---------------------------------------------------------------------------

void setCurrentLanguage(const QString &code)
{
    // 不校验就写入，会让"下次启动拿着一个不存在的语言"——
    // 那时界面静默回落到源码文本，用户以为切换失败了（而且没有任何
    // 地方能查）。挡在这里，比在每个读取点兜底便宜。
    //
    // "auto" 是合法取值（跟随系统），与语言代码一并放行。
    if (code != QLatin1String(kAutoLanguage)
        && !availableLanguages().contains(code))
        return;

    QSettings settings;
    settings.setValue(QLatin1String(kSettingsKey), code);
    // 紧接着调用方会弹"重启生效"的提示框；落盘要赶在那之前完成，
    // 否则用户看完提示直接 Alt+F4，这次选择就丢了。
    settings.sync();
}

// ---------------------------------------------------------------------------
//  装载
// ---------------------------------------------------------------------------

bool needsTranslation(const QString &code)
{
    return code != sourceLanguage();
}

bool installTranslator(QCoreApplication *app, const QString &code)
{
    if (!app)
        return false;

    // 进程内只保留一个翻译器。重复安装会让两份翻译叠在一起 ——
    // Qt 按安装顺序反序查找先装的后生效，行为取决于谁后装，极难查。
    // 这里用函数内 static：installTranslator 是启动期（或切语言时）
    // 的单线程入口，不需要更复杂的生命周期管理。
    static std::unique_ptr<QTranslator> translator;
    if (translator) {
        app->removeTranslator(translator.get());
        translator.reset();
    }

    if (!needsTranslation(code))
        return false;        // 源码语言：控件文本本身就是最终文本

    // 不设 parent：Qt 的 QObject 父子机制会在 app 析构时再 delete 一次，
    // 与上面的 removeTranslator + reset 冲突。生命周期只归这个 static 管，
    // 而 static 的析构发生在 main() 的局部对象（app）之后，顺序安全。
    translator.reset(new QTranslator);
    const QString path =
        QStringLiteral(":/i18n/zlphotoedit_%1.qm").arg(code);
    if (!translator->load(path)) {
        // ★ 失败时把"资源里到底有什么"打出来 ★
        //   只报"加载失败 :/i18n/xxx.qm"时，排查者无法区分：
        //     ① .qm 根本没内嵌（构建期 lrelease 缺失 → i18n.qrc 没生成）
        //     ② 内嵌了但路径对不上（qrc prefix 写错）
        //     ③ 内嵌了但文件损坏
        //   列出 :/i18n 目录的实际内容，一眼就能分辨 ①/②。
        //   2026-10-19 的"切中文没反应"就是 ①，而当时这条日志只说
        //   "加载失败"，看不出是构建没带翻译。
        const QStringList available =
            QDir(QStringLiteral(":/i18n")).entryList(QDir::Files, QDir::Name);
        qWarning().noquote()
            << "[ZlPhotoEdit] 翻译文件加载失败:" << path
            << "（界面将显示源码文本）"
            << "| :/i18n 下实际内嵌:" << (available.isEmpty()
                    ? QStringLiteral("【空 —— 这个构建没有内嵌任何翻译】")
                    : available.join(QLatin1Char(' ')));
        translator.reset();
        return false;
    }

    app->installTranslator(translator.get());
    return true;
}

QString displayName(const QString &code)
{
    if (code == QLatin1String("zh_CN"))
        return QString::fromUtf8("简体中文");
    if (code == QLatin1String("zh_TW"))
        return QString::fromUtf8("繁體中文");
    if (code == QLatin1String("fr"))
        return QString::fromUtf8("Français");
    if (code == QLatin1String("es"))
        return QString::fromUtf8("Español");
    if (code == QLatin1String("ja"))
        return QString::fromUtf8("日本語");
    if (code == QLatin1String("ru"))
        return QString::fromUtf8("Русский");
    if (code == QLatin1String("en"))
        return QStringLiteral("English");
    // 未知语言：把代码本身摆出来，胜过给一个空菜单项
    return code;
}

} // namespace uilang
} // namespace zl
