// SPDX-License-Identifier: GPL-3.0-only

// The list's cover thumbnails. A cover the tagger shows and the list does not
// is the bug this exists for: the list refused anything over 16 megapixels,
// and a 4320x4000 vinyl scan is 17.

#include "uicommon/local_artwork.hpp"

#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

namespace trackknife::ui {

class LocalArtworkTest final : public QObject {
    Q_OBJECT

  private slots:
    void aLargeScanStillGetsAThumbnail();
    void aDecompressionBombIsRefused();
};

namespace {

// A folder cover beside a track that does not exist: the embedded read fails,
// so the sibling image is what gets decoded.
[[nodiscard]] QImage thumbnailFor(const QTemporaryDir& directory, const int width,
                                  const int height) {
    QImage cover{width, height, QImage::Format_Grayscale8};
    cover.fill(128);
    if (!cover.save(directory.filePath(QStringLiteral("cover.jpg")), "JPEG", 50)) {
        return {};
    }
    const auto track = directory.filePath(QStringLiteral("01.flac")).toStdString();
    return loadLocalArtwork(track);
}

} // namespace

void LocalArtworkTest::aLargeScanStillGetsAThumbnail() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto thumbnail = thumbnailFor(directory, 4320, 4000);
    QVERIFY2(!thumbnail.isNull(), "a 17-megapixel scan is an ordinary cover, not an attack");
    // Decoded straight to thumbnail size, not to the full image.
    QVERIFY(thumbnail.width() <= 128 && thumbnail.height() <= 128);
}

void LocalArtworkTest::aDecompressionBombIsRefused() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    // 36 megapixels, over the shared limit: a small file that would expand
    // into far more memory than any cover needs.
    const auto thumbnail = thumbnailFor(directory, 6000, 6000);
    QVERIFY(thumbnail.isNull());
    QVERIFY(static_cast<std::int64_t>(6000) * 6000 > maximum_artwork_pixels);
}

} // namespace trackknife::ui

QTEST_MAIN(trackknife::ui::LocalArtworkTest)
#include "local_artwork_test.moc"
