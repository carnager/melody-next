// SPDX-License-Identifier: GPL-3.0-only

// The tagger's cover conversion: what a size limit in Cover settings turns a
// cover into before it is embedded or saved beside the tracks.

#include "bench/artwork_fitting.hpp"

#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

namespace trackknife::bench {

class ArtworkFittingTest final : public QObject {
    Q_OBJECT

  private slots:
    void aLargeScanBecomesASmallJpeg();
    void aCoverThatFitsIsLeftAlone();
    void transparencyKeepsPng();
    void anOpaquePngBecomesJpeg();
    void aCoverOverTheInputLimitCanStillBeShrunk();
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

void ArtworkFittingTest::transparencyKeepsPng() {
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
    QCOMPARE(fitted->mime_type, std::string{"image/png"});
    const auto result = decoded(*fitted);
    QCOMPARE(result.size(), QSize(500, 500));
    QCOMPARE(qAlpha(result.pixel(0, 0)), 0);
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

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::ArtworkFittingTest)
#include "artwork_fitting_test.moc"
