#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace berry {

struct ImageMetadata
{
    QString title;
    QString description;
    QString location;
    QString camera;
    QString lens;
    QDateTime taken;
    double aperture = 0;     // f-number
    double exposureTime = 0; // seconds
    double focalLength = 0;  // millimetres
    int iso = 0;
    // Only from the sidecar file
    QString quote;
    QString quoteAuthor;

    QVariantMap toVariantMap(const QLocale &locale) const;
};

// Must be called once before metadata is read from several threads.
void initializeMetadataReading();

// Reads EXIF, IPTC and XMP, then applies the sidecar file next to the image,
// whose values win. GPS positions are deliberately never read: the location of
// a nest or a den must not end up on a public screen.
ImageMetadata readImageMetadata(const QString &path);

// "elch.jpg" -> "elch.json"
QString sidecarPath(const QString &imagePath);
// Known keys: title, description, location, quote, quoteAuthor. An empty string
// clears a value taken from the image.
void applySidecar(ImageMetadata &metadata, const QJsonObject &sidecar);

QString formatCamera(const QString &make, const QString &model);
QString formatAperture(double fNumber, const QLocale &locale);
QString formatExposureTime(double seconds, const QLocale &locale);
QString formatFocalLength(double millimetres, const QLocale &locale);
// "f/8 · 1/250 s · ISO 400 · 400 mm", leaving out what is unknown
QString formatExposureSettings(const ImageMetadata &metadata, const QLocale &locale);
// Non-empty, distinct parts joined with ", "
QString joinLocation(const QStringList &parts);

} // namespace berry
