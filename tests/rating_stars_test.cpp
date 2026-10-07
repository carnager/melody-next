// SPDX-License-Identifier: GPL-3.0-only

// Stars clicked to rate: the header's strip, and the Rate menu's rows.

#include "uicommon/rating_stars.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QMenu>
#include <QSignalSpy>
#include <QtTest>

namespace trackknife::ui {

class RatingStarsTest final : public QObject {
    Q_OBJECT

  private slots:
    void theStripRatesByHalfAndWholeStars();
    void aRatingRowClosesEveryMenuOpen();
};

// As the phone's and the bar widget's: the left half of a star gives the
// half, the right half the whole; the rating already set clears it.
void RatingStarsTest::theStripRatesByHalfAndWholeStars() {
    RatingStrip strip;
    strip.resize(strip.sizeHint());
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));
    QSignalSpy rated{&strip, &RatingStrip::rated};
    // Star n spans [2 + 19(n-1), 2 + 19(n-1) + 16): its halves 8 apart.
    const auto left_of = [](const int star) { return QPoint{2 + 19 * (star - 1) + 4, 10}; };
    const auto right_of = [](const int star) { return QPoint{2 + 19 * (star - 1) + 12, 10}; };

    QTest::mouseClick(&strip, Qt::LeftButton, {}, right_of(3));
    QCOMPARE(rated.count(), 1);
    QCOMPARE(rated.takeFirst().at(0).toUInt(), 6U);
    QCOMPARE(strip.rating(), 6U);
    QTest::mouseClick(&strip, Qt::LeftButton, {}, left_of(4));
    QCOMPARE(rated.takeFirst().at(0).toUInt(), 7U);
    QTest::mouseClick(&strip, Qt::LeftButton, {}, left_of(1));
    QCOMPARE(rated.takeFirst().at(0).toUInt(), 1U);
    QTest::mouseClick(&strip, Qt::LeftButton, {}, right_of(5));
    QCOMPARE(rated.takeFirst().at(0).toUInt(), 10U);
    // The rating already set, clicked again: cleared.
    QTest::mouseClick(&strip, Qt::LeftButton, {}, right_of(5));
    QCOMPARE(rated.takeFirst().at(0).toUInt(), 0U);
    QCOMPARE(strip.rating(), 0U);
    // Between stars: nothing.
    QTest::mouseClick(&strip, Qt::LeftButton, {}, QPoint{2 + 16 + 1, 10});
    QCOMPARE(rated.count(), 0);
    // Told from elsewhere, it shows it.
    strip.setRating(4U);
    QCOMPARE(strip.rating(), 4U);
}

// Rated from a row of the Rate submenu, every menu goes, the one the
// submenu opened from too -- as a plain entry closes them -- and the one
// value is taken. It had stayed open, to be rated from again and again.
void RatingStarsTest::aRatingRowClosesEveryMenuOpen() {
    QMenu context;
    auto* rate_menu = context.addMenu(QStringLiteral("Rate"));
    auto* group = new QActionGroup(rate_menu);
    group->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    auto* three = new RatingMenuAction(6U, rate_menu);
    auto* four = new RatingMenuAction(8U, rate_menu);
    for (auto* action : {three, four}) {
        group->addAction(action);
        rate_menu->addAction(action);
    }
    three->setChecked(true);
    QSignalSpy triggered{four, &QAction::triggered};

    context.popup(QPoint{100, 100});
    QVERIFY(QTest::qWaitForWindowExposed(&context));
    rate_menu->popup(QPoint{220, 100});
    QVERIFY(QTest::qWaitForWindowExposed(rate_menu));
    auto* row = rate_menu->findChildren<QWidget*>().isEmpty() ? nullptr : [&] {
        QWidget* found = nullptr;
        for (auto* child : rate_menu->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            if (rate_menu->actionGeometry(four).contains(child->geometry().center())) {
                found = child;
            }
        }
        return found;
    }();
    QVERIFY(row != nullptr);
    QTest::mouseClick(row, Qt::LeftButton, {}, row->rect().center());

    QCOMPARE(triggered.count(), 1);
    QTRY_VERIFY(!rate_menu->isVisible());
    QTRY_VERIFY(!context.isVisible());
    // One rating, one mark.
    QVERIFY(four->isChecked());
    QVERIFY(!three->isChecked());
}

} // namespace trackknife::ui

QTEST_MAIN(trackknife::ui::RatingStarsTest)
#include "rating_stars_test.moc"
