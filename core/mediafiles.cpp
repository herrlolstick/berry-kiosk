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

    // The name may be fine while an existing folder on the way is a symbolic
    // link to somewhere else: check where the deepest existing part really is
    QString existing = directory;
    while (existing != root && !QFileInfo::exists(existing))
        existing = QFileInfo(existing).path();
    if (existing != root) {
        const QString realRoot = QFileInfo(root).canonicalFilePath();
        const QString realExisting = QFileInfo(existing).canonicalFilePath();
        if (realRoot.isEmpty() || !realExisting.startsWith(realRoot + u'/'))
            return {};
    }
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

bool moveFolderToTrash(const QString &mediaRoot, const QString &trashRoot, const QString &folder, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QString root = QDir::cleanPath(mediaRoot);
    const QString source = QDir::cleanPath(folder);
    if (source == root || !source.startsWith(root + u'/'))
        return fail(QStringLiteral("Nur Alben im Bilderordner lassen sich löschen"));

    const QString relative = QDir(root).relativeFilePath(source);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString target = QDir(QDir(trashRoot).filePath(stamp)).filePath(relative);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return fail(QStringLiteral("Papierkorb nicht anlegbar"));
    const QString unique = uniqueFilePath(QFileInfo(target).absolutePath(), QFileInfo(target).fileName());
    // Folders cannot be copied by QFile; trash and media live on the same disk by default
    if (!QDir().rename(source, unique))
        return fail(QStringLiteral("%1 nicht verschiebbar (anderes Laufwerk als der Papierkorb?)").arg(relative));
    return true;
}

QString moveImage(const QString &imagePath, const QString &targetDirectory, QString *error)
{
    if (!QDir().mkpath(targetDirectory)) {
        if (error)
            *error = QStringLiteral("Album nicht anlegbar");
        return {};
    }
    const QFileInfo info(imagePath);
    if (QDir::cleanPath(info.absolutePath()) == QDir::cleanPath(targetDirectory))
        return imagePath; // already there

    const QString target = uniqueFilePath(targetDirectory, info.fileName());
    const QDateTime modified = info.lastModified();
    if (!moveFile(imagePath, target)) {
        if (error)
            *error = QStringLiteral("%1 nicht verschiebbar").arg(info.fileName());
        return {};
    }
    // A copy across file systems gets a new time; the library matches moves by it
    QFile moved(target);
    if (moved.open(QIODevice::ReadWrite))
        moved.setFileTime(modified, QFileDevice::FileModificationTime);

    const QString sidecar = sidecarPath(imagePath);
    if (QFileInfo::exists(sidecar))
        moveFile(sidecar, sidecarPath(target));
    return target;
}

} // namespace berry
