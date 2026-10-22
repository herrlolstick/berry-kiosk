#include "imagemetadata.h"

#include "state.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <exiv2/exiv2.hpp>

#include <cmath>
#include <initializer_list>

namespace berry {

namespace {

// EXIF strings are often padded with NULs; stop at the first one
QString toQString(const std::string &value)
{
    return QString::fromUtf8(value.c_str()).trimmed();
}

QString firstNonEmpty(std::initializer_list<QString> values)
{
    for (const QString &value : values) {
        if (!value.isEmpty())
            return value;
    }
    return {};
}

QString exifText(const Exiv2::ExifData &exif, const char *key)
{
    const auto it = exif.findKey(Exiv2::ExifKey(key));
    return it == exif.end() ? QString() : toQString(it->toString());
}

double exifNumber(const Exiv2::ExifData &exif, const char *key)
{
    const auto it = exif.findKey(Exiv2::ExifKey(key));
    if (it == exif.end() || it->count() == 0)
        return 0;
    const double value = it->toFloat();
    return std::isfinite(value) && value > 0 ? value : 0;
}

QString iptcText(const Exiv2::IptcData &iptc, const char *key)
{
    const auto it = iptc.findKey(Exiv2::IptcKey(key));
    return it == iptc.end() ? QString() : toQString(it->toString());
}

QString xmpText(const Exiv2::XmpData &xmp, const char *key)
{
    const auto it = xmp.findKey(Exiv2::XmpKey(key));
    if (it == xmp.end())
        return {};
    // Titles and descriptions are language alternatives; prefer the default language
    if (const auto *langAlt = dynamic_cast<const Exiv2::LangAltValue *>(&it->value())) {
        std::string text = langAlt->toString("x-default");
        if (text.empty() && !langAlt->value_.empty())
            text = langAlt->value_.begin()->second;
        return toQString(text);
    }
    return toQString(it->toString());
}

QDateTime parseExifDate(const QString &value)
{
    QDateTime date = QDateTime::fromString(value, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!date.isValid())
        date = QDateTime::fromString(value, Qt::ISODate); // XMP
    return date;
}

QString formatNumber(double value, const QLocale &locale)
{
    QLocale plain = locale;
    plain.setNumberOptions(QLocale::OmitGroupSeparator);
    const double rounded = std::round(value * 10) / 10;
    if (rounded == std::floor(rounded))
        return plain.toString(qint64(rounded));
    return plain.toString(rounded, 'f', 1);
}

void readEmbedded(ImageMetadata &m, const QString &path)
{
    const auto image = Exiv2::ImageFactory::open(QFile::encodeName(path).toStdString());
    image->readMetadata();
    const Exiv2::ExifData &exif = image->exifData();
    const Exiv2::IptcData &iptc = image->iptcData();
    const Exiv2::XmpData &xmp = image->xmpData();

    // EXIF ImageDescription is left out on purpose: cameras fill it with
    // things like "OLYMPUS DIGITAL CAMERA".
    m.title = firstNonEmpty({xmpText(xmp, "Xmp.dc.title"), iptcText(iptc, "Iptc.Application2.ObjectName")});
    m.description = firstNonEmpty({xmpText(xmp, "Xmp.dc.description"), iptcText(iptc, "Iptc.Application2.Caption")});
    m.location = joinLocation({
        firstNonEmpty({xmpText(xmp, "Xmp.iptc.Location"), iptcText(iptc, "Iptc.Application2.SubLocation")}),
        firstNonEmpty({xmpText(xmp, "Xmp.photoshop.City"), iptcText(iptc, "Iptc.Application2.City")}),
        firstNonEmpty({xmpText(xmp, "Xmp.photoshop.Country"), iptcText(iptc, "Iptc.Application2.CountryName")}),
    });

    m.camera = formatCamera(exifText(exif, "Exif.Image.Make"), exifText(exif, "Exif.Image.Model"));
    m.lens = firstNonEmpty({exifText(exif, "Exif.Photo.LensModel"), xmpText(xmp, "Xmp.aux.Lens")});
    m.aperture = exifNumber(exif, "Exif.Photo.FNumber");
    m.exposureTime = exifNumber(exif, "Exif.Photo.ExposureTime");
    m.focalLength = exifNumber(exif, "Exif.Photo.FocalLength");
    m.iso = int(exifNumber(exif, "Exif.Photo.ISOSpeedRatings"));
    m.taken = parseExifDate(firstNonEmpty({
        exifText(exif, "Exif.Photo.DateTimeOriginal"),
        xmpText(xmp, "Xmp.exif.DateTimeOriginal"),
        xmpText(xmp, "Xmp.photoshop.DateCreated"),
    }));
}

} // namespace

QVariantMap ImageMetadata::toVariantMap(const QLocale &locale) const
{
    return {
        {QStringLiteral("title"), title},
        {QStringLiteral("description"), description},
        {QStringLiteral("location"), location},
        {QStringLiteral("camera"), camera},
        {QStringLiteral("lens"), lens},
        {QStringLiteral("taken"), taken.isValid() ? QVariant(taken) : QVariant()},
        {QStringLiteral("exposure"), formatExposureSettings(*this, locale)},
        {QStringLiteral("quote"), quote},
        {QStringLiteral("quoteAuthor"), quoteAuthor},
    };
}

void initializeMetadataReading()
{
    Exiv2::XmpParser::initialize();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
}

ImageMetadata readImageMetadata(const QString &path)
{
    ImageMetadata metadata;
    try {
        readEmbedded(metadata, path);
    } catch (const std::exception &error) {
        // Unsupported or broken files simply have no metadata
        qDebug().noquote() << "Keine Metadaten:" << path << '-' << error.what();
    }

    QFile sidecar(sidecarPath(path));
    if (sidecar.exists() && sidecar.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(sidecar.readAll(), &parseError);
        if (doc.isObject())
            applySidecar(metadata, doc.object());
        else
            qWarning().noquote() << "Sidecar unlesbar:" << sidecar.fileName() << '-' << parseError.errorString();
    }
    return metadata;
}

QString sidecarPath(const QString &imagePath)
{
    const QFileInfo info(imagePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".json"));
}

