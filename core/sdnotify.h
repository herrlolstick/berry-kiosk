#pragma once

#include <QByteArray>

namespace berry {

// Tells systemd about the service state ("READY=1", "WATCHDOG=1",
// "STATUS=…") through $NOTIFY_SOCKET. Does nothing outside systemd and on
// other systems than Linux, so callers need no checks.
void sdNotify(const QByteArray &state);

// Whether systemd expects watchdog pings ($WATCHDOG_USEC), and how often to
// send them: half the timeout, as systemd recommends. 0 = no watchdog.
int watchdogIntervalMs();

} // namespace berry
