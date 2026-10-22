#include "metadatareader.h"

#include "imagemetadata.h"

#include <QCoreApplication>
#include <QLocale>
#include <QPointer>
#include <QThreadPool>

MetadataReader::MetadataReader(QObject *parent)
    : QObject(parent)
{
}

void MetadataReader::setPath(const QString &path)
{
    if (m_path == path)
        return;
    m_path = path;
    emit pathChanged();
    reload();
}

void MetadataReader::setLocale(const QString &locale)
{
    if (m_locale == locale)
        return;
    m_locale = locale;
    emit localeChanged();
    reload();
}

void MetadataReader::reload()
{
    const quint64 generation = ++m_generation;
    if (m_path.isEmpty()) {
        apply(generation, {});
        return;
    }

    // Files may sit on a slow network share, so keep this off the GUI thread.
    // Results of outdated requests are dropped via the generation counter.
    const QPointer<MetadataReader> guard(this);
    const QString path = m_path;
    const QLocale locale(m_locale);
    QThreadPool::globalInstance()->start([guard, generation, path, locale] {
        const QVariantMap metadata = berry::readImageMetadata(path).toVariantMap(locale);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, generation, metadata] {
            if (guard)
                guard->apply(generation, metadata);
        });
    });
}

void MetadataReader::apply(quint64 generation, const QVariantMap &metadata)
{
    if (generation != m_generation)
        return;
    if (metadata != m_metadata) {
        m_metadata = metadata;
        emit metadataChanged();
    }
    emit loaded();
}
