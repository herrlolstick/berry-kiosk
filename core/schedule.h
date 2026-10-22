#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>

namespace berry {

// One entry of config.json "schedule":
//   { "days": "mo-fr", "from": "07:00", "to": "22:30", "layout": "tag" }
//   { "days": "daily", "from": "22:30", "to": "07:00", "screen": "off" }
// days: "daily"/"täglich", German or English abbreviations with ranges and
// lists ("mo-fr", "sa,so", "fri-mon"), or an array of such names or of
// numbers 1 (Monday) … 7. A "to" before "from" runs past midnight into the
// next day; from == to means the whole day.
struct ScheduleRule
{
    QList<int> days; // 1 = Monday … 7 = Sunday
    QTime from;
    QTime to;
    QString layout;      // empty = keep the active layout
    bool screenOff = false;

    bool matches(const QDateTime &when) const;
};

// What the schedule wants right now. Later rules win where they overlap.
struct ScheduleState
{
    QString layout;
    bool screenOff = false;
    bool operator==(const ScheduleState &other) const { return layout == other.layout && screenOff == other.screenOff; }
    bool operator!=(const ScheduleState &other) const { return !(*this == other); }
};

// Invalid entries are skipped; *errors receives a German description per entry
QList<ScheduleRule> parseSchedule(const QJsonArray &entries, QStringList *errors = nullptr);
ScheduleState evaluateSchedule(const QList<ScheduleRule> &rules, const QDateTime &when);

} // namespace berry
