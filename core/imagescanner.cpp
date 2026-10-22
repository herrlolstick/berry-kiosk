#include "imagescanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <algorithm>

namespace berry {

namespace {

bool isIgnoredName(QStringView name)
{
    return name.startsWith(u'.') || name.startsWith(u'@');
}

} // namespace

bool isSupportedImage(const QString &fileName)
{
    static const QStringList extensions = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("webp"),
    };
    const QFileInfo info(fileName);
    return !isIgnoredName(info.fileName()) && extensions.contains(info.suffix().toLower());
}

QStringList scanImages(const QString &directory)
{
    const QDir root(directory);
    if (!root.exists())
        return {};

    QStringList files;
    QDirIterator it(root.absolutePath(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!isSupportedImage(path))
            continue;

        const QStringList parts = root.relativeFilePath(path).split(u'/');
        const bool inIgnoredFolder = std::any_of(parts.cbegin(), parts.cend() - 1, [](const QString &part) {
            return isIgnoredName(part);
        });
        if (!inIgnoredFolder)
            files.append(path);
    }

    std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return files;
}

} // namespace berry
