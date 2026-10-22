#include "displayprotocol.h"

#include <QCborValue>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QIODevice>

namespace berry {

namespace {

constexpr auto kStreamVersion = QDataStream::Qt_6_5;
constexpr qsizetype kMaxSocketPath = 100;

} // namespace

QString displaySocketName(const QString &stateDir)
{
    const QString path = QDir(stateDir).filePath(QStringLiteral("display.sock"));
    if (QFile::encodeName(path).size() < kMaxSocketPath)
        return path;
    const QByteArray hash = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    return QStringLiteral("berry-kiosk-") + QString::fromLatin1(hash);
}

void writeFrame(QIODevice *device, const QCborMap &message)
{
    QDataStream out(device);
    out.setVersion(kStreamVersion);
    out << QCborValue(message).toCbor();
}

QList<QCborMap> readFrames(QIODevice *device)
{
    QList<QCborMap> frames;
    QDataStream in(device);
    in.setVersion(kStreamVersion);
    for (;;) {
        in.startTransaction();
        QByteArray frame;
        in >> frame;
        if (!in.commitTransaction())
            break;
        frames.append(QCborValue::fromCbor(frame).toMap());
    }
    return frames;
}

} // namespace berry
