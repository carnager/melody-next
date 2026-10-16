// SPDX-License-Identifier: GPL-3.0-only
#include "bench/playlist_transfer_bar.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/lists/m3u8.hpp"

#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <filesystem>
#include <sys/stat.h>

namespace trackknife::bench {
namespace {
std::string path(const QTemporaryDir& dir, const char* name) {
    return QFile::encodeName(dir.filePath(QString::fromUtf8(name))).toStdString();
}
void put(const std::string& name, const QByteArray& bytes) {
    QFile file{QFile::decodeName(QByteArray::fromStdString(name))};
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Cannot write fixture");
}
QByteArray get(const std::string& name) {
    QFile file{QFile::decodeName(QByteArray::fromStdString(name))};
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
lists::PlaylistEntry entry(std::string raw_path) {
    lists::PlaylistEntry value;
    value.raw_path = std::move(raw_path);
    return value;
}
} // namespace
class M3u8Test final : public QObject {
    Q_OBJECT
  private slots:
    void relativeBomExtinfAndMissingOccurrences();
    void rawPathAndDurationRoundTrips();
    void rejectsUnrepresentableAndInvalidInputs();
    void symlinkParentMeaningIsPreserved();
    void publicationNeverClobbersAndCancels();
    void asynchronousImportAndExport();
    void cancellationAndStaleCapture();
};
void M3u8Test::relativeBomExtinfAndMissingOccurrences() {
    const auto parsed = lists::parse_m3u8(
        "\xef\xbb\xbf#EXTM3U\r\n#comment\r\n#EXTINF:12.345,Missing, "
        "title\r\n../gone.flac\r\n../gone.flac\nfile://localhost/tmp/a%20b.flac\n",
        "/music/lists/favs.m3u8");
    QVERIFY(parsed);
    QCOMPARE(parsed->size(), std::size_t{3});
    QCOMPARE((*parsed)[0].raw_path, std::string{"/music/lists/../gone.flac"});
    QCOMPARE((*parsed)[1].raw_path, (*parsed)[0].raw_path);
    QCOMPARE((*parsed)[0].title, std::string{"Missing, title"});
    QCOMPARE((*parsed)[0].duration_ms, std::optional<std::int64_t>{12345});
    QVERIFY((*parsed)[1].title.empty());
    QVERIFY(!(*parsed)[1].duration_ms);
    QCOMPARE((*parsed)[2].raw_path, std::string{"/tmp/a b.flac"});
}
void M3u8Test::rawPathAndDurationRoundTrips() {
    std::vector entries{entry("/music/普通.flac"),         entry("/music/#hash.flac"),
                        entry("/music/ name .flac"),       entry("/music/a:b.flac"),
                        entry("/music/raw-\xff.flac"),     entry("/music/line\nbreak\r.flac"),
                        entry("/elsewhere/song%20?#.flac")};
    entries[0].title = "Song, title";
    entries[0].duration_ms = 12345;
    auto bytes = lists::serialize_m3u8(entries, "/music/export.m3u8");
    QVERIFY(bytes);
    QVERIFY(core::unicodeCodePointCount(*bytes));
    QVERIFY(bytes->find("\n普通.flac\n") != std::string::npos);
    QVERIFY(bytes->find("file:///music/raw-%FF.flac") != std::string::npos);
    const auto parsed = lists::parse_m3u8(*bytes, "/music/export.m3u8");
    QVERIFY(parsed);
    QCOMPARE(parsed->size(), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        QCOMPARE((*parsed)[i].raw_path, entries[i].raw_path);
        QCOMPARE((*parsed)[i].title, entries[i].title);
        QCOMPARE((*parsed)[i].duration_ms, entries[i].duration_ms);
    }
}
void M3u8Test::rejectsUnrepresentableAndInvalidInputs() {
    for (const auto* text :
         {"https://example.org/a.flac\n", "file://server/a.flac\n", "file:///bad%Q0\n",
          "file:///bad%00\n", "\xff\n", "#EXT-X-TARGETDURATION:4\n", "#EXTINF:nan,title\nx.flac\n",
          "#EXTVLCOPT:start-time=30\na.flac\n"})
        QVERIFY(!lists::parse_m3u8(text, "/music/test.m3u8"));
    QVERIFY(!lists::parse_m3u8(std::string{"a\0b\n", 4}, "/music/test.m3u8"));
    QVERIFY(!lists::parse_m3u8(std::string(lists::m3u8_max_line + 1, 'x'), "/test.m3u8"));
    QVERIFY(!lists::parse_m3u8("a.flac", "relative.m3u8"));
    auto row = entry("/music/a.flac");
    row.segment = formats::SampleRange{.start_sample = 0, .end_sample = 400};
    auto result = lists::serialize_m3u8(std::span{&row, 1}, "/test.m3u8");
    QVERIFY(!result);
    QCOMPARE(result.error().context.front().value, std::string{"1"});
    row.segment.reset();
    row.selection.subsong_index = 0;
    QVERIFY(!lists::serialize_m3u8(std::span{&row, 1}, "/test.m3u8"));
    row.selection = {};
    row.selection.stream_index = 0;
    QVERIFY(!lists::serialize_m3u8(std::span{&row, 1}, "/test.m3u8"));
    row.selection = {};
    row.logical_reference = "logical";
    QVERIFY(!lists::serialize_m3u8(std::span{&row, 1}, "/test.m3u8"));
    row.logical_reference.reset();
    row.title = "name\n#EXTINF:1,Injected";
    QVERIFY(!lists::serialize_m3u8(std::span{&row, 1}, "/test.m3u8"));
    core::CancellationSource cancelled;
    cancelled.request_cancellation();
    QCOMPARE(lists::parse_m3u8("a.flac", "/test.m3u8", cancelled.token()).error().code,
             core::ErrorCode::cancelled);
    QCOMPARE(lists::serialize_m3u8({}, "/test.m3u8", cancelled.token()).error().code,
             core::ErrorCode::cancelled);
}
void M3u8Test::symlinkParentMeaningIsPreserved() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::filesystem::create_directories(path(dir, "actual/child"));
    std::filesystem::create_directory_symlink(path(dir, "actual/child"), path(dir, "link"));
    put(path(dir, "actual/audio.flac"), "actual");
    put(path(dir, "audio.flac"), "wrong");
    auto parsed = lists::parse_m3u8("link/../audio.flac\n", path(dir, "list.m3u8"));
    QVERIFY(parsed);
    QCOMPARE(get(parsed->front().raw_path), QByteArray{"actual"});
    auto exported = lists::serialize_m3u8(*parsed, path(dir, "export.m3u8"));
    QVERIFY(exported);
    QVERIFY(exported->find("link/../audio.flac") != std::string::npos);
}
void M3u8Test::publicationNeverClobbersAndCancels() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto target = path(dir, "export.m3u8");
    QVERIFY(lists::write_m3u8_new(target, "#EXTM3U\n"));
    const auto second = lists::write_m3u8_new(target, "changed");
    QVERIFY(!second);
    QCOMPARE(second.error().code, core::ErrorCode::conflict);
    QCOMPARE(get(target), QByteArray{"#EXTM3U\n"});
    const auto dangling = path(dir, "dangling.m3u8");
    std::filesystem::create_symlink(path(dir, "missing"), dangling);
    QCOMPARE(lists::write_m3u8_new(dangling, "changed").error().code, core::ErrorCode::conflict);
    QVERIFY(std::filesystem::is_symlink(dangling));
    core::CancellationSource cancelled;
    cancelled.request_cancellation();
    const auto aborted = path(dir, "cancelled.m3u8");
    QVERIFY(!lists::write_m3u8_new(aborted, "#EXTM3U\n", cancelled.token()));
    QVERIFY(!std::filesystem::exists(aborted));
    const auto fifo = path(dir, "fifo.m3u8");
    QVERIFY(::mkfifo(fifo.c_str(), 0600) == 0);
    QVERIFY(!lists::read_m3u8(fifo)); // Does not block waiting for a writer.
    for (const auto& file : std::filesystem::directory_iterator(dir.path().toStdString()))
        QVERIFY(!file.path().filename().native().starts_with(".trackbench-playlist-"));
}
void M3u8Test::asynchronousImportAndExport() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile fixture{QStringLiteral(MELODY_AUDIO_FIXTURE_DIR "/tagged-tone-flac.b64")};
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    put(path(dir, "audio.flac"), QByteArray::fromBase64(fixture.readAll()));
    put(path(dir, "input.m3u8"),
        "#EXTM3U\naudio.flac\n#EXTINF:1.250,Gone\nmissing.flac\nmissing.flac\n");
    QMainWindow window;
    auto* bar = new PlaylistTransferBar(&window);
    window.addToolBar(bar);
    window.show();
    LocalListModel model;
    QString name;
    connect(bar, &PlaylistTransferBar::imported, &model,
            [&](auto rows, const QString& imported_name) {
                model.replaceRows(std::move(*rows));
                name = imported_name;
            });
    QSignalSpy completed{bar, &PlaylistTransferBar::completed};
    bar->importFile(path(dir, "input.m3u8"));
    QTRY_COMPARE(completed.size(), 1);
    QVERIFY(completed[0][0].toBool());
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(name, QStringLiteral("input"));
    QVERIFY(model.rows()[0].source_revision);
    QVERIFY(!model.rows()[0].metadata.fields.empty());
    QCOMPARE(model.rows()[1].title, std::string{"Gone"});
    QCOMPARE(model.rows()[1].duration_ms, std::optional<std::int64_t>{1250});
    QCOMPARE(model.rows()[1].raw_path, model.rows()[2].raw_path);
    QVERIFY(model.rows()[1].probed);
    bar->exportFile(path(dir, "output.m3u8"), &model);
    QTRY_COMPARE(completed.size(), 2);
    QVERIFY(completed[1][0].toBool());
    auto exported = lists::read_m3u8(path(dir, "output.m3u8"));
    QVERIFY(exported);
    QCOMPARE(exported->size(), std::size_t{3});
    QCOMPARE((*exported)[1].raw_path, model.rows()[1].raw_path);
    auto rows = model.rows();
    rows[1].selection.subsong_index = 1;
    model.replaceRows(std::move(rows));
    bar->exportFile(path(dir, "unsupported.m3u8"), &model);
    QTRY_COMPARE(completed.size(), 3);
    QVERIFY(!completed[2][0].toBool());
    QVERIFY(!std::filesystem::exists(path(dir, "unsupported.m3u8")));
    QVERIFY(completed[2][1].toString().contains(QStringLiteral("2")));
}
void M3u8Test::cancellationAndStaleCapture() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QMainWindow window;
    auto* bar = new PlaylistTransferBar(&window);
    window.addToolBar(bar);
    LocalListModel model;
    LocalTrackRow row;
    row.raw_path = path(dir, "missing.flac");
    row.probed = true;
    model.replaceRows(std::vector<LocalTrackRow>(2000, row));
    QSignalSpy completed{bar, &PlaylistTransferBar::completed};
    const auto output = path(dir, "cancelled.m3u8");
    bar->exportFile(output, &model);
    bar->cancel();
    QCOMPARE(completed.size(), 1);
    QVERIFY(!std::filesystem::exists(output));
    bar->exportFile(output, &model);
    model.removeRowIndexes({0});
    QCOMPARE(completed.size(), 2);
    QVERIFY(!std::filesystem::exists(output));
    put(path(dir, "import.m3u8"), "missing.flac\n");
    QSignalSpy imported{bar, &PlaylistTransferBar::imported};
    bar->importFile(path(dir, "import.m3u8"));
    bar->cancel();
    QTRY_COMPARE(completed.size(), 3);
    QCOMPARE(imported.size(), 0);
    QVERIFY(!completed[2][0].toBool());
    // A fresh operation after cancelled completion must not reuse stale state.
    bar->exportFile(output, &model);
    QTRY_COMPARE(completed.size(), 4);
    QVERIFY(completed[3][0].toBool());
    QVERIFY(std::filesystem::exists(output));
    bar->importFile(path(dir, "import.m3u8"));
    bar->stop();
    QCoreApplication::processEvents();
    QCOMPARE(imported.size(), 0);
}
} // namespace trackknife::bench
QTEST_MAIN(trackknife::bench::M3u8Test)
#include "m3u8_test.moc"
