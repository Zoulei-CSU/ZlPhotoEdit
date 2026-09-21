#include "core/DocumentManager.h"
#include "engine/ImageMeta.h"

#include <QFileInfo>
#include <QDir>   // QDir::cleanPath（路径归一化用）

namespace zl {

DocumentManager::DocumentManager(QObject *parent)
    : QObject(parent)
{
}

DocumentManager::~DocumentManager()
{
    // 负责删除所有文档。用 qDeleteAll 而不是逐个 delete，
    // 但要注意先清空列表再删 —— 避免文档的析构函数里如果触发了
    // 什么回调（目前没有，但将来可能有）导致遍历中出现悬空指针。
    const QList<Document *> docs = m_docs;
    m_docs.clear();
    m_currentIndex = -1;

    for (Document *doc : docs)
        delete doc;
}

// ===========================================================================
//  文档的创建与销毁
// ===========================================================================

Document *DocumentManager::createDocument(const QSize &size, const QColor &fillColor)
{
    // 新建文档的尺寸上限检查。
    // 一张 4000×3000 的 ARGB32 图是 48MB，4 个图层就接近 200MB。
    // 尺寸特别大时给出提示而不是硬做 —— 用户可能只是多打了一个 0。
    const qint64 pixels = qint64(size.width()) * qint64(size.height());
    constexpr qint64 kMaxPixels = 200'000'000LL;   // 2 亿像素

    if (pixels > kMaxPixels) {
        m_lastError = tr("图像尺寸过大（%1 × %2，约 %3 亿像素），"
                         "超出内存承受范围。请减小尺寸后重试。")
                          .arg(size.width())
                          .arg(size.height())
                          .arg(pixels / 100'000'000.0, 0, 'f', 1);
        return nullptr;
    }

    auto *doc = new Document(size, fillColor, this);
    addDocument(doc);
    m_lastError.clear();
    return doc;
}

Document *DocumentManager::openDocument(const QString &filePath)
{
    if (filePath.isEmpty()) {
        m_lastError = tr("文件路径为空。");
        return nullptr;
    }

    // 已经打开过同一个文件 → 切换到已有文档，而不是新开一个。
    // 这样用户重复点击"打开"同一个文件时不会得到两个一样的标签。
    const int existing = indexOfPath(filePath);
    if (existing >= 0) {
        setCurrentIndex(existing);
        m_lastError.clear();
        return m_docs.at(existing);
    }

    QFileInfo info(filePath);
    if (!info.exists()) {
        m_lastError = tr("文件不存在：\n%1").arg(filePath);
        return nullptr;
    }
    if (!info.isReadable()) {
        m_lastError = tr("没有读取权限：\n%1").arg(filePath);
        return nullptr;
    }

    // ── 大文件预检 ──
    //
    // 先只读文件头拿尺寸，判断是否值得尝试解码。
    // 这比"直接解码失败"友好得多：一个 2 亿像素的 TIFF 解码时会先
    // 申请几 GB 内存，可能让整个系统卡死，用户连错误提示都看不到。
    const ImageMetadata meta = readMetadata(filePath);
    if (meta.size.isValid() && !meta.size.isEmpty()) {
        const qint64 pixels = qint64(meta.size.width()) * qint64(meta.size.height());
        constexpr qint64 kMaxPixels = 200'000'000LL;

        if (pixels > kMaxPixels) {
            m_lastError = tr("图像过大，无法打开：\n%1\n\n"
                             "尺寸 %2 × %3（约 %4 亿像素），"
                             "超出内存承受范围。")
                              .arg(info.fileName())
                              .arg(meta.size.width())
                              .arg(meta.size.height())
                              .arg(pixels / 100'000'000.0, 0, 'f', 1);
            return nullptr;
        }
    }

    // ── 加载图像 ──
    // loadImageWithMetadata 内部启用了 EXIF 自动方向，
    // 手机拍的竖图会被自动摆正。
    ImageMetadata actualMeta;
    QImage image = loadImageWithMetadata(filePath, &actualMeta);

    if (image.isNull()) {
        // 解码失败。给出可读的错误描述而不是只说"打开失败" ——
        // 用户需要知道是"格式不支持"还是"文件损坏"，才能决定下一步。
        m_lastError = tr("无法打开图像：\n%1\n\n"
                         "可能的原因：文件已损坏、格式不受支持，"
                         "或文件不是有效的图像。")
                          .arg(info.fileName());
        return nullptr;
    }

    // ── 建立文档 ──
    auto *doc = new Document(image.size(), Qt::transparent, this);

    // 用加载的图像替换初始图层。
    // 这里直接操作图层而不是走 addLayer —— 因为初始图层已经存在，
    // 再 add 一个会得到两个图层（一个空的 + 一个内容层）。
    if (Layer *layer = doc->layers().layer(0)) {
        layer->setImage(image);
        layer->setName(info.completeBaseName().isEmpty()
                           ? tr("背景")
                           : info.completeBaseName());
    }

    doc->setFilePath(filePath);

    // 同步 DPI 与元数据
    doc->setDpi(actualMeta.dpiX, actualMeta.dpiY);

    // 关键：markDirty 会让合成缓存失效，下次绘制时用新内容重新合成。
    doc->markDirty();

    // 刚打开的文件不算"已修改"
    doc->setModified(false);

    addDocument(doc);
    m_lastError.clear();
    return doc;
}

int DocumentManager::addDocument(Document *doc)
{
    if (!doc)
        return -1;

    connectDocument(doc);

    m_docs.append(doc);
    const int index = m_docs.size() - 1;

    emit documentAdded(doc);

    // 新加入的文档自动成为当前文档（这是最符合直觉的行为：
    // 用户新建或打开一个文件，焦点当然应该跳到它上面）
    setCurrentIndex(index);

    return index;
}

bool DocumentManager::closeDocument(int index)
{
    if (index < 0 || index >= m_docs.size())
        return false;

    Document *doc = m_docs.takeAt(index);

    emit documentRemoved(doc);

    // ── 修正当前索引 ──
    // 关掉的是当前文档时，把焦点移到相邻的一个；
    // 关掉的是前面某个文档时，当前索引要减一。
    if (m_currentIndex == index) {
        // 优先选前一个（视觉上"焦点往上移"更自然）
        m_currentIndex = (index > 0) ? index - 1
                                     : (m_docs.isEmpty() ? -1 : 0);
    } else if (m_currentIndex > index) {
        --m_currentIndex;
    }

    m_currentIndex = qBound(-1, m_currentIndex, m_docs.size() - 1);

    delete doc;

    // 通知面板重新绑定
    emit currentChanged(currentDocument());

    return true;
}

void DocumentManager::closeAll()
{
    const QList<Document *> docs = m_docs;
    m_docs.clear();
    m_currentIndex = -1;

    for (Document *doc : docs) {
        emit documentRemoved(doc);
        delete doc;
    }

    emit currentChanged(nullptr);
}

// ===========================================================================
//  当前文档
// ===========================================================================

Document *DocumentManager::currentDocument() const
{
    if (m_currentIndex < 0 || m_currentIndex >= m_docs.size())
        return nullptr;
    return m_docs.at(m_currentIndex);
}

void DocumentManager::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_docs.size())
        return;
    if (m_currentIndex == index)
        return;

    m_currentIndex = index;

    // 这个信号是面板重新绑定的唯一触发点。
    // 所有面板（图层面板、历史、导航器、状态栏）都必须在收到它时
    // 调用自己的 setDocument()，而不是在构造时绑定文档指针。
    emit currentChanged(currentDocument());
}

