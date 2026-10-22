#include "state.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace berry {

QString displayStatePath(const QString &stateDir)
{
    return QDir(stateDir).filePath(QStringLiteral("display.json"));
}

QString libraryStatePath(const QString &stateDir)
{
    return QDir(stateDir).filePath(QStringLiteral("library.json"));
}

bool writeJsonFile(const QString &path, const QJsonObject &object, QString *error)
{
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("%1: Ordner nicht anlegbar").arg(dir);
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(object).toJson());
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

QJsonObject readJsonFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

FileSnapshot::FileSnapshot(const QString &path)
{
    setPath(path);
}

void FileSnapshot::setPath(const QString &path)
{
    m_path = path;
    m_exists = false;
    m_content.clear();
    changed();
}

bool FileSnapshot::changed()
{
    QFile file(m_path);
    const bool exists = !m_path.isEmpty() && file.open(QIODevice::ReadOnly);
    const QByteArray content = exists ? file.readAll() : QByteArray();
    if (exists == m_exists && content == m_content)
        return false;
    m_exists = exists;
    m_content = content;
    return true;
}

} // namespace berry
