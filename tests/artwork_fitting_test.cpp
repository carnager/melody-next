// SPDX-License-Identifier: GPL-3.0-only

// The tagger's cover conversion: what a size limit in Cover settings turns a
// cover into before it is embedded or saved beside the tracks.

#include "bench/artwork_fitting.hpp"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <utility>

namespace trackknife::bench {

class ArtworkFittingTest final : public QObject {
    Q_OBJECT

  private slots:
    void aLargeScanBecomesASmallJpeg();
    void aCoverThatFitsIsLeftAlone();
    void transparencyIsFlattenedIntoAJpeg();
    void anOpaquePngBecomesJpeg();
    void aCoverOverTheInputLimitCanStillBeShrunk();
    void anyImageBecomesTheJpegMelodyWrites();
    void aPhotoIsTurnedUpright();
};

namespace {

[[nodiscard]] metadata::ArtworkImageFile saved(const QTemporaryDir& directory, const QImage& image,
                                               const QString& name, const char* format) {
    const auto path = directory.filePath(name);
    if (!image.save(path, format, 90))
        return {};
    auto file = metadata::read_artwork_image_file(QFile::encodeName(path).toStdString(),
                                                  operations::maximum_fittable_artwork_bytes);
    return file ? *file : metadata::ArtworkImageFile{};
}

[[nodiscard]] QImage decoded(const metadata::ArtworkImageFile& image) {
    return QImage{QFile::decodeName(QByteArray::fromStdString(image.raw_path))};
}

} // namespace

void ArtworkFittingTest::aLargeScanBecomesASmallJpeg() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage scan{4320, 4000, QImage::Format_RGB32};
    scan.fill(Qt::darkRed);
    const auto source = saved(directory, scan, QStringLiteral("scan.jpg"), "JPEG");
    QVERIFY(!source.raw_path.empty());
    const auto fitted =
        fitArtworkImage(source, 1000U, directory.filePath(QStringLiteral("drafts")));
    QVERIFY(fitted.has_value());
    QCOMPARE(fitted->mime_type, std::string{"image/jpeg"});
    QCOMPARE(fitted->width, std::optional<std::uint32_t>{1000U});
    QCOMPARE(fitted->height, std::optional<std::uint32_t>{925U});
    QVERIFY(fitted->content_fingerprint != source.content_fingerprint);
    QVERIFY(QString::fromStdString(fitted->raw_path)
                .startsWith(directory.filePath(QStringLiteral("drafts"))));
    // The same conversion is the same draft, not a second copy.
    const auto again = fitArtworkImage(source, 1000U, directory.filePath(QStringLiteral("drafts")));
    QVERIFY(again && again->raw_path == fitted->raw_path);
}

void ArtworkFittingTest::aCoverThatFitsIsLeftAlone() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage cover{600, 600, QImage::Format_RGB32};
    cover.fill(Qt::darkBlue);
    const auto source = saved(directory, cover, QStringLiteral("cover.jpg"), "JPEG");
    const auto fitted = fitArtworkImage(source, 600U, directory.filePath(QStringLiteral("drafts")));
    QVERIFY(fitted.has_value());
    QCOMPARE(*fitted, source);
    QVERIFY(!QDir{directory.filePath(QStringLiteral("drafts"))}.exists());
}

void ArtworkFittingTest::transparencyIsFlattenedIntoAJpeg() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage logo{2000, 2000, QImage::Format_ARGB32};
    logo.fill(Qt::transparent);
    for (int y = 500; y < 1500; ++y)
        for (int x = 500; x < 1500; ++x)
            logo.setPixel(x, y, qRgba(255, 0, 0, 255));
    const auto source = saved(directory, logo, QStringLiteral("logo.png"), "PNG");
    const auto fitted = fitArtworkImage(source, 500U, directory.path());
    QVERIFY(fitted.has_value());
    // ADR-0317: a cover is a JPEG -- transparency over white.
    QCOMPARE(fitted->mime_type, std::string{"image/jpeg"});
    const auto result = decoded(*fitted);
    QCOMPARE(result.size(), QSize(500, 500));
    QVERIFY(qRed(result.pixel(0, 0)) > 250 && qGreen(result.pixel(0, 0)) > 250 &&
            qBlue(result.pixel(0, 0)) > 250);
}

void ArtworkFittingTest::anOpaquePngBecomesJpeg() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage paste{2000, 1000, QImage::Format_ARGB32};
    paste.fill(qRgba(10, 200, 30, 255));
    const auto source = saved(directory, paste, QStringLiteral("paste.png"), "PNG");
    const auto fitted = fitArtworkImage(source, 800U, directory.path());
    QVERIFY(fitted.has_value());
    QCOMPARE(fitted->mime_type, std::string{"image/jpeg"});
    QCOMPARE(decoded(*fitted).size(), QSize(800, 400));
}

