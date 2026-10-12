#include "core/UiLanguage.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

#include <memory>

namespace zl {
namespace uilang {

namespace {

// ---------------------------------------------------------------------------
//  源码文本的语言
// ---------------------------------------------------------------------------
//  ★ 阶段 2 英文化时把这一行从 "zh_CN" 改成 "en" ★
//
//    它一改，整个决策链自动翻转，不需要动别处：
//      · "en"     从"翻译目标"变成"源码本身"（不再需要 .qm）；
//      · "zh_CN"  从"源码"变成"唯一需要 .qm 的语言"。
//    这正是把语言决策集中到一个文件的原因 —— 英文化是阶段 2 的
//    一场大改（724 处 tr()），但"程序认为自己是什么语言"只有这一行。
constexpr const char *kSourceLanguage = "zh_CN";

// QSettings 键名。发布后不要改（改了等于重置用户的选择）。
//   "ui/" 分组与 tools/ 平级，装的是"应用级偏好"而非某个工具的参数。
constexpr const char *kSettingsKey = "ui/language";

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
};

QString deriveFromLocale()
{
    // 简体中文环境 → zh_CN；其余一律先按英文猜，再由可用列表钳制。
    // （zh_TW / zh_HK 目前没有翻译文件，落回英文是合理默认；
    //   真出了繁体翻译，在这里加一支即可。）
    if (QLocale::system().language() == QLocale::Chinese)
        return QString::fromLatin1("zh_CN");
    return QString::fromLatin1("en");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------

QString sourceLanguage()
{
    return QString::fromLatin1(kSourceLanguage);
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

QString currentLanguage()
{
    const QStringList avail = availableLanguages();

    QSettings settings;
    const QString saved =
        settings.value(QString::fromLatin1(kSettingsKey)).toString();
    if (!saved.isEmpty() && avail.contains(saved))
        return saved;

    // 没选过（或存的已不可用，例如装过测试版又卸了翻译）→ 按 locale 推断。
    const QString guessed = deriveFromLocale();
    if (avail.contains(guessed))
        return guessed;

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
    if (!availableLanguages().contains(code))
        return;

    QSettings settings;
    settings.setValue(QString::fromLatin1(kSettingsKey), code);
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
        qWarning().noquote()
            << "[ZlPhotoEdit] 翻译文件加载失败:" << path
            << "（界面将显示源码文本）";
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
    if (code == QLatin1String("en"))
        return QStringLiteral("English");
    // 未知语言：把代码本身摆出来，胜过给一个空菜单项
    return code;
}

} // namespace uilang
} // namespace zl
