// SPDX-License-Identifier: GPL-3.0-only

// The library tree followed in place (ADR-0268): a level asked again is
// merged into the rows it has -- each kept with what it holds, moved where
// the engine now lists it -- and a row whose entry did not change is not
// said again, so a change to one album does not redraw the whole tree.

#include "bench/catalogue_source.hpp"
#include "workspace/library_browser.hpp"

#include <QElapsedTimer>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

#include <string>
#include <vector>

namespace trackknife::bench {
namespace {

[[nodiscard]] persistence::LibraryEntry artist(const std::string& name,
                                               const std::size_t tracks = 10U) {
    persistence::LibraryEntry entry;
    entry.kind = persistence::LibraryEntryKind::artist;
    entry.key = name;
    entry.label = name;
    entry.artist = name;
    entry.tracks = tracks;
    entry.available = tracks;
    return entry;
}

[[nodiscard]] persistence::LibraryPage page(const std::vector<std::string>& names) {
    persistence::LibraryPage result;
    for (const auto& name : names) {
        result.entries.push_back(artist(name));
    }
    return result;
}

[[nodiscard]] QStringList labels(const QStandardItem& level) {
    QStringList shown;
    for (int row = 0; row < level.rowCount(); ++row) {
        shown.push_back(level.child(row)->text());
    }
    return shown;
}

} // namespace

class LibraryBrowserTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void aLevelIsMergedInPlace();
    void anUnchangedRowIsNotSaidAgain();
    void aLargeLevelMergesQuickly();

  private:
    void merge(LibraryBrowser& browser, const persistence::LibraryPage& listed) {
        browser.mergeLevel(*browser.model()->invisibleRootItem(), listed, {}, true);
    }
    QTemporaryDir directory_;
};

void LibraryBrowserTest::init() { QStandardPaths::setTestModeEnabled(true); }

void LibraryBrowserTest::aLevelIsMergedInPlace() {
    CatalogueSource catalogues{directory_.path().toStdString() + "/unused.sqlite3",
                               CatalogueSource::Role::remote};
    LibraryBrowser browser{catalogues, EngineKey::local()};
    auto& root = *browser.model()->invisibleRootItem();
    root.removeRows(0, root.rowCount());
    merge(browser, page({"A", "B", "C"}));
    QCOMPARE(labels(root), (QStringList{"A", "B", "C"}));
    auto* a = root.child(0);
    auto* c = root.child(2);
    a->appendRow(new QStandardItem(QStringLiteral("A's album")));

    // Moved, kept with what it holds; gone; new; and one listed twice.
    merge(browser, page({"C", "A", "D", "D"}));
    QCOMPARE(labels(root), (QStringList{"C", "A", "D", "D"}));
    QCOMPARE(root.child(0), c);
    QCOMPARE(root.child(1), a);
    QCOMPARE(a->rowCount(), 1);
    QVERIFY(root.child(2) != root.child(3));

    // What an entry says now is what its row says.
    auto renamed = page({"C", "A"});
    renamed.entries[1].available = 4U;
    merge(browser, renamed);
    QCOMPARE(labels(root), (QStringList{"C", "A — 6 unavailable"}));
    QCOMPARE(root.child(1), a);
    QCOMPARE(root.child(1)->isDragEnabled(), true);

    // Nothing at all: the level says so.
    merge(browser, page({}));
    QCOMPARE(root.rowCount(), 1);
    QVERIFY(!root.child(0)->isEnabled());
}

void LibraryBrowserTest::anUnchangedRowIsNotSaidAgain() {
    CatalogueSource catalogues{directory_.path().toStdString() + "/unused.sqlite3",
                               CatalogueSource::Role::remote};
    LibraryBrowser browser{catalogues, EngineKey::local()};
    auto& root = *browser.model()->invisibleRootItem();
    root.removeRows(0, root.rowCount());
    merge(browser, page({"A", "B", "C"}));
    int changed = 0;
    QObject::connect(browser.model(), &QAbstractItemModel::dataChanged, [&changed] { ++changed; });
    merge(browser, page({"A", "B", "C"}));
    QCOMPARE(changed, 0);
    auto counted = page({"A", "B", "C"});
    counted.entries[1].tracks = 11U;
    counted.entries[1].available = 11U;
    merge(browser, counted);
    QVERIFY(changed > 0);
    QCOMPARE(root.child(1)->data(Qt::ToolTipRole).toString(), QStringLiteral("B"));
}

// Measured, not judged: a level of many thousands merged as it was.
void LibraryBrowserTest::aLargeLevelMergesQuickly() {
    CatalogueSource catalogues{directory_.path().toStdString() + "/unused.sqlite3",
                               CatalogueSource::Role::remote};
    LibraryBrowser browser{catalogues, EngineKey::local()};
    auto& root = *browser.model()->invisibleRootItem();
    root.removeRows(0, root.rowCount());
    std::vector<std::string> names;
    for (int index = 0; index < 20'000; ++index) {
        names.push_back("Artist " + std::to_string(100'000 + index));
    }
    merge(browser, page(names));
    // Shown, as the panel shows it: what a row says again is drawn again.
    QTreeView tree;
    tree.setModel(browser.model());
    tree.resize(400, 800);
    tree.show();
    QCoreApplication::processEvents();
    QElapsedTimer timer;
    timer.start();
    merge(browser, page(names));
    const auto same = timer.nsecsElapsed();
    // One new near the top: every row after it found again.
    names.insert(names.begin() + 10, "Artist 0");
    timer.restart();
    merge(browser, page(names));
    const auto inserted = timer.nsecsElapsed();
    QCOMPARE(root.rowCount(), 20'001);
    qInfo("merge of 20000 rows: unchanged %.1f ms, one inserted %.1f ms",
          static_cast<double>(same) / 1e6, static_cast<double>(inserted) / 1e6);
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LibraryBrowserTest)
#include "library_browser_merge_test.moc"
