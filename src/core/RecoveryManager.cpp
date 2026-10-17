#include "core/RecoveryManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace zl {

namespace {

const char *kImageSuffix = ".png";
const char *kMetaSuffix  = ".json";

} // namespace

RecoveryManager::RecoveryManager(QObject *parent)
    : QObject(parent)
    , m_directory(defaultDirectory())
{
}

QString RecoveryManager::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/recovery");
}

QString RecoveryManager::directory() const
{
    return m_directory;
}

void RecoveryManager::setDirectory(const QString &dir)
{
    m_directory = dir;
}

QString RecoveryManager::makeKey(int sequence)
{
    return QStringLiteral("rec-%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(sequence);
}

QString RecoveryManager::metaPath(const QString &key) const
{
    return m_directory + QLatin1Char('/') + key + QLatin1String(kMetaSuffix);
}

QString RecoveryManager::imagePath(const QString &key) const
{
    return m_directory + QLatin1Char('/') + key + QLatin1String(kImageSuffix);
}

bool RecoveryManager::ensureDirectory()
{
    if (m_directory.isEmpty()) {
        m_lastError = QStringLiteral("recovery directory is empty");
        return false;
    }
    if (QDir().mkpath(m_directory))
        return true;

    m_lastError = QStringLiteral("cannot create recovery directory: %1").arg(m_directory);
    return false;
}

bool RecoveryManager::save(const QString &key, const QImage &image,
                           const QString &originalPath, const QString &title)
{
    m_lastError.clear();

    if (key.isEmpty() || image.isNull()) {
        m_lastError = QStringLiteral("empty key or image");
        return false;
    }
    if (!ensureDirectory())
        return false;

    // 图先写。用 QSaveFile：写到临时文件再原子替换，
    // 崩溃时不会留下半张图被人当成有效快照读出来。
    {
        QSaveFile file(imagePath(key));
        if (!file.open(QIODevice::WriteOnly)) {
            m_lastError = file.errorString();
            return false;
        }

        QImageWriter writer(&file, "PNG");
        if (!writer.write(image)) {
            m_lastError = writer.errorString();
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            m_lastError = file.errorString();
            return false;
        }
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("originalPath"), originalPath);
    meta.insert(QStringLiteral("title"), title);
    meta.insert(QStringLiteral("savedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    meta.insert(QStringLiteral("width"), image.width());
    meta.insert(QStringLiteral("height"), image.height());

    QSaveFile file(metaPath(key));
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        m_lastError = file.errorString();
        return false;
    }

    return true;
}

void RecoveryManager::discard(const QString &key)
{
    if (key.isEmpty())
        return;

    QFile::remove(metaPath(key));
    QFile::remove(imagePath(key));
}

QList<RecoveryManager::Entry> RecoveryManager::pendingEntries() const
{
    QList<Entry> entries;

    if (m_directory.isEmpty())
        return entries;

    const QDir dir(m_directory);
    const QFileInfoList files = dir.entryInfoList(
        QStringList() << QStringLiteral("*") + QLatin1String(kMetaSuffix),
        QDir::Files);

    for (const QFileInfo &info : files) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;

        const QJsonObject meta = QJsonDocument::fromJson(file.readAll()).object();

        Entry entry;
        entry.key = info.completeBaseName();
        entry.imagePath = imagePath(entry.key);

        // 元数据有、图没有 —— 上次写到一半就被杀了，丢弃。
        if (!QFileInfo::exists(entry.imagePath))
            continue;

        entry.originalPath = meta.value(QStringLiteral("originalPath")).toString();
        entry.title = meta.value(QStringLiteral("title")).toString();
        entry.savedAt = QDateTime::fromString(
            meta.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
        entry.imageSize = QSize(meta.value(QStringLiteral("width")).toInt(),
                                meta.value(QStringLiteral("height")).toInt());

        entries.append(entry);
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.savedAt < b.savedAt; });

    return entries;
}

void RecoveryManager::clearAll()
{
    const QList<Entry> entries = pendingEntries();
    for (const Entry &entry : entries)
        discard(entry.key);

    // 顺手删掉可能存在的孤儿文件（元数据丢了、图还在）。
    if (!m_directory.isEmpty()) {
        QDir dir(m_directory);
        const QFileInfoList images = dir.entryInfoList(
            QStringList() << QStringLiteral("*") + QLatin1String(kImageSuffix),
            QDir::Files);
        for (const QFileInfo &info : images) {
            const QString keyPath = m_directory + QLatin1Char('/')
                                    + info.completeBaseName() + QLatin1String(kMetaSuffix);
            if (!QFileInfo::exists(keyPath))
                QFile::remove(info.absoluteFilePath());
        }
    }
}

} // namespace zl
