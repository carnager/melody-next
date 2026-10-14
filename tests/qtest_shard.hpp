// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// One share of a QtTest class, so ctest can run a long class as several
// processes at once. TRACKKNIFE_TEST_SHARD="i/n" (0 <= i < n) runs every n-th
// test -- a function, or one row of a data-driven one -- starting at the i-th,
// in the order QtTest lists them (-datatags). Unset, or with tests named on
// the command line, everything runs as QTest::qExec would.

#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtTest>

#include <cstdio>

namespace trackknife::testing {

inline constexpr char shard_variable[] = "TRACKKNIFE_TEST_SHARD";

// "function" or "function:tag" for each test the binary has, asked of the
// binary itself so that what counts as a test is QtTest's own answer.
[[nodiscard]] inline QList<QByteArray> listedTests() {
    QProcess self;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QString::fromLatin1(shard_variable));
    self.setProcessEnvironment(environment);
    self.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-datatags")});
    if (!self.waitForFinished(30'000) || self.exitStatus() != QProcess::NormalExit ||
        self.exitCode() != 0) {
        return {};
    }
    QList<QByteArray> tests;
    // "Class function" or "Class function tag", the tag possibly with spaces.
    for (const auto& line : self.readAllStandardOutput().split('\n')) {
        const auto function_at = line.indexOf(' ');
        if (function_at < 0) {
            continue;
        }
        const auto tag_at = line.indexOf(' ', function_at + 1);
        tests.push_back(tag_at < 0 ? line.mid(function_at + 1)
                                   : line.mid(function_at + 1, tag_at - function_at - 1) + ':' +
                                         line.mid(tag_at + 1));
    }
    return tests;
}

[[nodiscard]] inline int qExecShard(QObject* object, int argc, char** argv) {
    const auto shard = qgetenv(shard_variable);
    bool names_tests = false;
    for (int i = 1; i < argc; ++i) {
        names_tests = names_tests || argv[i][0] != '-';
    }
    if (shard.isEmpty() || names_tests) {
        return QTest::qExec(object, argc, argv);
    }
    const auto slash = shard.indexOf('/');
    bool index_ok = false;
    bool count_ok = false;
    const auto index = shard.left(slash).toInt(&index_ok);
    const auto count = shard.mid(slash + 1).toInt(&count_ok);
    if (slash <= 0 || !index_ok || !count_ok || count <= 0 || index < 0 || index >= count) {
        std::fprintf(stderr, "%s=%s is not i/n with 0 <= i < n\n", shard_variable,
                     shard.constData());
        return 1;
    }
    const auto listed = listedTests();
    if (listed.isEmpty()) {
        std::fprintf(stderr, "the tests could not be listed with -datatags\n");
        return 1;
    }

    QList<QByteArray> chosen;
    for (qsizetype position = index; position < listed.size(); position += count) {
        chosen.push_back(listed[position]);
    }
    if (chosen.isEmpty()) {
        return 0;
    }
    QList<char*> arguments(argv, argv + argc);
    for (auto& test : chosen) {
        arguments.push_back(test.data());
    }
    return QTest::qExec(object, static_cast<int>(arguments.size()), arguments.data());
}

} // namespace trackknife::testing

// QTEST_MAIN, run by share.
#define TRACKKNIFE_QTEST_SHARDED_MAIN(TestObject)                                                  \
    int main(int argc, char* argv[]) {                                                             \
        QT_PREPEND_NAMESPACE(QTest::Internal::callInitMain)<TestObject>();                         \
        QTEST_MAIN_SETUP()                                                                         \
        TestObject tc;                                                                             \
        QTEST_SET_MAIN_SOURCE_PATH                                                                 \
        return ::trackknife::testing::qExecShard(&tc, argc, argv);                                 \
    }
