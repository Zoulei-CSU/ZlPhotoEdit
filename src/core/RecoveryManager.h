#pragma once

// RecoveryManager.h —— 未保存内容的自动快照与恢复。
//
// 只存扁平合成图，不存图层结构：多图层文档恢复后是单图层。
// 之所以不做可续编的工程格式，见架构规划 §10（.zlpe 已定案不做）；
// 这里的文件只写在用户数据目录、不出现在"另存为"里，也没有跨版本兼容承诺。

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

namespace zl {

class RecoveryManager : public QObject
{
public:
    struct Entry {
        QString   key;
        QString   imagePath;
        QString   originalPath;
        QString   title;
        QDateTime savedAt;
        QSize     imageSize;
    };

    explicit RecoveryManager(QObject *parent = nullptr);

    // 默认目录：<AppData>/recovery
    static QString defaultDirectory();

    QString directory() const;
    void    setDirectory(const QString &dir);

    // 写入一条快照，同名覆盖。图先落盘、元数据后写，
    // 这样不会出现"元数据说有、图却不在"的条目。
    bool save(const QString &key, const QImage &image,
              const QString &originalPath, const QString &title);

    // 删掉一条（正常保存、关闭文档、恢复完成后都走这里）。
    void discard(const QString &key);

    // 枚举现存快照，按时间从旧到新。
    QList<Entry> pendingEntries() const;

    // 全部清空（正常退出时用）。
    void clearAll();

    // 本次运行的唯一前缀。恢复文件跨进程可见，用 pid 区分"哪一次运行"。
    static QString makeKey(int sequence);

    QString lastError() const { return m_lastError; }

private:
    QString metaPath(const QString &key) const;
    QString imagePath(const QString &key) const;
    bool    ensureDirectory();

    QString m_directory;
    QString m_lastError;
};

} // namespace zl
