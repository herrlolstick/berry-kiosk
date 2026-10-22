#pragma once

#include <QCborMap>
#include <QList>
#include <QString>

class QIODevice;

namespace berry {

// Local socket between the display (server) and the admin service (client).
// Each frame is a CBOR map, length-prefixed by QDataStream.
//   request   { id, command, ...arguments }
//   response  { id, ok, error?, ...result }
//   event     { event, ... }            sent by the display on its own
// Commands: status, next, pause, resume, screenshot { maxWidth }
// Events:   shown { region, path }

// A path in the state directory, or a short name in the temp directory where
// the path would exceed the socket path limit (104 bytes on macOS).
QString displaySocketName(const QString &stateDir);

void writeFrame(QIODevice *device, const QCborMap &message);
// All complete frames available; a partial frame stays in the device
QList<QCborMap> readFrames(QIODevice *device);

} // namespace berry
