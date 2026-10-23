#include "schedule.h"

#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>

namespace berry {

namespace {

int dayNumber(QString name)
{
    static const QHash<QString, int> names = {
        {QStringLiteral("mo"), 1},  {QStringLiteral("mon"), 1}, {QStringLiteral("montag"), 1}, {QStringLiteral("monday"), 1},
        {QStringLiteral("di"), 2},  {QStringLiteral("tue"), 2}, {QStringLiteral("dienstag"), 2}, {QStringLiteral("tuesday"), 2},
        {QStringLiteral("mi"), 3},  {QStringLiteral("wed"), 3}, {QStringLiteral("mittwoch"), 3}, {QStringLiteral("wednesday"), 3},
        {QStringLiteral("do"), 4},  {QStringLiteral("thu"), 4}, {QStringLiteral("donnerstag"), 4}, {QStringLiteral("thursday"), 4},
        {QStringLiteral("fr"), 5},  {QStringLiteral("fri"), 5}, {QStringLiteral("freitag"), 5}, {QStringLiteral("friday"), 5},
        {QStringLiteral("sa"), 6},  {QStringLiteral("sat"), 6}, {QStringLiteral("samstag"), 6}, {QStringLiteral("saturday"), 6},
        {QStringLiteral("so"), 7},  {QStringLiteral("sun"), 7}, {QStringLiteral("sonntag"), 7}, {QStringLiteral("sunday"), 7},
    };
    name = name.trimmed().toLower();
    bool isNumber = false;
    const int number = name.toInt(&isNumber);
    if (isNumber)
        return number >= 1 && number <= 7 ? number : 0;
    return names.value(name, 0);
}

// "mo-fr", "sa,so", "fr-mo" (wraps), "daily"
bool parseDays(const QString &text, QList<int> *days)
{
    const QString spec = text.trimmed().toLower();
    if (spec == u"daily" || spec == u"täglich" || spec == u"taeglich" || spec == u"*") {
        *days = {1, 2, 3, 4, 5, 6, 7};
        return true;
    }
    for (const QString &part : spec.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
        const QStringList range = part.split(u'-');
        if (range.size() == 1) {
            const int day = dayNumber(range.first());
            if (!day)
                return false;
            days->append(day);
        } else if (range.size() == 2) {
            const int first = dayNumber(range.at(0));
            const int last = dayNumber(range.at(1));
            if (!first || !last)
                return false;
            for (int day = first;; day = day % 7 + 1) {
                days->append(day);
                if (day == last)
                    break;
            }
        } else {
            return false;
        }
    }
    return !days->isEmpty();
}

} // namespace

bool ScheduleRule::matches(const QDateTime &when) const
{
    const int today = when.date().dayOfWeek();
    const QTime now = when.time();
    if (from == to)
        return days.contains(today);
    if (from < to)
        return days.contains(today) && now >= from && now < to;
    // Past midnight: the evening part belongs to a listed day, the morning
    // part to the day after one
    const int yesterday = today == 1 ? 7 : today - 1;
    return (days.contains(today) && now >= from) || (days.contains(yesterday) && now < to);
}

QList<ScheduleRule> parseSchedule(const QJsonArray &entries, QStringList *errors)
{
    QList<ScheduleRule> rules;
    int number = 0;
    for (const QJsonValue &value : entries) {
        ++number;
        const auto reject = [&](const QString &why) {
            if (errors)
                errors->append(QStringLiteral("Zeitplan-Eintrag %1: %2").arg(number).arg(why));
        };
        const QJsonObject entry = value.toObject();
        ScheduleRule rule;

        const QJsonValue days = entry.value(u"days");
        bool daysOk = true;
        if (days.isUndefined()) {
            rule.days = {1, 2, 3, 4, 5, 6, 7};
        } else if (days.isArray()) {
            for (const QJsonValue &day : days.toArray()) {
                const int n = day.isDouble() ? day.toInt() : dayNumber(day.toString());
                if (n < 1 || n > 7)
                    daysOk = false;
                else
                    rule.days.append(n);
            }
        } else {
            daysOk = parseDays(days.toString(), &rule.days);
        }
        if (!daysOk || rule.days.isEmpty()) {
            reject(QStringLiteral("unbekannte Tage"));
            continue;
        }

        rule.from = QTime::fromString(entry.value(u"from").toString(QStringLiteral("00:00")), QStringLiteral("H:mm"));
        rule.to = QTime::fromString(entry.value(u"to").toString(QStringLiteral("00:00")), QStringLiteral("H:mm"));
        if (!rule.from.isValid() || !rule.to.isValid()) {
            reject(QStringLiteral("Uhrzeit im Format HH:mm erwartet"));
            continue;
        }

        rule.layout = entry.value(u"layout").toString().trimmed();
        const QString screen = entry.value(u"screen").toString().trimmed().toLower();
        rule.screenOff = screen == u"off" || screen == u"aus";
        if (rule.layout.isEmpty() && !rule.screenOff) {
            reject(QStringLiteral("braucht \"layout\" oder \"screen\": \"off\""));
            continue;
        }
        rules.append(rule);
    }
    return rules;
}

ScheduleState evaluateSchedule(const QList<ScheduleRule> &rules, const QDateTime &when)
{
    ScheduleState state;
    for (const ScheduleRule &rule : rules) {
        if (!rule.matches(when))
            continue;
        state.screenOff = rule.screenOff;
        state.layout = rule.screenOff ? QString() : rule.layout;
    }
    return state;
}

} // namespace berry
