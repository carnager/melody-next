// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/local_files_mime_data.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractTableModel>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QMimeData>
#include <QScrollBar>
#include <QUrl>
#include <QtTest>

namespace trackknife::ui {
namespace {

class CueBatchModel final : public QAbstractTableModel {
  public:
    int group_size{0};
    void appendCueAlbum(const int tracks) {
        if (tracks <= 0) {
            return;
        }
        const auto first = rows_;
        beginInsertRows({}, first, first + tracks - 1);
        rows_ += tracks;
        endInsertRows();
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : rows_;
    }

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : track_column_count;
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid()) {
            return {};
        }
        if (role == track_album_group_start_role) {
            return group_size > 0 ? index.row() % group_size == 0 : index.row() == 0;
        }
        if (role == track_album_artist_role) {
            return QStringLiteral("Cue Artist");
        }
        if (role == track_duration_ms_role) {
            return 60'000;
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (index.column()) {
        case track_title_column:
            return QStringLiteral("Cue track %1").arg(index.row() + 1);
        case track_album_column:
            return QStringLiteral("Cue Album");
        case track_date_column:
            return QStringLiteral("2026");
        case track_number_column:
            return QString::number(index.row() + 1);
        default:
            return {};
        }
    }

    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override {
        auto item_flags = QAbstractTableModel::flags(index);
        if (index.isValid()) {
            item_flags |= Qt::ItemIsDragEnabled;
        }
        return item_flags;
    }

    bool removeRows(const int /*row*/, const int /*count*/,
                    const QModelIndex& /*parent*/ = {}) override {
        ++remove_attempts_;
        return false;
    }

    [[nodiscard]] int removeAttempts() const noexcept { return remove_attempts_; }

  private:
    int rows_{0};
    int remove_attempts_{0};
};

} // namespace

class QueueTableViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void preGroupedBatchReservesHeaderAboveFirstTrack();
    void largeGroupedResultsScrollToLastRow();
    void homeAndEndSelectQueueBoundaries();
    void shiftHomeAndEndExtendFromSelectionAnchor();
    void boundaryKeysHandleEmptyQueue();
    void handledDropRestoresRowsAndShowsExactInsertionTarget();
    void typedMoveDoesNotAskModelToRemoveSourceRows();
    void localFilesResolveOnlyOnAcceptedDrop();
};

