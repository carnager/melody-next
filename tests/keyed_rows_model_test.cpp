// SPDX-License-Identifier: GPL-3.0-only
#include "quick/keyed_rows_model.hpp"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTest>

namespace trackknife::quick {

class KeyedRowsModelTest final : public QObject {
    Q_OBJECT

  private:
    static QList<QVariantMap> named(const QStringList& titles) {
        QList<QVariantMap> rows;
        for (const auto& title : titles) {
            rows.push_back({{QStringLiteral("title"), title}});
        }
        return rows;
    }
    static QStringList titles(const KeyedRowsModel& model) {
        QStringList shown;
        for (int row = 0; row < model.rowCount(); ++row) {
            shown.push_back(model.index(row).data(Qt::UserRole).toString());
        }
        return shown;
    }

  private slots:
    // Each change is told as what it is -- nothing reset -- and the rows end
    // as given.
    void updatesRowByRowNeverResetting() {
        KeyedRowsModel model({"title"});
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

        model.update({1, 2, 3}, named({"a", "b", "c"}));
        QCOMPARE(titles(model), (QStringList{"a", "b", "c"}));
        QCOMPARE(inserted.size(), 3);

        // One taken from the middle.
        model.update({1, 3}, named({"a", "c"}));
        QCOMPARE(titles(model), (QStringList{"a", "c"}));
        QCOMPARE(removed.size(), 1);

        // One moved up, one new at the end.
        model.update({3, 1, 4}, named({"c", "a", "d"}));
        QCOMPARE(titles(model), (QStringList{"c", "a", "d"}));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(inserted.size(), 4);

        // The same track, changed: its row kept, its values told.
        model.update({3, 1, 4}, named({"c", "A", "d"}));
        QCOMPARE(titles(model), (QStringList{"c", "A", "d"}));
        QCOMPARE(changed.size(), 1);

        // Everything at once: reversed, one gone, one new first.
        model.update({5, 4, 3}, named({"e", "d", "c"}));
        QCOMPARE(titles(model), (QStringList{"e", "d", "c"}));
        QCOMPARE(model.count(), 3);

        model.update({}, {});
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(resets.size(), 0);
    }
};

} // namespace trackknife::quick

QTEST_GUILESS_MAIN(trackknife::quick::KeyedRowsModelTest)
#include "keyed_rows_model_test.moc"
