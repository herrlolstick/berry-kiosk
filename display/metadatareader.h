#pragma once

#include <QObject>
#include <QVariantMap>
#include <QtQmlIntegration>

// Reads the metadata of one image in the background, for the widgets.
class MetadataReader : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString locale READ locale WRITE setLocale NOTIFY localeChanged)
    Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY metadataChanged)

public:
    explicit MetadataReader(QObject *parent = nullptr);

    QString path() const { return m_path; }
    void setPath(const QString &path);
    QString locale() const { return m_locale; }
    void setLocale(const QString &locale);
    QVariantMap metadata() const { return m_metadata; }

signals:
    void pathChanged();
    void localeChanged();
    void metadataChanged();
    // After every read, also when the result equals the previous one
    void loaded();

private:
    void reload();
    void apply(quint64 generation, const QVariantMap &metadata);

    QString m_path;
    QString m_locale = QStringLiteral("de_DE");
    QVariantMap m_metadata;
    quint64 m_generation = 0;
};
