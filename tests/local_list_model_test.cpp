// SPDX-License-Identifier: GPL-3.0-only

// A list model telling a view only what changed, and finding rows by their
// paths without comparing every row with every path: the answers the same
// as before, the work no longer the size of the whole list.

#include "bench/local_list_model.hpp"
#include "trackknife/lists/list_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QtTest>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace trackknife::bench {
namespace {

[[nodiscard]] LocalTrackRow row(std::string path, std::string album = "Album",
                                std::string title = "Title") {
    LocalTrackRow result;
    result.raw_path = std::move(path);
    result.title = std::move(title);
    result.artist = "Artist";
    result.album = std::move(album);
    result.duration_ms = 1'000;
    result.probed = true;
    return result;
}

// What entriesAt answered before it looked paths up: every row against every
// path, the path itself or a folder above it.
[[nodiscard]] std::vector<core::StableId>
entriesAtByComparison(const LocalListModel& model, const std::vector<std::string>& paths,
                      const bool everything) {
    std::vector<core::StableId> entries;
    for (const auto& each : model.rows()) {
        if (each.segment) {
            continue;
        }
        if (everything || std::ranges::any_of(paths, [&each](const std::string& path) {
                return each.raw_path == path ||
                       (each.raw_path.size() > path.size() && each.raw_path.starts_with(path) &&
                        each.raw_path[path.size()] == '/');
            })) {
            entries.push_back(each.entry_id);
        }
    }
    return entries;
}

// The rows each dataChanged named, by the roles it named.
struct Told {
    std::set<int> rows;
    int signals_seen{0};
};

void listen(LocalListModel& model, Told& told, const int role) {
    QObject::connect(
        &model, &QAbstractItemModel::dataChanged,
        [&told, role](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
            if (!roles.contains(role)) {
                return;
            }
            ++told.signals_seen;
            for (int at = first.row(); at <= last.row(); ++at) {
                told.rows.insert(at);
            }
        });
}

[[nodiscard]] std::shared_ptr<const lists::CompiledListView> groupedByAlbum() {
    auto view = *lists::shipped_list_view(lists::columns_view_id);
    view.grouping = lists::ViewGrouping::expression;
    view.group_key = "%album%";
    view.group_title = "%album% · %rating%";
    view.row_style = "$if($eq(%rating%,7),bold,)";
    for (auto& column : view.columns) {
        if (column.id == "title") {
            column.display = "[%title%]";
            column.alignment = lists::ColumnAlignment::right;
            column.second_line = "%artist%";
        }
    }
    view.row_lines = 2;
    auto compiled = lists::CompiledListView::compile(view);
    if (!compiled) {
        qFatal("%s", compiled.error().message.c_str());
    }
    return std::make_shared<const lists::CompiledListView>(std::move(*compiled));
}

// Evaluated and grouped, headers and all.
void settle(LocalListModel& model) {
    QObject::connect(&model, &LocalListModel::historyWanted, &model, [&model] {
        std::vector<core::StableId> entries;
        for (const auto& request : model.takeHistoryRequests()) {
            entries.push_back(request.entry);
        }
        model.historyUnavailable(entries);
    });
    QTRY_VERIFY_WITH_TIMEOUT(model.expressionGroupsReady(), 10'000);
    const auto last_start = [&model] {
        for (int at = model.rowCount() - 1; at >= 0; --at) {
            if (model.index(at, 0).data(ui::track_album_group_start_role).toBool()) {
                return at;
            }
        }
        return 0;
    }();
    QTRY_VERIFY_WITH_TIMEOUT(
        !model.index(last_start, 0).data(ui::track_group_title_role).toString().isEmpty(), 10'000);
    QTest::qWait(50);
}

// Every role a view of the groups reads, for every row.
[[nodiscard]] QList<QVariantList> groupRoles(const LocalListModel& model) {
    QList<QVariantList> roles;
    for (int at = 0; at < model.rowCount(); ++at) {
        const auto index = model.index(at, 0);
        roles.push_back(
            {index.data(ui::track_album_group_start_role), index.data(ui::track_disc_start_role),
             index.data(ui::track_group_key_role), index.data(ui::track_group_title_role),
             index.data(ui::track_group_details_role)});
    }
    return roles;
}

} // namespace

class LocalListModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void rowsAreFoundAtPathsAndUnderFolders();
    void lookedUpAsTheyWereCompared();
    void ratingsTellOnlyTheRowsTheyChange();
    void scatteredRowsAreToldOnce();
    void regroupingTellsOnlyTheRowsItChanges();
    void aViewsColumnsAreReadAsTheViewSays();
};

void LocalListModelTest::rowsAreFoundAtPathsAndUnderFolders() {
    LocalListModel model;
    auto segment = row("/a/b/sheet.flac");
    segment.segment = formats::SampleRange{.start_sample = 0, .end_sample = 10};
    model.replaceRows({row("/a/b/c.flac"), row("/a/bc/d.flac"), row("/a/b"),
                       row("/x/\xff\xfe/y.flac"), segment, row("relative/z.flac"),
                       row("/a//b/e.flac")});
    const auto& rows = model.rows();
    const auto ids = [&rows](const std::vector<int>& at) {
        std::vector<core::StableId> picked;
        for (const auto index : at) {
            picked.push_back(rows[static_cast<std::size_t>(index)].entry_id);
        }
        return picked;
    };
    // A folder, not a name beginning the same; a CUE track left out.
    QCOMPARE(model.entriesAt({"/a/b"}, false), ids({0, 2}));
    // Bytes that are no UTF-8, as bytes.
    QCOMPARE(model.entriesAt({"/x/\xff\xfe"}, false), ids({3}));
    QCOMPARE(model.entriesAt({"/x/\xff"}, false), ids({}));
    // A folder named with its slash is what is under "/a/b/" -- "/a/b//...".
    QCOMPARE(model.entriesAt({"/a/"}, false), ids({6}));
    // The empty path: every absolute path is under it.
    QCOMPARE(model.entriesAt({""}, false), ids({0, 1, 2, 3, 6}));
    QCOMPARE(model.entriesAt({}, false), ids({}));
    QCOMPARE(model.entriesAt({}, true), ids({0, 1, 2, 3, 5, 6}));
    QCOMPARE(model.entriesAt({"relative"}, false), ids({5}));
}

void LocalListModelTest::lookedUpAsTheyWereCompared() {
    std::mt19937 random{20'261'015U};
    const std::vector<std::string> parts{"a", "b", "ab", "\xff", "\xc3\xa9", "", ".", "x y"};
    const auto path = [&] {
        std::string made;
        const auto depth = std::uniform_int_distribution<int>{0, 4}(random);
        if (std::uniform_int_distribution<int>{0, 5}(random) != 0) {
            made += '/';
        }
        for (int level = 0; level < depth; ++level) {
            made += parts[std::uniform_int_distribution<std::size_t>{0, parts.size() - 1}(random)];
            if (level + 1 < depth || std::uniform_int_distribution<int>{0, 3}(random) == 0) {
                made += '/';
            }
        }
        return made;
    };
    std::vector<LocalTrackRow> rows;
    for (int index = 0; index < 2'000; ++index) {
        rows.push_back(row(path()));
    }
    LocalListModel model;
    model.replaceRows(std::move(rows));
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::string> paths;
        const auto count = std::uniform_int_distribution<int>{0, 6}(random);
        for (int index = 0; index < count; ++index) {
            paths.push_back(path());
        }
        QCOMPARE(model.entriesAt(paths, false), entriesAtByComparison(model, paths, false));
    }
}