// ===========================================================================
//  查询
// ===========================================================================

Document *DocumentManager::documentAt(int index) const
{
    if (index < 0 || index >= m_docs.size())
        return nullptr;
    return m_docs.at(index);
}

bool DocumentManager::isOwnerOf(const Document *doc) const
{
    if (!doc)
        return false;
    // 用地址比较。虽然指针可能被复用（文档销毁后新文档占用同一地址），
    // 但实际使用场景不会触发这个边界：剪贴板数据的生命周期通常短于文档。
    return m_docs.contains(const_cast<Document *>(doc));
}

int DocumentManager::indexOf(const Document *doc) const
{
    if (!doc)
        return -1;

    for (int i = 0; i < m_docs.size(); ++i) {
        if (m_docs.at(i) == doc)
            return i;
    }
    return -1;
}

int DocumentManager::indexOfPath(const QString &filePath) const
{
    if (filePath.isEmpty())
        return -1;

    // 统一路径分隔符与大小写后再比较。
    // Windows 下路径不区分大小写，且 / 与 \ 混用很常见
    // （Qt 返回的路径用 /，用户从资源管理器复制的用 \）。
    const QString normalized = QFileInfo(filePath).canonicalFilePath().isEmpty()
                                   ? QDir::cleanPath(filePath)
                                   : QFileInfo(filePath).canonicalFilePath();

    for (int i = 0; i < m_docs.size(); ++i) {
        const QString existing = m_docs.at(i)->filePath();
        if (existing.isEmpty())
            continue;

        const QString existingNormalized =
            QFileInfo(existing).canonicalFilePath().isEmpty()
                ? QDir::cleanPath(existing)
                : QFileInfo(existing).canonicalFilePath();

        if (QString::compare(existingNormalized, normalized,
                             Qt::CaseInsensitive) == 0) {
            return i;
        }
    }

    return -1;
}

// ===========================================================================
//  修改状态
// ===========================================================================

bool DocumentManager::hasUnsavedChanges() const
{
    for (const Document *doc : m_docs) {
        if (doc && doc->isModified())
            return true;
    }
    return false;
}

QStringList DocumentManager::unsavedDocumentNames() const
{
    QStringList names;
    int untitledCounter = 1;

    for (const Document *doc : m_docs) {
        if (!doc || !doc->isModified())
            continue;

        if (doc->filePath().isEmpty()) {
            names << tr("未命名 %1").arg(untitledCounter++);
        } else {
            names << QFileInfo(doc->filePath()).fileName();
        }
    }

    return names;
}

// ===========================================================================
//  多标签预留接口
// ===========================================================================

QString DocumentManager::titleAt(int index) const
{
    const Document *doc = documentAt(index);
    if (!doc)
        return QString();
    return doc->displayName();
}

QStringList DocumentManager::allTitles() const
{
    QStringList titles;
    titles.reserve(m_docs.size());
    for (const Document *doc : m_docs) {
        titles << (doc ? doc->displayName() : QString());
    }
    return titles;
}

// ===========================================================================
//  内部
// ===========================================================================

void DocumentManager::connectDocument(Document *doc)
{
    if (!doc)
        return;

    // 把文档的"修改状态变化"转发成管理器级别的信号。
    // 这样 MainWindow 只需要监听 DocumentManager 就能更新窗口标题，
    // 不必为每个文档单独连信号 —— 多标签化时这一点尤其重要。
    connect(doc, &Document::modifiedChanged, this,
            [this, doc](bool modified) {
                emit documentModifiedChanged(doc, modified);
            });
}

} // namespace zl
