// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/language_reference.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace trackknife::bench {

class LanguageReferenceTest final : public QObject {
    Q_OBJECT

  private slots:
    // The shipped documents become pages that stand on their own: titled,
    // styled, their tables kept, linked to each other, and with no link left
    // into the source tree.
    void pagesAreWrittenAndLinked() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // To look at the pages: they are kept, and where is said.
        if (qEnvironmentVariableIsSet("TRACKKNIFE_KEEP_REFERENCE")) {
            directory.setAutoRemove(false);
            qInfo("pages in %s", qPrintable(directory.path()));
        }
        QString error;
        const auto page =
            writeLanguageReference(LanguageReference::scripts, directory.path(), &error);
        QVERIFY2(!page.isEmpty(), qPrintable(error));
        QVERIFY(page.endsWith(QStringLiteral("tagging-scripts.html")));

        QFile scripts{page};
        QVERIFY(scripts.open(QIODevice::ReadOnly));
        const auto html = QString::fromUtf8(scripts.readAll());
        QVERIFY(html.contains(QStringLiteral("<title>Tagging scripts reference</title>")));
        QVERIFY(html.contains(QStringLiteral("color-scheme")));
        QVERIFY(html.contains(QStringLiteral("<table")));
        QVERIFY(html.contains(QStringLiteral("$numberby")));
        QVERIFY(html.contains(QStringLiteral("href=\"tkfmt-1.html\"")));
        QVERIFY(!html.contains(QStringLiteral(".md\"")));

        QFile formatting{directory.filePath(QStringLiteral("tkfmt-1.html"))};
        QVERIFY(formatting.open(QIODevice::ReadOnly));
        const auto reference = QString::fromUtf8(formatting.readAll());
        QVERIFY(reference.contains(QStringLiteral("<title>tkfmt-1 reference</title>")));
        QVERIFY(reference.contains(QStringLiteral("$if2")));
        QVERIFY(!reference.contains(QStringLiteral(".md\"")));
    }
};

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LanguageReferenceTest)
#include "language_reference_test.moc"
