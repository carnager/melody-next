// SPDX-License-Identifier: GPL-3.0-only
#include "bench/local_list_edit_bar.hpp"
#include "bench/local_list_model.hpp"
#include "bench/local_playback_service.hpp"
#include "trackknife/lists/edit_plan.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QTableView>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <set>

namespace trackknife::bench {
namespace {
LocalTrackRow row(std::string title, std::string path = "/missing/audio.flac") {
    LocalTrackRow result;
    result.raw_path = std::move(path);
    result.title = std::move(title);
    result.probed = true;
    return result;
}
lists::Entry entry(std::string title, std::string number = {}) {
    lists::Entry result;
    result.raw_path = "/raw-\xff.flac";
    result.display[0] = std::move(title);
    result.display[5] = std::move(number);
    return result;
}
struct Workspace {
    QMainWindow window;
    LocalListModel model;
    QTableView* view{new QTableView(&window)};
    LocalListEditBar* bar{new LocalListEditBar(&window)};
    explicit Workspace(std::vector<LocalTrackRow> rows) {
        model.replaceRows(std::move(rows));
        view->setModel(&model);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        window.setCentralWidget(view);
        window.addToolBar(Qt::BottomToolBarArea, bar);
        bar->setView(view);
        window.show();
    }
};
} // namespace
class LocalListEditTest final : public QObject {
    Q_OBJECT
  private slots:
    void stableNumericUnicodeAndCustomSorting();
    void albumShuffleRetainsOrderAndOccurrences();
    void duplicateIdentityRetainsLogicalSources();
    void invalidAndCancelledPlansDoNotMutate();
    void editsPreserveOccurrencesAndUndo();
    void asynchronousCancellationAndStaleSnapshots();
    void limitsRejectIncompleteEdits();
    void entryIdentitiesStayDistinctAndSurviveReordering();
    void probingPreservesEntryIdentity();
    void thePlaybackServiceDecidesWithoutAWindow();
};

// ADR-0220 Phase 0: the service holds the playback state and the decisions
// that need it, so both can be exercised without constructing
// BenchMainWindow. This is the object the remaining orchestration migrates
// into, and the one Phase 2 serialises.
void LocalListEditTest::thePlaybackServiceDecidesWithoutAWindow() {
    LocalListModel model;
    model.replaceRows({row("A", "/a.flac"), row("B", "/b.flac"), row("C", "/c.flac")});
    const LocalListPlaybackView list{model};
    const auto rows = model.rows();

    LocalPlaybackService playback;
    QVERIFY(!playback.anchors.playing());
    QCOMPARE(playback.resolveRow(list), -1);

    playback.anchors.document = core::StableId::random();
    playback.adopt(rows[0].entry_id, 0, model.source(0));
    playback.order.reset(3, 0, false);
    QVERIFY(playback.anchors.playing());
    QCOMPARE(playback.resolveRow(list), 0);

    const auto next = playback.adjacentRow(list, 1);
    QVERIFY(next.has_value());
    QCOMPARE(next->row, 1);

    // Reordering moves the row without disturbing the identity, and the
    // cached row is only a hint: the service still finds the entry.
    auto reversed = rows;
    std::reverse(reversed.begin(), reversed.end());
    model.replaceRows(reversed);
    QCOMPARE(playback.resolveRow(list), 2);

    // An entry that leaves the list stops playback rather than guessing.
    model.replaceRows({reversed[0], reversed[1]});
    QCOMPARE(playback.resolveRow(list), -1);
    QVERIFY(!playback.adjacentRow(list, 1).has_value());

    playback.stop();
    QVERIFY(!playback.anchors.playing());
    QVERIFY(playback.anchors.source.empty());
    QCOMPARE(playback.row, -1);

    // The service owns the request queue, so it derives for itself the two
    // facts the advance rules need rather than being told them.
    LocalListModel fresh;
    fresh.replaceRows({row("A", "/a.flac"), row("B", "/b.flac")});
    const LocalListPlaybackView list2{fresh};
    LocalPlaybackService serving;
    serving.anchors.document = core::StableId::random();
    serving.adopt(fresh.rows()[0].entry_id, 0, fresh.source(0));
    serving.order.reset(2, 0, false);
    QVERIFY(!serving.requestState().active);
    QVERIFY(serving.requestState().pending_empty);

    // Single + Repeat normally loops the current track, but a pending request
    // is an explicit ask and outranks it.
    serving.modes.single = audio::ModeState::on;
    serving.modes.repeat = true;
    QVERIFY(serving.automaticRow(list2).has_value());
    QVERIFY(serving.requests.insert({fresh.rows()[1]}, 0));
    QVERIFY(!serving.requestState().pending_empty);
    QVERIFY(!serving.automaticRow(list2).has_value());
}

// ADR-0221: a probe refreshes what a row says about its track. It must not
// change which entry the row is, or anything anchored to it -- playback
// position, the request return point -- silently stops resolving the moment
// background enrichment completes.
void LocalListEditTest::probingPreservesEntryIdentity() {
    Workspace workspace{{row("Provisional", "/a.flac"), row("Other", "/b.flac")}};
    auto before = workspace.model.rows();
    // A probe result is a freshly built row, not a copy of the existing one.
    LocalTrackRow probed;
    probed.raw_path = "/a.flac";
    probed.title = "Probed";
    QVERIFY(probed.entry_id != before[0].entry_id);
    QVERIFY(workspace.model.applyMetadata("/a.flac", 0, probed));
    const auto after = workspace.model.rows();
    QCOMPARE(after.size(), 2U);
    QCOMPARE(after[0].title, std::string{"Probed"});
    QCOMPARE(after[0].entry_id, before[0].entry_id);
    QCOMPARE(after[1].entry_id, before[1].entry_id);

    // A probe that discovers several playable entries keeps the identity on
    // the row it replaces; the extra rows are new entries.
    // applyProbeRows only targets an unprobed row, which is what a freshly
    // added path looks like before enrichment runs.
    LocalTrackRow provisional;
    provisional.raw_path = "/c.flac";
    Workspace split{{provisional}};
    const auto original = split.model.rows().at(0).entry_id;
    LocalTrackRow first;
    first.raw_path = "/c.flac";
    LocalTrackRow second;
    second.raw_path = "/c.flac";
    QVERIFY(split.model.applyProbeRows("/c.flac", 0, {first, second}));
    const auto expanded = split.model.rows();
    QCOMPARE(expanded.size(), 2U);
    QCOMPARE(expanded[0].entry_id, original);
    QVERIFY(expanded[1].entry_id != original);
}

// ADR-0221: an entry identity addresses a slot in the list. It must be unique
// within the model however rows arrive, and must travel with its row when the
// list is reordered, because ordering is no longer what identifies an entry.
void LocalListEditTest::entryIdentitiesStayDistinctAndSurviveReordering() {
    Workspace workspace{{row("A"), row("B"), row("C")}};
    const auto initial = workspace.model.rows();
    QCOMPARE(initial.size(), 3U);
    std::set<core::StableId> distinct;
    for (const auto& item : initial) {
        QVERIFY(!item.entry_id.is_nil());
        distinct.insert(item.entry_id);
    }
    QCOMPARE(distinct.size(), 3U);

    // Appending copies of existing rows is ordinary -- duplicating a
    // selection, or copying within a tab -- and a copy carries its source's
    // identity. The model must stamp the arrivals instead of admitting a
    // collision.
    workspace.model.appendRows({initial[0], initial[0], initial[2]});
    const auto grown = workspace.model.rows();
    QCOMPARE(grown.size(), 6U);
    std::set<core::StableId> all;
    for (const auto& item : grown) {
        all.insert(item.entry_id);
    }
    QCOMPARE(all.size(), 6U);
    // The originals keep their identities; only the arrivals are restamped.
    QCOMPARE(grown[0].entry_id, initial[0].entry_id);
    QCOMPARE(grown[2].entry_id, initial[2].entry_id);
    QVERIFY(grown[3].entry_id != initial[0].entry_id);
    QVERIFY(grown[5].entry_id != initial[2].entry_id);
    // A restamped copy still describes the same track.
    QCOMPARE(grown[3], initial[0]);

    auto reversed = grown;
    std::reverse(reversed.begin(), reversed.end());
    workspace.model.replaceRows(reversed);
    const auto after = workspace.model.rows();
    QCOMPARE(after.size(), 6U);
    for (std::size_t position = 0; position < after.size(); ++position) {
        QCOMPARE(after[position].entry_id, grown[grown.size() - 1U - position].entry_id);
    }
}
void LocalListEditTest::albumShuffleRetainsOrderAndOccurrences() {
    std::vector<lists::Entry> entries(8);
    for (auto& item : entries)
        item.display[3] = "Album Artist";
    entries[0].display[2] = entries[3].display[2] = entries[5].display[2] = "A";
    entries[1].display[2] = entries[4].display[2] = "B";
    entries[6].display[2] = "A";
    entries[6].display[4] = "Other edition";
    std::set<std::vector<int>> outcomes;
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
        const lists::EditRequest request{
            .kind = lists::EditKind::shuffle_albums, .expression = {}, .seed = seed};
        const auto plan = lists::plan_edit(entries, request);
        QVERIFY(plan);
        QCOMPARE(lists::plan_edit(entries, request)->positions, plan->positions);
        auto sorted = plan->positions;
        std::sort(sorted.begin(), sorted.end());
        QCOMPARE(sorted, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
        const auto a = std::find(plan->positions.begin(), plan->positions.end(), 0);
        QVERIFY(a + 2 < plan->positions.end());
        QCOMPARE(*(a + 1), 3);
        QCOMPARE(*(a + 2), 5);
        const auto b = std::find(plan->positions.begin(), plan->positions.end(), 1);
        QVERIFY(b + 1 < plan->positions.end());
        QCOMPARE(*(b + 1), 4);
        outcomes.insert(plan->positions);
    }
    QVERIFY(outcomes.size() > 1);
    Workspace workspace{{row("A1"), row("B1"), row("A2")}};
    auto tracks = workspace.model.rows();
    tracks[0].album = tracks[2].album = "A";
    tracks[1].album = "B";
    workspace.model.replaceRows(tracks);
    const QPersistentModelIndex playing{workspace.model.index(2, 0)};
    QSignalSpy edited{workspace.bar, &LocalListEditBar::edited};
    workspace.bar->start({.kind = lists::EditKind::shuffle_albums, .expression = {}, .seed = 3});
    QTRY_COMPARE(edited.size(), 1);
    QVERIFY(playing.isValid());
    QCOMPARE(workspace.model.rows()[static_cast<std::size_t>(playing.row())].title,
             std::string{"A2"});
    QCOMPARE(workspace.model.undoLabel(), QStringLiteral("Shuffle albums"));
    QVERIFY(workspace.model.undo());
    QCOMPARE(workspace.model.rows(), tracks);
}
void LocalListEditTest::stableNumericUnicodeAndCustomSorting() {
    std::vector entries{entry("Éclair", "10/12"), entry("éclair", "2/12"), entry("Apple", "1")};
    auto plan = lists::plan_edit(entries, {.kind = lists::EditKind::sort, .expression = "%TITLE%"});
    QVERIFY(plan);
    QCOMPARE(plan->positions, (std::vector<int>{2, 0, 1}));
    plan = lists::plan_edit(
        entries, {.kind = lists::EditKind::sort, .expression = "%title%", .descending = true});
    QVERIFY(plan);
    QCOMPARE(plan->positions, (std::vector<int>{0, 1, 2}));
    plan =
        lists::plan_edit(entries, {.kind = lists::EditKind::sort, .expression = "%tracknumber%"});
    QVERIFY(plan);
    QCOMPARE(plan->positions, (std::vector<int>{2, 1, 0}));
    entries[0].metadata.fields.push_back({.canonical_name = "genre",
                                          .native_name = "GENRE",
                                          .values = {"Jazz", "Blues"},
                                          .qualifier = {},
                                          .provenance = metadata::FieldProvenance::embedded});
    plan = lists::plan_edit(entries,
                            {.kind = lists::EditKind::sort, .expression = "$getmulti(genre,1)"});
    QVERIFY(plan);
    QCOMPARE(plan->positions, (std::vector<int>{1, 2, 0}));
    plan = lists::plan_edit(entries, {.kind = lists::EditKind::sort, .expression = "$info(PATH)"});
    QVERIFY(plan); // Raw non-UTF-8 paths are escaped, never decoded or resolved.
    QCOMPARE(plan->positions, (std::vector<int>{0, 1, 2}));
    entries = {entry("Track 02"), entry("track 2"), entry("Track 100000000000000000000000000000")};
    plan = lists::plan_edit(
        entries, {.kind = lists::EditKind::sort, .expression = "%title%", .descending = true});
    QVERIFY(plan);
    QCOMPARE(plan->positions, (std::vector<int>{2, 0, 1}));
}
void LocalListEditTest::duplicateIdentityRetainsLogicalSources() {
    const auto original = entry("First");
    std::vector<lists::Entry> entries{original, original};
    entries.back().display[0] = "Different cached title";
    auto chapter = original;
    chapter.segment = formats::SampleRange{.start_sample = 0, .end_sample = 48000};
    entries.push_back(chapter);
    entries.push_back(chapter);
    chapter.segment->start_sample = 48000;
    entries.push_back(chapter);
    auto subsong = original;
    subsong.selection.subsong_index = 0;
    entries.push_back(subsong);
    subsong.selection.subsong_index = 1;
    entries.push_back(subsong);
    auto stream = original;
    stream.selection.stream_index = 0;
    entries.push_back(stream);
    auto logical = original;
    logical.logical_reference = "cue:01";
    entries.push_back(logical);
    logical.logical_reference = "cue:02";
    entries.push_back(logical);
    const auto plan =
        lists::plan_edit(entries, {.kind = lists::EditKind::remove_duplicates, .expression = {}});
    QVERIFY(plan && plan->removal);
    QCOMPARE(plan->positions, (std::vector<int>{1, 3}));
}
void LocalListEditTest::invalidAndCancelledPlansDoNotMutate() {
    const std::vector entries{entry("B"), entry("A")};
    const auto invalid =
        lists::plan_edit(entries, {.kind = lists::EditKind::sort, .expression = "$unknown()"});
    QVERIFY(!invalid);
    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    for (auto kind : {lists::EditKind::sort, lists::EditKind::reverse,
                      lists::EditKind::remove_duplicates, lists::EditKind::shuffle_albums}) {
        const auto stopped = lists::plan_edit(entries, {.kind = kind, .expression = "%title%"},
                                              cancellation.token());
        QVERIFY(!stopped);
        QCOMPARE(stopped.error().code, core::ErrorCode::cancelled);
    }
}
void LocalListEditTest::editsPreserveOccurrencesAndUndo() {
    Workspace workspace{
        {row("C", "/c.flac"), row("A", "/a.flac"), row("B", "/b.flac"), row("copy", "/a.flac")}};
    auto& model = workspace.model;
    const auto original = model.rows();
    model.setCurrentPath("/b.flac", 2);
    workspace.view->selectRow(2);
    const QPersistentModelIndex playing{model.index(2, 0)};
    QSignalSpy resets{&model, &QAbstractItemModel::modelReset};
    QSignalSpy edits{workspace.bar, &LocalListEditBar::edited};
    workspace.bar->start({.kind = lists::EditKind::sort, .expression = "%title%"});
    QTRY_COMPARE(edits.size(), 1);
    QCOMPARE(model.rows()[0].title, std::string{"A"});
    QCOMPARE(playing.row(), 1);
    QCOMPARE(workspace.view->selectionModel()->selectedRows().front().row(), 1);
    QCOMPARE(model.undoLabel(), QStringLiteral("Sort list"));
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), original);
    QCOMPARE(playing.row(), 2);
    QVERIFY(model.redo());
    workspace.bar->start({.kind = lists::EditKind::reverse, .expression = {}});
    QTRY_COMPARE(edits.size(), 2);
    QCOMPARE(model.undoLabel(), QStringLiteral("Reverse list"));
    QVERIFY(model.undo());
    workspace.bar->start({.kind = lists::EditKind::remove_duplicates, .expression = {}});
    QTRY_COMPARE(edits.size(), 3);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.rows().front().title, std::string{"A"});
    QCOMPARE(model.undoLabel(), QStringLiteral("Remove duplicate entries"));
    QVERIFY(model.undo());
    QCOMPARE(model.rowCount(), 4);
    QVERIFY(model.redo());
    QCOMPARE(model.rowCount(), 3);
    QVERIFY(playing.isValid());
    QCOMPARE(resets.size(), 0);
    // No-op ordering does not replace the undo label or erase redo history.
    QVERIFY(model.undo());
    const auto label = model.redoLabel();
    QVERIFY(!model.applyPermutation({0, 1, 2, 3}, QStringLiteral("No-op")));
    QCOMPARE(model.redoLabel(), label);
    QVERIFY(!model.applyPermutation({0, 1, 1, 3}, QStringLiteral("Invalid")));
    QCOMPARE(model.redoLabel(), label);
}
void LocalListEditTest::asynchronousCancellationAndStaleSnapshots() {
    std::vector<LocalTrackRow> rows;
    rows.reserve(10'000);
    for (int i = 0; i < 10'000; ++i)
        rows.push_back(row(std::to_string(10'000 - i), "/" + std::to_string(i)));
    Workspace workspace{rows};
    QSignalSpy edits{workspace.bar, &LocalListEditBar::edited};
    bool yielded = false;
    workspace.bar->start({.kind = lists::EditKind::sort, .expression = "%title%"});
    QTimer::singleShot(0, &workspace.window, [&] {
        yielded = true;
        workspace.bar->cancel();
    });
    QTRY_VERIFY(yielded);
    QVERIFY(!workspace.bar->busy());
    QCOMPARE(workspace.model.rows(), rows);
    workspace.bar->start({.kind = lists::EditKind::reverse, .expression = {}});
    workspace.model.reorderRows({0}, 2);
    QVERIFY(!workspace.bar->busy());
    QCOMPARE(workspace.bar->findChild<QLabel*>()->text(),
             QStringLiteral("List changed — run the command again"));
    workspace.bar->start({.kind = lists::EditKind::reverse, .expression = {}});
    auto changed = rows[4];
    changed.title = "Updated during capture";
    QVERIFY(workspace.model.applyMetadata(changed.raw_path, 4, changed));
    QVERIFY(!workspace.bar->busy());
    workspace.bar->start({.kind = lists::EditKind::reverse, .expression = {}});
    workspace.bar->setView(nullptr);
    QVERIFY(!workspace.bar->busy());
    QTest::qWait(50);
    QCOMPARE(edits.size(), 0);
    workspace.bar->setView(workspace.view);
    workspace.bar->start({.kind = lists::EditKind::reverse, .expression = {}});
    QTRY_COMPARE(edits.size(), 1);
    QVERIFY(workspace.model.undo());
    // Artwork/current-row notifications must not cancel a pending ordering.
    workspace.bar->start({.kind = lists::EditKind::sort, .expression = "%title%"});
    workspace.model.setCurrentPath(rows[2].raw_path, 2);
    QTRY_COMPARE(edits.size(), 2);
}
void LocalListEditTest::limitsRejectIncompleteEdits() {
    Workspace workspace{{row(std::string(70U * 1024U, 'x')), row("A")}};
    workspace.bar->start({.kind = lists::EditKind::sort, .expression = "%title%"});
    QTRY_VERIFY(!workspace.bar->busy());
    QVERIFY(!workspace.model.canUndo());
    QVERIFY(workspace.bar->findChild<QLabel*>()->text().contains(QStringLiteral("limit")));
}
} // namespace trackknife::bench
QTEST_MAIN(trackknife::bench::LocalListEditTest)
#include "local_list_edit_test.moc"
