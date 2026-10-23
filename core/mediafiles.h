#pragma once

#include <QByteArray>
#include <QString>

namespace berry {

// File name for an upload: no folders, no control or reserved characters, at
// most 120 characters and an extension matching the detected image format
// ("jpeg", "png" or "webp"). Empty when the format is not supported.
QString sanitizeFileName(const QString &name, const QByteArray &format);

// Absolute folder of an album ("reisen/norwegen") below mediaRoot; mediaRoot
// itself for an empty album. Empty when the album would leave mediaRoot or
// touch hidden folders (".x", "@eaDir"), or when an existing folder on the
// way is a symbolic link leading out of mediaRoot.
QString albumDirectory(const QString &mediaRoot, const QString &album);

// directory/fileName, or "name (2).jpg", "name (3).jpg" … when taken
QString uniqueFilePath(const QString &directory, const QString &fileName);

// Moves an image and its sidecar to <trashRoot>/<yyyyMMdd-HHmmss>/<path below mediaRoot>.
// Nothing is deleted for good, so a wrong click can be undone by hand.
bool moveToTrash(const QString &mediaRoot, const QString &trashRoot, const QString &imagePath,
                 QString *error = nullptr);

// Moves a whole album folder with everything in it to the trash, like moveToTrash().
bool moveFolderToTrash(const QString &mediaRoot, const QString &trashRoot, const QString &folder,
                       QString *error = nullptr);

// Moves an image and its sidecar into another folder below the media root,
// keeping the modification time so the library recognises the move. Returns
// the new path, or an empty string on failure.
QString moveImage(const QString &imagePath, const QString &targetDirectory, QString *error = nullptr);

} // namespace berry
