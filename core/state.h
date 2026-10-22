#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace berry {

// Small JSON files in the state directory through which display and admin
// tell each other what changed:
//   display.json  written by the display: render tier, screen, device
//   library.json  written by the admin: generation counter after library changes
QString displayStatePath(const QString &stateDir);
QString libraryStatePath(const QString &stateDir);

// Atomic: readers never see a half-written file
bool writeJsonFile(const QString &path, const QJsonObject &object, QString *error = nullptr);
QJsonObject readJsonFile(const QString &path);

// Tells real changes of a small file from the noise of directory watches
// (QFileSystemWatcher reports any entry of a directory) by comparing contents.
class FileSnapshot
{
public:
    explicit FileSnapshot(const QString &path = {});

    QString path() const { return m_path; }
    // Starts over with the current content of path as the known state
    void setPath(const QString &path);
    // True when the file appeared, vanished or got a different content since the last call
    bool changed();

private:
    QString m_path;
    bool m_exists = false;
    QByteArray m_content;
};

} // namespace berry
