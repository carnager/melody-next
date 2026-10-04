// SPDX-License-Identifier: GPL-3.0-only
#include "bench/single_instance.hpp"

#include <QSignalSpy>
#include <QTest>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <optional>
#include <string>
#include <vector>

using trackknife::bench::SingleInstance;

class SingleInstanceTest final : public QObject {
    Q_OBJECT

  private slots:
    // ADR-0267: the first start claims the workspace; a second hands it its
    // files -- raw paths, bytes and all -- and is done; once the first is
    // gone, a new start claims it again.
    void secondStartHandsOverToTheFirst() {
        const auto key = QUuid::createUuid().toString();
        std::optional<SingleInstance> first{std::in_place, key};
        QVERIFY(first->claim({}));
        QSignalSpy asked{&*first, &SingleInstance::asked};
        const std::vector<std::string> paths{"/music/A/01.flac",
                                             std::string{"/music/\xff\xfe raw.flac"}};
        // The second waits for the first's answer: on another thread, as
        // another process would.
        auto second = QtConcurrent::run([key, paths] {
            SingleInstance again{key};
            return again.claim(paths);
        });
        QTRY_VERIFY_WITH_TIMEOUT(second.isFinished(), 10'000);
        QVERIFY(!second.result());
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked.front().front().value<std::vector<std::string>>(), paths);

        // Nothing to hand over: still brought forward.
        auto bare = QtConcurrent::run([key] {
            SingleInstance again{key};
            return again.claim({});
        });
        QTRY_VERIFY_WITH_TIMEOUT(bare.isFinished(), 10'000);
        QVERIFY(!bare.result());
        QCOMPARE(asked.count(), 2);
        QVERIFY(asked.back().front().value<std::vector<std::string>>().empty());

        first.reset();
        SingleInstance next{key};
        QVERIFY(next.claim({}));
    }
};

QTEST_MAIN(SingleInstanceTest)
#include "single_instance_test.moc"