void LocalListModelTest::ratingsTellOnlyTheRowsTheyChange() {
    LocalListModel model;
    std::vector<LocalTrackRow> rows;
    for (int index = 0; index < 20; ++index) {
        auto made =
            row("/m/" + std::to_string(index) + ".flac", "Album " + std::to_string(index / 5));
        made.rating_hash = "t" + std::to_string(index);
        made.album_rating_hash = "a" + std::to_string(index / 5);
        rows.push_back(std::move(made));
    }
    model.replaceRows(std::move(rows));
    Told stars;
    Told covers;
    listen(model, stars, ui::track_rating_role);
    listen(model, covers, ui::track_album_rating_role);

    model.applyRatings(QHash<QString, unsigned>{
        {QStringLiteral("t3"), 6U}, {QStringLiteral("t12"), 2U}, {QStringLiteral("t4"), 0U}});
    QCOMPARE(stars.rows, (std::set<int>{3, 12}));
    QCOMPARE(covers.rows, (std::set<int>{3, 12}));
    QCOMPARE(model.rows()[3].rating, 6U);
    QCOMPARE(model.rows()[12].rating, 2U);
    QCOMPARE(model.index(3, local_rating_column).data(ui::track_rating_role).toUInt(), 6U);

    // An album's: each of its rows, told as one run.
    stars = {};
    covers = {};
    model.applyRatings(LocalListModel::RatingsByHash{{"a2", 8U}});
    QCOMPARE(covers.rows, (std::set<int>{10, 11, 12, 13, 14}));
    QCOMPARE(covers.signals_seen, 1);
    QCOMPARE(model.rows()[14].album_rating, 8U);

    // Nothing changed, nothing told; what is not known is asked for.
    stars = {};
    covers = {};
    std::unordered_set<std::string> unknown;
    model.adoptKnownRatings({{"t3", 6U}, {"a0", 0U}, {"a1", 0U}, {"a2", 8U}, {"a3", 0U}}, unknown);
    QCOMPARE(stars.signals_seen + covers.signals_seen, 0);
    QCOMPARE(unknown.size(), 19U);
    QVERIFY(!unknown.contains("t3"));
    QVERIFY(unknown.contains("t12"));

    // What is known is shown at once.
    unknown.clear();
    model.adoptKnownRatings({{"t12", 9U}}, unknown);
    QCOMPARE(stars.rows, (std::set<int>{12}));
    QCOMPARE(model.rows()[12].rating, 9U);
}

