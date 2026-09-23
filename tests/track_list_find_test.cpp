// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_list_model.hpp"
#include "bench/track_list_find_bar.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QTableView>
#include <QTimer>
#include <QtTest>

namespace trackknife::bench {
namespace {

LocalTrackRow track(std::string title) {
    LocalTrackRow row;
    row.raw_path = "/missing/audio.flac";
    row.title = std::move(title);
    row.probed = true;
    return row;
}

struct Workspace {
    QMainWindow window;
    LocalListModel model;
    QTableView* view{new QTableView(&window)};
    TrackListFindBar* bar{new TrackListFindBar(&window)};
    QLineEdit* query{bar->findChild<QLineEdit*>(QStringLiteral("bench-list-find-query"))};
    QLabel* status{bar->findChild<QLabel*>(QStringLiteral("bench-list-find-status"))};

    explicit Workspace(std::vector<LocalTrackRow> rows) {
        model.replaceRows(std::move(rows));
        view->setModel(&model);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        window.setCentralWidget(view);
        window.addToolBar(Qt::BottomToolBarArea, bar);
        bar->setView(view);
        window.show();
        bar->open();
    }
};

} // namespace

class TrackListFindTest final : public QObject {
    Q_OBJECT
  private slots:
    void navigatesDuplicateOccurrencesWithoutMutatingOrPlaying();
    void searchesCachedFieldsAndEscapedPaths_data();
    void searchesCachedFieldsAndEscapedPaths();
    void preservesSelectionOnNoMatchAndEmptyQuery();
    void discardsStaleResultsAndYieldsBetweenBatches();
    void invalidatesOnStructuralAndMetadataChanges();
    void reportsOversizedTextWithoutClaimingNoMatch();
};

void TrackListFindTest::navigatesDuplicateOccurrencesWithoutMutatingOrPlaying() {
    auto logical = track("Älbum song");
    logical.logical_reference = "cue-second-track";
    logical.segment = formats::SampleRange{.start_sample = 44100, .end_sample = 88200};
    Workspace w{{track("Älbum song"), track("Other"), logical}};
    w.model.setCurrentSource(w.model.source(1), 1);
    const auto original = w.model.rows();
    QSignalSpy changed{&w.model, &QAbstractItemModel::dataChanged};
    QSignalSpy reset{&w.model, &QAbstractItemModel::modelReset};
    QSignalSpy activated{w.view, &QTableView::activated};
    w.query->setText(QStringLiteral("äLBUM"));
    QTRY_COMPARE(w.status->text(), QStringLiteral("Track 1 of 3"));
    QCOMPARE(w.view->currentIndex().row(), 0);
    QTest::keyClick(w.query, Qt::Key_Return);
    QTRY_COMPARE(w.view->currentIndex().row(), 2);
    QCOMPARE(w.view->selectionModel()->selectedRows().size(), 1);
    QTest::keyClick(w.query, Qt::Key_Return);
    QTRY_COMPARE(w.status->text(), QStringLiteral("Wrapped · Track 1 of 3"));
    QTest::keyClick(w.query, Qt::Key_Return, Qt::ShiftModifier);
    QTRY_COMPARE(w.status->text(), QStringLiteral("Wrapped · Track 3 of 3"));
    QVERIFY(w.model.rows() == original);
    QVERIFY(w.model.index(1, 0).data(ui::track_current_role).toBool());
    QCOMPARE(changed.count(), 0);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(activated.count(), 0);
    QVERIFY(!w.model.canUndo());
    QTest::keyClick(w.query, Qt::Key_Escape);
    QTRY_VERIFY(w.bar->isHidden());
    QTRY_VERIFY(w.view->hasFocus());
}

void TrackListFindTest::searchesCachedFieldsAndEscapedPaths_data() {
    QTest::addColumn<QString>("query");
    // ADR-0142: beyond the display projection, arbitrary metadata values
    // (any provenance, any multiplicity) and the formatted duration match.
    // ADR-0153: retained probe technicals (codec, rate, depth, channels)
    // match like MPD rows' audio_format.
    for (const auto* text : {"Björk", "RELEASE", "collective", "2026", "07", "\\xff", "cue title",
                             "hidden GEM", "-6.02 db", "0:07", "wavpack", "44100", "16"})
        QTest::newRow(text) << QString::fromUtf8(text);
}

void TrackListFindTest::searchesCachedFieldsAndEscapedPaths() {
    QFETCH(QString, query);
    auto row = track("Cue title");
    row.artist = "BJÖRK";
    row.album = "Release";
    row.album_artist = "Collective";
    row.date = "2026";
    row.track_number = "07";
    row.raw_path = std::string{"/music/raw-"} + static_cast<char>(0xff) + ".dat";
    row.duration_ms = 7'000;
    row.technicals = LocalTrackTechnicals{
        .codec = "wavpack", .sample_rate = 44'100, .bits = 16, .channels = 2, .bit_rate = 900'000};
    row.metadata.fields.push_back(metadata::MetadataField{
        .canonical_name = "comment",
        .native_name = "COMMENT",
        .values = {"first note", "Hidden gem"},
        .qualifier = {},
        .provenance = metadata::FieldProvenance::embedded,
    });
    row.metadata.fields.push_back(metadata::MetadataField{
        .canonical_name = "replaygaintrackgain",
        .native_name = "REPLAYGAIN_TRACK_GAIN",
        .values = {"-6.02 dB"},
        .qualifier = {},
        .provenance = metadata::FieldProvenance::sidecar,
    });
    Workspace w{{track("Other"), row}};
    w.query->setText(query);
    QTRY_COMPARE(w.status->text(), QStringLiteral("Track 2 of 2"));
    QCOMPARE(w.view->currentIndex().row(), 1);
}

void TrackListFindTest::preservesSelectionOnNoMatchAndEmptyQuery() {
    Workspace w{{track("One"), track("Two")}};
    w.view->setCurrentIndex(w.model.index(1, 0));
    w.query->setText(QStringLiteral("unmatched [literal]"));
    QTRY_COMPARE(w.status->text(), QStringLiteral("No matches"));
    QCOMPARE(w.view->currentIndex().row(), 1);
    w.query->clear();
    QCOMPARE(w.status->text(), QString{});
    w.bar->findNext();
    QCOMPARE(w.view->currentIndex().row(), 1);
    w.model.replaceRows({});
    w.query->setText(QStringLiteral("One"));
    QTRY_COMPARE(w.status->text(), QStringLiteral("List is empty"));
}

void TrackListFindTest::discardsStaleResultsAndYieldsBetweenBatches() {
    std::vector<LocalTrackRow> rows(10'000, track("ordinary"));
    rows.back().title = "last match";
    Workspace w{std::move(rows)};
    w.view->setCurrentIndex(w.model.index(0, 0));
    w.query->setText(QStringLiteral("last match"));
    bool event_delivered = false;
    w.bar->findNext();
    QTimer::singleShot(0, &w.window, [&event_delivered] { event_delivered = true; });
    QTRY_VERIFY(event_delivered);
    QTRY_COMPARE_WITH_TIMEOUT(w.view->currentIndex().row(), 9999, 5'000);

    w.query->setText(QStringLiteral("ordinary"));
    w.bar->findNext();
    // Extending selection need not change the current index; it must still
    // cancel pending find so the new selection is not replaced by a result.
    w.view->selectionModel()->select(w.model.index(1, 0),
                                     QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(w.status->text(), QStringLiteral("Selection changed — search again"));
    QTest::qWait(50);
    QCOMPARE(w.view->currentIndex().row(), 9999);
    QCOMPARE(w.view->selectionModel()->selectedRows().size(), 2);

    // Several replacements can overtake a finished worker whose completion
    // is still queued. Only the latest query may select a row.
    w.query->setText(QStringLiteral("ordinary"));
    w.bar->findNext();
    w.query->setText(QStringLiteral("not present"));
    w.bar->findNext();
    QTRY_COMPARE_WITH_TIMEOUT(w.status->text(), QStringLiteral("No matches"), 5'000);
    QCOMPARE(w.view->currentIndex().row(), 9999);

    w.query->setText(QStringLiteral("ordinary"));
    w.bar->findNext();
    w.bar->dismiss();
    QTest::qWait(50);
    QCOMPARE(w.view->currentIndex().row(), 9999);
    QVERIFY(w.bar->isHidden());

    LocalListModel other;
    other.replaceRows({track("other tab")});
    QTableView other_view;
    other_view.setModel(&other);
    w.bar->open();
    w.bar->findNext();
    w.bar->setView(&other_view);
    QTest::qWait(50);
    QVERIFY(w.bar->isHidden());
    QCOMPARE(w.view->currentIndex().row(), 9999);
    QVERIFY(!other_view.currentIndex().isValid());
}

void TrackListFindTest::invalidatesOnStructuralAndMetadataChanges() {
    Workspace w{{track("first"), track("match"), track("last")}};
    w.view->setCurrentIndex(w.model.index(0, 0));
    w.query->setText(QStringLiteral("match"));
    w.bar->findNext();
    w.model.removeRowIndexes({1});
    QCOMPARE(w.status->text(), QStringLiteral("List changed — search again"));
    QTest::qWait(50);
    QCOMPARE(w.view->currentIndex().row(), 0);
    w.bar->findNext();
    QTRY_COMPARE(w.status->text(), QStringLiteral("No matches"));
    QVERIFY(w.model.undo());
    w.bar->findNext();
    QTRY_COMPARE(w.view->currentIndex().row(), 1);

    w.bar->findNext();
    w.model.reorderRows({1}, 0);
    QTRY_COMPARE(w.status->text(), QStringLiteral("List changed — search again"));
    w.bar->findNext();
    QTRY_COMPARE(w.status->text(), QStringLiteral("Wrapped · Track 1 of 3"));

    w.bar->findNext();
    auto replacement = w.model.rows()[0];
    replacement.title = "updated";
    QVERIFY(w.model.applyMetadata(replacement.raw_path, 0, replacement));
    QCOMPARE(w.status->text(), QStringLiteral("List changed — search again"));
    w.query->setText(QStringLiteral("updated"));
    QTRY_COMPARE(w.status->text(), QStringLiteral("Track 1 of 3"));
    // Artwork/playback-role notifications must not cancel a text search.
    w.bar->findNext();
    w.model.setCurrentSource(w.model.source(2), 2);
    QTRY_COMPARE(w.status->text(), QStringLiteral("Wrapped · Track 1 of 3"));
}

void TrackListFindTest::reportsOversizedTextWithoutClaimingNoMatch() {
    Workspace w{{track(std::string(70'000, 'x'))}};
    w.query->setText(QStringLiteral("not present"));
    QTRY_VERIFY(w.status->text().contains(QStringLiteral("64 KiB")));
    QVERIFY(!w.status->text().contains(QStringLiteral("No matches")));
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::TrackListFindTest)
#include "track_list_find_test.moc"
