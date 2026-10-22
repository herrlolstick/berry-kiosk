#include "mediafiles.h"

#include "imagemetadata.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace berry {

namespace {

constexpr qsizetype kMaxFileNameLength = 120;
constexpr qsizetype kMaxAlbumSegmentLength = 100;

bool isReserved(QChar c)
{
    return c.unicode() < 0x20 || c == u'/' || c == u'\\' || c == u':' || c == u'*' || c == u'?' || c == u'"'
        || c == u'<' || c == u'>' || c == u'|' || c.unicode() == 0x7f;
}

// Same rule as the media scanner: these are skipped, so nothing may land there
bool isHiddenName(QStringView name)
{
    return name.startsWith(u'.') || name.startsWith(u'@');
}

bool moveFile(const QString &from, const QString &to)
{
    // Across file systems (NAS → SD card) rename fails; copy instead
    return QFile::rename(from, to) || (QFile::copy(from, to) && QFile::remove(from));
}

} // namespace

QString sanitizeFileName(const QString &name, const QByteArray &format)
{
    QStringList extensions;
    if (format == "jpeg" || format == "jpg")
        extensions = {QStringLiteral("jpg"), QStringLiteral("jpeg")};
    else if (format == "png")
        extensions = {QStringLiteral("png")};
    else if (format == "webp")
        extensions = {QStringLiteral("webp")};
    else
        return {};

    // Browsers on Windows may send "C:\fakepath\bild.jpg"
    QString fileName = name;
    fileName.replace(u'\\', u'/');
    fileName = fileName.section(u'/', -1);

    QString clean;
    for (const QChar c : std::as_const(fileName)) {
        if (!isReserved(c))
            clean.append(c);
    }
    clean = clean.trimmed();
    while (clean.startsWith(u'.') || clean.startsWith(u'@'))
        clean.remove(0, 1);

    const qsizetype dot = clean.lastIndexOf(u'.');
    QString base = dot > 0 ? clean.left(dot).trimmed() : clean;
    QString extension = dot > 0 ? clean.mid(dot + 1).toLower() : QString();
    if (!extensions.contains(extension)) {
        // Wrong or missing extension: keep the whole name, add the right one
        base = clean;
        extension = extensions.constFirst();
    }
    if (base.isEmpty())
        base = QStringLiteral("bild");
    base.truncate(kMaxFileNameLength - extension.size() - 1);
    return base.trimmed() + u'.' + extension;
}

QString albumDirectory(const QString &mediaRoot, const QString &album)
{
    const QString root = QDir::cleanPath(mediaRoot);
    QString path = album;
    path.replace(u'\\', u'/');

    QStringList segments;
    for (const QString &raw : path.split(u'/', Qt::SkipEmptyParts)) {
        const QString segment = raw.trimmed();
        if (segment.isEmpty())
            continue;
        if (segment == u"." || segment == u".." || isHiddenName(segment) || segment.size() > kMaxAlbumSegmentLength)
            return {};
        for (const QChar c : segment) {
            if (isReserved(c))
                return {};
        }
        segments.append(segment);
    }

    const QString directory = segments.isEmpty() ? root : QDir::cleanPath(root + u'/' + segments.join(u'/'));
    if (directory != root && !directory.startsWith(root + u'/'))
        return {};
    return directory;
}

QString uniqueFilePath(const QString &directory, const QString &fileName)
{
    const QDir dir(directory);
    if (!dir.exists(fileName))
        return dir.filePath(fileName);

    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : u'.' + info.suffix();
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix);
        if (!dir.exists(candidate))
            return dir.filePath(candidate);
    }
}

bool moveToTrash(const QString &mediaRoot, const QString &trashRoot, const QString &imagePath, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QString relative = QDir(mediaRoot).relativeFilePath(imagePath);
    if (relative.startsWith(QStringLiteral("..")) || QFileInfo(relative).isAbsolute())
        return fail(QStringLiteral("Bild liegt nicht im Bilderordner"));

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QDir target(QDir(trashRoot).filePath(stamp));
    const QString targetImage = target.filePath(relative);
    if (!QDir().mkpath(QFileInfo(targetImage).absolutePath()))
        return fail(QStringLiteral("Papierkorb nicht anlegbar: %1").arg(target.path()));
    if (!moveFile(imagePath, uniqueFilePath(QFileInfo(targetImage).absolutePath(), QFileInfo(targetImage).fileName())))
        return fail(QStringLiteral("%1 nicht verschiebbar").arg(relative));

    const QString sidecar = sidecarPath(imagePath);
    if (QFileInfo::exists(sidecar)) {
        const QString targetSidecar = QDir(QFileInfo(targetImage).absolutePath()).filePath(QFileInfo(sidecar).fileName());
        moveFile(sidecar, uniqueFilePath(QFileInfo(targetSidecar).absolutePath(), QFileInfo(targetSidecar).fileName()));
    }
    return true;
}

} // namespace berry