void QueueTableViewTest::localFilesResolveOnlyOnAcceptedDrop() {
    CueBatchModel model;
    model.appendCueAlbum(2);
    QueueTableView view{nullptr};
    view.setModel(&model);
    view.setAcceptDrops(true);
    view.setDragDropMode(QAbstractItemView::DragDrop);
    view.resize(640, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    int resolutions = 0;
    LocalFilesMimeData files{[&](LocalFilesMimeData::Completion done) {
        ++resolutions;
        done({"/music/raw-\xff.flac"});
    }};
    const auto position = view.visualRect(model.index(1, 0)).topLeft() + QPoint{2, 2};
    QDragEnterEvent rejected{position, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &rejected);
    QVERIFY(!rejected.isAccepted());
    QCOMPARE(resolutions, 0);
    int inserted = -1;
    std::vector<std::string> paths;
    view.setLocalFilesDropCallback([&](const LocalFilesMimeData& payload, int row) {
        inserted = row;
        payload.resolve([&](std::vector<std::string> value) { paths = std::move(value); });
        return true;
    });
    QMimeData counterfeit;
    counterfeit.setData(LocalFilesMimeData::mimeType(), QByteArrayLiteral("1"));
    QDragEnterEvent invalid{position, Qt::CopyAction, &counterfeit, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &invalid);
    QVERIFY(!invalid.isAccepted());
    QDragEnterEvent enter{position, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(resolutions, 0);
    QDropEvent drop{QPointF{position}, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drop.dropAction(), Qt::CopyAction);
    QCOMPARE(inserted, 1);
    QCOMPARE(resolutions, 1);
    QCOMPARE(paths, std::vector<std::string>{"/music/raw-\xff.flac"});
    QCOMPARE(model.rowCount(), 2);
}

void QueueTableViewTest::homeAndEndSelectQueueBoundaries() {
    QueueTableView view{nullptr};
    CueBatchModel model;
    model.appendCueAlbum(5);
    view.setModel(&model);
    view.setSelectionBehavior(QAbstractItemView::SelectRows);
    view.setSelectionMode(QAbstractItemView::ExtendedSelection);
    view.resize(640, 360);
    view.show();
    view.setFocus();
    view.setCurrentIndex(model.index(2, track_title_column));

    QTest::keyClick(&view, Qt::Key_Home);
    QCOMPARE(view.currentIndex().row(), 0);
    QCOMPARE(view.currentIndex().column(), track_title_column);
    QCOMPARE(view.selectionModel()->selectedRows().size(), 1);
    QCOMPARE(view.selectionModel()->selectedRows().front().row(), 0);

    QTest::keyClick(&view, Qt::Key_End);
    QCOMPARE(view.currentIndex().row(), 4);
    QCOMPARE(view.currentIndex().column(), track_title_column);
    QCOMPARE(view.selectionModel()->selectedRows().size(), 1);
    QCOMPARE(view.selectionModel()->selectedRows().front().row(), 4);
}

void QueueTableViewTest::shiftHomeAndEndExtendFromSelectionAnchor() {
    QueueTableView view{nullptr};
    CueBatchModel model;
    model.appendCueAlbum(50);
    view.setModel(&model);
    view.setSelectionBehavior(QAbstractItemView::SelectRows);
    view.setSelectionMode(QAbstractItemView::ExtendedSelection);
    view.resize(640, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setFocus();

    const auto expect_range = [&](const int first, const int last, const int current) {
        QCOMPARE(view.currentIndex(), model.index(current, track_title_column));
        QCOMPARE(view.selectionModel()->selectedRows().size(), last - first + 1);
        for (int row = first; row <= last; ++row) {
            QVERIFY(view.selectionModel()->isRowSelected(row, {}));
        }
        QVERIFY(view.viewport()->rect().intersects(view.visualRect(view.currentIndex())));
    };

    const auto anchor = model.index(4, track_title_column);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                      view.visualRect(anchor).center());
    QTest::keyClick(&view, Qt::Key_Home, Qt::ShiftModifier);
    expect_range(0, 4, 0);
    QTest::keyClick(&view, Qt::Key_Down, Qt::ShiftModifier);
    expect_range(1, 4, 1);
    QTest::keyClick(&view, Qt::Key_End, Qt::ShiftModifier);
    expect_range(4, 49, 49);
    QTest::keyClick(&view, Qt::Key_Up, Qt::ShiftModifier);
    expect_range(4, 48, 48);
    QTest::keyClick(&view, Qt::Key_Home, Qt::ShiftModifier);
    expect_range(0, 4, 0);

    QTest::keyClick(&view, Qt::Key_End);
    expect_range(49, 49, 49);
    QTest::keyClick(&view, Qt::Key_Home, Qt::ShiftModifier);
    expect_range(0, 49, 0);

    QTest::keyClick(&view, Qt::Key_Down);
    expect_range(1, 1, 1);
    QTest::keyClick(&view, Qt::Key_End, Qt::ShiftModifier);
    expect_range(1, 49, 49);
}

void QueueTableViewTest::boundaryKeysHandleEmptyQueue() {
    QueueTableView view{nullptr};
    CueBatchModel model;
    view.setModel(&model);
    for (const auto key : {Qt::Key_Home, Qt::Key_End}) {
        QTest::keyClick(&view, key);
        QTest::keyClick(&view, key, Qt::ShiftModifier);
        QVERIFY(!view.currentIndex().isValid());
        QVERIFY(view.selectionModel()->selectedRows().isEmpty());
    }
}

void QueueTableViewTest::largeGroupedResultsScrollToLastRow() {
    CueBatchModel model;
    model.group_size = 5;
    QueueTableView view{nullptr};
    view.setModel(&model);
    view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view.verticalHeader()->setDefaultSectionSize(22);
    view.verticalHeader()->setMinimumSectionSize(18);
    view.setAlbumGroupingEnabled(true);
    view.resize(640, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    model.appendCueAlbum(2000);
    QTRY_COMPARE(view.rowHeight(1995), 22 + QueueItemDelegate::album_header_height);
    view.setCurrentIndex(model.index(0, track_title_column));
    QTest::keyClick(&view, Qt::Key_End);
    QCOMPARE(view.currentIndex().row(), 1999);
    QTRY_VERIFY(view.viewport()->rect().contains(view.visualRect(view.currentIndex()).center()));
    const auto last = view.visualRect(view.currentIndex());
    QVERIFY(last.bottom() < view.viewport()->height());
    QTest::keyClick(&view, Qt::Key_Up);
    QCOMPARE(view.currentIndex().row(), 1998);
    QVERIFY(view.viewport()->rect().contains(view.visualRect(view.currentIndex()).center()));
    // Removing header heights must also shrink the scroll range.
    view.setAlbumGroupingEnabled(false);
    QTRY_VERIFY(view.verticalScrollBar()->maximum() <= 2000 * 22);
}

void QueueTableViewTest::preGroupedBatchReservesHeaderAboveFirstTrack() {
    QueueTableView view{nullptr};
    QCOMPARE(view.property("trackknife-hover-row").toInt(), -1);
    CueBatchModel model;
    view.setModel(&model);
    view.setItemDelegate(new QueueItemDelegate{&view});
    view.setShowGrid(false);
    view.verticalHeader()->setDefaultSectionSize(22);
    view.verticalHeader()->setMinimumSectionSize(18);
    view.setAlbumGroupingEnabled(true);
    view.resize(640, 360);
    view.show();

    model.appendCueAlbum(12);

    QTRY_COMPARE(view.rowHeight(0), 22 + QueueItemDelegate::album_header_height);
    QCOMPARE(view.rowHeight(1), 22);
    const auto first = view.visualRect(model.index(0, track_title_column));
    const auto second = view.visualRect(model.index(1, track_title_column));
    QCOMPARE(second.top(), first.top() + view.rowHeight(0));
    QVERIFY(first.height() > QueueItemDelegate::album_header_height);
    const QPoint album_header_target{first.center().x(),
                                     first.top() + QueueItemDelegate::album_header_height / 2};
    const QPoint first_track_bottom{first.center().x(), first.bottom() - 1};
    QCOMPARE(view.resolvedDropInsertionRow(album_header_target), 0);
    QCOMPARE(view.resolvedDropInsertionRow(first_track_bottom), 1);
}

void QueueTableViewTest::handledDropRestoresRowsAndShowsExactInsertionTarget() {
    QueueTableView view{nullptr};
    CueBatchModel model;
    model.appendCueAlbum(4);
    view.setModel(&model);
    view.setItemDelegate(new QueueItemDelegate{&view});
    view.setAlbumGroupingEnabled(true);
    view.setAlbumArtworkColumn(track_marker_column);
    view.setAcceptDrops(true);
    view.setDropIndicatorShown(true);
    view.setDragDropOverwriteMode(false);
    int callback_count = 0;
    int callback_url_count = 0;
    int callback_insertion_row = -1;
    view.setLocalUrlDropCallback([&](const QList<QUrl>& urls, const int insertion_row) {
        ++callback_count;
        callback_url_count = static_cast<int>(urls.size());
        callback_insertion_row = insertion_row;
        return true;
    });
    view.verticalHeader()->hide();
    view.horizontalHeader()->hide();
    view.resize(640, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    const auto second = view.visualRect(model.index(1, track_title_column));
    const QPoint target{second.center().x(), second.bottom() - 1};
    QCOMPARE(view.resolvedDropInsertionRow(target), 2);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/drop-target.flac"))});
    QDragEnterEvent enter{target, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move{target, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &move);
    QVERIFY(move.isAccepted());

    QCOMPARE(view.property("trackknife-drop-insertion-row").toInt(), 2);
    QCOMPARE(view.property("trackknife-drop-target-label").toString(),
             QStringLiteral("Copy here · position 3"));
    QVERIFY(view.accessibleDescription().contains(QStringLiteral("Copy here · position 3")));
    QVERIFY(view.viewport()->findChild<QWidget*>(QStringLiteral("track-drop-indicator"),
                                                 Qt::FindDirectChildrenOnly) == nullptr);
    view.viewport()->repaint();
    const auto with_indicator = view.viewport()->grab().toImage();

    QDropEvent drop{QPointF{target}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(callback_count, 1);
    QCOMPARE(callback_url_count, 1);
    QCOMPARE(callback_insertion_row, 2);
    QCOMPARE(view.property("trackknife-drop-insertion-row").toInt(), -1);
    QVERIFY(view.property("trackknife-drop-target-label").toString().isEmpty());
    QVERIFY(view.viewport()->updatesEnabled());
    view.viewport()->repaint();
    const auto without_indicator = view.viewport()->grab().toImage();
    QVERIFY(with_indicator != without_indicator);
    for (int row = 0; row < model.rowCount(); ++row) {
        QVERIFY(!view.visualRect(model.index(row, track_title_column)).isEmpty());
        QVERIFY(!view.isRowHidden(row));
    }
}

void QueueTableViewTest::typedMoveDoesNotAskModelToRemoveSourceRows() {
    QueueTableView view{nullptr};
    CueBatchModel model;
    model.appendCueAlbum(4);
    view.setModel(&model);
    view.setSelectionBehavior(QAbstractItemView::SelectRows);
    view.setSelectionMode(QAbstractItemView::ExtendedSelection);
    view.setDragDropMode(QAbstractItemView::DragDrop);
    view.setDefaultDropAction(Qt::MoveAction);
    view.selectionModel()->select(model.index(1, 0),
                                  QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    bool executed = false;
    bool source_is_view = false;
    bool carries_model_data = false;
    Qt::DropActions executed_actions;
    Qt::DropAction executed_default = Qt::IgnoreAction;
    view.drag_executor_for_testing_ = [&](QDrag* drag, const Qt::DropActions actions,
                                          const Qt::DropAction default_action) {
        executed = true;
        source_is_view = drag->source() == &view;
        carries_model_data =
            drag->mimeData()->hasFormat(QStringLiteral("application/x-qabstractitemmodeldatalist"));
        executed_actions = actions;
        executed_default = default_action;
        return Qt::MoveAction;
    };

    view.startDrag(Qt::MoveAction | Qt::CopyAction);

    QVERIFY(executed);
    QVERIFY(source_is_view);
    QVERIFY(carries_model_data);
    QVERIFY(executed_actions.testFlag(Qt::MoveAction));
    QCOMPARE(executed_default, Qt::MoveAction);
    QCOMPARE(model.removeAttempts(), 0);
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.index(1, track_title_column).data().toString(), QStringLiteral("Cue track 2"));
}

} // namespace trackknife::ui

QTEST_MAIN(trackknife::ui::QueueTableViewTest)

#include "queue_table_view_test.moc"