void applySidecar(ImageMetadata &metadata, const QJsonObject &sidecar)
{
    const auto take = [&sidecar](QStringView key, QString &field) {
        const QJsonValue value = sidecar.value(key);
        if (value.isString())
            field = value.toString().trimmed();
    };
    take(u"title", metadata.title);
    take(u"description", metadata.description);
    take(u"location", metadata.location);
    take(u"quote", metadata.quote);
    take(u"quoteAuthor", metadata.quoteAuthor);
    if (const QJsonValue enabled = sidecar.value(u"enabled"); enabled.isBool())
        metadata.enabled = enabled.toBool();
}

bool updateSidecar(const QString &imagePath, const QJsonObject &changes, QString *error)
{
    const QString path = sidecarPath(imagePath);
    QJsonObject sidecar;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray content = file.readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(content);
        // Never overwrite a file someone wrote by hand that we cannot read
        if (!doc.isObject() && !content.trimmed().isEmpty()) {
            if (error)
                *error = QStringLiteral("%1 ist kein gültiges JSON").arg(path);
            return false;
        }
        sidecar = doc.object();
        file.close();
    }

    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        if (it.value().isNull() || it.value().isUndefined())
            sidecar.remove(it.key());
        else
            sidecar.insert(it.key(), it.value());
    }

    if (sidecar.isEmpty()) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            if (error)
                *error = QStringLiteral("%1 nicht löschbar").arg(path);
            return false;
        }
        return true;
    }
    return writeJsonFile(path, sidecar, error);
}

QString formatCamera(const QString &make, const QString &model)
{
    const QString cleanMake = make.trimmed();
    const QString cleanModel = model.trimmed();
    if (cleanModel.isEmpty())
        return cleanMake;
    // "NIKON CORPORATION" + "NIKON Z 6_2" -> "NIKON Z 6_2", "SONY" + "ILCE-7M3" -> "SONY ILCE-7M3"
    const QString brand = cleanMake.section(u' ', 0, 0);
    if (brand.isEmpty() || cleanModel.startsWith(brand, Qt::CaseInsensitive))
        return cleanModel;
    return brand + u' ' + cleanModel;
}

QString formatAperture(double fNumber, const QLocale &locale)
{
    return fNumber > 0 ? QStringLiteral("f/") + formatNumber(fNumber, locale) : QString();
}

QString formatExposureTime(double seconds, const QLocale &locale)
{
    if (seconds <= 0)
        return {};
    // Short exposures as fractions like cameras show them, long ones in seconds
    if (seconds < 0.25)
        return QStringLiteral("1/%1 s").arg(qRound(1 / seconds));
    return formatNumber(seconds, locale) + QStringLiteral(" s");
}

QString formatFocalLength(double millimetres, const QLocale &locale)
{
    return millimetres > 0 ? formatNumber(millimetres, locale) + QStringLiteral(" mm") : QString();
}

QString formatExposureSettings(const ImageMetadata &metadata, const QLocale &locale)
{
    QStringList parts{
        formatAperture(metadata.aperture, locale),
        formatExposureTime(metadata.exposureTime, locale),
        metadata.iso > 0 ? QStringLiteral("ISO %1").arg(metadata.iso) : QString(),
        formatFocalLength(metadata.focalLength, locale),
    };
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

QString joinLocation(const QStringList &parts)
{
    QStringList unique;
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty() && !unique.contains(trimmed, Qt::CaseInsensitive))
            unique.append(trimmed);
    }
    return unique.join(QStringLiteral(", "));
}

} // namespace berry
