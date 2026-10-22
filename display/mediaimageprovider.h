#pragma once

#include <QQuickImageProvider>

// Serves "image://media/<cover|contain>/<percent-encoded absolute path>".
// Decodes JPEGs straight to the size they are shown at (the decoder scales
// while reading, which keeps memory low on small boards) and caps every image
// at the GPU's texture limit.
class MediaImageProvider : public QQuickImageProvider
{
public:
    explicit MediaImageProvider(int maxTextureSize);

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    const int m_maxTextureSize;
};