void LocalListModelTest::scatteredRowsAreToldOnce() {
    LocalListModel model;
    std::vector<LocalTrackRow> rows;
    for (int index = 0; index < 200; ++index) {
        auto made = row("/m/" + std::to_string(index) + ".flac");
        made.rating_hash = "t" + std::to_string(index);
        rows.push_back(std::move(made));
    }
    model.replaceRows(std::move(rows));
    Told stars;
    listen(model, stars, ui::track_rating_role);
    // A handful apart: one each.
    model.applyRatings(LocalListModel::RatingsByHash{{"t1", 1U}, {"t50", 1U}, {"t51", 1U}});
    QCOMPARE(stars.signals_seen, 2);
    // Scattered over the list: once, from the first to the last.
    stars = {};
    LocalListModel::RatingsByHash every_fourth;
    for (int index = 0; index < 200; index += 4) {
        every_fourth.emplace("t" + std::to_string(index), 5U);
    }
    model.applyRatings(every_fourth);
    QCOMPARE(stars.signals_seen, 1);
    QCOMPARE(*stars.rows.begin(), 0);
    QCOMPARE(*stars.rows.rbegin(), 196);
}

void LocalListModelTest::regroupingTellsOnlyTheRowsItChanges() {
    std::vector<LocalTrackRow> rows;
    for (int index = 0; index < 300; ++index) {
        auto made = row("/m/" + std::to_string(index) + ".flac",
                        "Album " + std::to_string(index / 10), "Title " + std::to_string(index));
        made.rating_hash = "t" + std::to_string(index);
        rows.push_back(std::move(made));
    }
    const auto view = groupedByAlbum();
    LocalListModel model;
    model.replaceRows(rows);
    model.setListView(view);
    settle(model);

    Told groups;
    Told headers;
    listen(model, groups, ui::track_album_group_start_role);
    listen(model, headers, ui::track_group_details_role);
    // A rating its group's header shows: that group's header is evaluated
    // again, and the groups it is in told -- not the list.
    model.applyRatings(LocalListModel::RatingsByHash{{"t150", 7U}});
    QTRY_VERIFY_WITH_TIMEOUT(
        model.index(150, 0).data(ui::track_group_title_role).toString().endsWith(u'7'), 10'000);
    QTest::qWait(50);
    QVERIFY(!groups.rows.empty());
    QVERIFY(groups.rows.size() <= 20U);
    QVERIFY(std::ranges::all_of(groups.rows, [](const int at) { return at >= 140 && at < 160; }));
    QVERIFY(headers.rows.size() <= 2U);

    // As a model made afresh with the same rows says.
    LocalListModel fresh;
    auto rated = model.rows();
    fresh.replaceRows(std::move(rated));
    fresh.setListView(view);
    settle(fresh);
    QCOMPARE(groupRoles(model), groupRoles(fresh));

    // A row gone: the list is told, as before, and says what a fresh one does.
    groups = {};
    model.removeRowIndexes({0}, false);
    QTRY_VERIFY_WITH_TIMEOUT(model.expressionGroupsReady() && !groups.rows.empty(), 10'000);
    QTest::qWait(100);
    LocalListModel shorter;
    auto left = model.rows();
    shorter.replaceRows(std::move(left));
    shorter.setListView(view);
    settle(shorter);
    QCOMPARE(groupRoles(model), groupRoles(shorter));
}

void LocalListModelTest::aViewsColumnsAreReadAsTheViewSays() {
    LocalListModel model;
    auto first = row("/m/1.flac", "Album", "First");
    auto second = row("/m/2.flac", "Album", "Second");
    second.rating_hash = "second";
    model.replaceRows({first, second});
    model.setListView(groupedByAlbum());
    settle(model);
    const auto title = model.index(1, local_title_column);
    QCOMPARE(title.data(Qt::DisplayRole).toString(), QStringLiteral("[Second]"));
    QCOMPARE(title.data(Qt::TextAlignmentRole).value<Qt::Alignment>(),
             Qt::AlignRight | Qt::AlignVCenter);
    QCOMPARE(title.data(ui::track_second_line_role).toString(), QStringLiteral("Artist"));
    // A column showing itself: no expression, no second line.
    const auto artist = model.index(1, local_artist_column);
    QCOMPARE(artist.data(Qt::DisplayRole).toString(), QStringLiteral("Artist"));
    QCOMPARE(artist.data(ui::track_second_line_role).toString(), QString{});
    // The row's style, and none until it says one.
    QVERIFY(!title.data(Qt::FontRole).isValid());
    model.applyRatings(LocalListModel::RatingsByHash{{"second", 7U}});
    QTRY_VERIFY_WITH_TIMEOUT(model.index(1, local_title_column).data(Qt::FontRole).isValid(),
                             10'000);
    QVERIFY(model.index(1, local_title_column).data(Qt::FontRole).value<QFont>().bold());
    QVERIFY(!model.index(0, local_title_column).data(Qt::FontRole).isValid());
    // Another view: its columns, not the last one's.
    model.setListView(nullptr);
    QCOMPARE(model.index(1, local_title_column).data(Qt::DisplayRole).toString(),
             QStringLiteral("Second"));
    QCOMPARE(model.index(1, local_title_column).data(ui::track_second_line_role).toString(),
             QString{});
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LocalListModelTest)
#include "local_list_model_test.moc"