void ArtworkFittingTest::aCoverOverTheInputLimitCanStillBeShrunk() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    // Noise does not compress: well over the 16 MiB a replacement may be.
    QImage noise{3000, 2000, QImage::Format_RGB32};
    auto* random = QRandomGenerator::global();
    for (int y = 0; y < noise.height(); ++y) {
        auto* line = reinterpret_cast<quint32*>(noise.scanLine(y));
        for (int x = 0; x < noise.width(); ++x)
            line[x] = random->generate() | 0xff000000U;
    }
    const auto source = saved(directory, noise, QStringLiteral("noise.png"), "PNG");
    QVERIFY(source.byte_size > 16U * 1024U * 1024U);
    const auto fitted = fitArtworkImage(source, 1500U, directory.path());
    QVERIFY(fitted.has_value());
    QVERIFY(fitted->byte_size < 16U * 1024U * 1024U);
    QCOMPARE(decoded(*fitted).size(), QSize(1500, 1000));
}

// ADR-0317: whatever comes in -- a BMP, a WebP where Qt reads it -- is the
// JPEG Melody writes, no larger than the edge; one that fits keeps its size.
void ArtworkFittingTest::anyImageBecomesTheJpegMelodyWrites() {
    QImage picture{1600, 1200, QImage::Format_RGB32};
    picture.fill(Qt::darkGreen);
    QList<QByteArray> formats{"BMP", "PNG"};
    if (QImageWriter::supportedImageFormats().contains("webp") &&
        QImageReader::supportedImageFormats().contains("webp"))
        formats << "WEBP";
    if (QImageWriter::supportedImageFormats().contains("tiff"))
        formats << "TIFF";
    for (const auto& format : formats) {
        QByteArray encoded;
        QBuffer buffer{&encoded};
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY2(picture.save(&buffer, format.constData()), format.constData());
        buffer.close();
        const auto cover = coverJpeg(encoded, 500U);
        QVERIFY2(cover.has_value(), format.constData());
        QVERIFY(cover->startsWith("\xFF\xD8"));
        const auto decoded = QImage::fromData(*cover, "JPEG");
        QCOMPARE(decoded.size(), QSize(500, 375));
        const auto whole = coverJpeg(encoded, 0U);
        QVERIFY(whole.has_value());
        QCOMPARE(QImage::fromData(*whole, "JPEG").size(), QSize(1600, 1200));
    }
    QVERIFY(!coverJpeg(QByteArray{"not an image"}, 500U).has_value());
}

// A photo's EXIF orientation -- each of the eight -- is undone as the cover
// is made: it comes out as a viewer shows the photo, here as Qt does.
void ArtworkFittingTest::aPhotoIsTurnedUpright() {
    // Four quadrants of their own colour: where each lands says the turn.
    QImage photo{80, 40, QImage::Format_RGB32};
    const std::array colours{QColor{Qt::red}, QColor{Qt::green}, QColor{Qt::blue},
                             QColor{Qt::yellow}};
    for (int y = 0; y < photo.height(); ++y)
        for (int x = 0; x < photo.width(); ++x)
            photo.setPixelColor(x, y,
                                colours[static_cast<std::size_t>((y < photo.height() / 2 ? 0 : 2) +
                                                                 (x < photo.width() / 2 ? 0 : 1))]);
    for (const auto transform :
         {QImageIOHandler::TransformationNone, QImageIOHandler::TransformationMirror,
          QImageIOHandler::TransformationFlip, QImageIOHandler::TransformationRotate180,
          QImageIOHandler::TransformationFlipAndRotate90, QImageIOHandler::TransformationRotate90,
          QImageIOHandler::TransformationMirrorAndRotate90,
          QImageIOHandler::TransformationRotate270}) {
        QByteArray encoded;
        QBuffer buffer{&encoded};
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImageWriter writer{&buffer, "JPEG"};
        writer.setQuality(100);
        writer.setTransformation(transform);
        QVERIFY(writer.write(photo));
        buffer.close();
        QBuffer reading{&encoded};
        QVERIFY(reading.open(QIODevice::ReadOnly));
        QImageReader reader{&reading};
        reader.setAutoTransform(true);
        const auto expected = reader.read();
        QVERIFY(!expected.isNull());
        // Smaller than it is, so it is made anew rather than kept.
        const auto cover = coverJpeg(encoded, 60U);
        QVERIFY(cover.has_value());
        const auto shown = QImage::fromData(*cover, "JPEG");
        QCOMPARE(shown.size(), expected.size().scaled(60, 60, Qt::KeepAspectRatio));
        for (const auto& [fx, fy] : {std::pair{0.25, 0.25}, std::pair{0.75, 0.25},
                                     std::pair{0.25, 0.75}, std::pair{0.75, 0.75}}) {
            const auto want = expected.pixelColor(static_cast<int>(fx * expected.width()),
                                                  static_cast<int>(fy * expected.height()));
            const auto got = shown.pixelColor(static_cast<int>(fx * shown.width()),
                                              static_cast<int>(fy * shown.height()));
            QVERIFY2(std::abs(want.red() - got.red()) < 60 &&
                         std::abs(want.green() - got.green()) < 60 &&
                         std::abs(want.blue() - got.blue()) < 60,
                     qPrintable(QStringLiteral("transform %1").arg(static_cast<int>(transform))));
        }
    }
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::ArtworkFittingTest)
#include "artwork_fitting_test.moc"
