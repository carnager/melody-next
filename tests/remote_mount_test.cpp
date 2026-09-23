// SPDX-License-Identifier: GPL-3.0-only

// ADR-0227: a path moving between a remote engine's tab and this computer's,
// translated through where this computer mounts the remote's music.

#include "bench/remote_mount.hpp"

#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <fstream>

namespace trackknife::bench {

class RemoteMountTest final : public QObject {
    Q_OBJECT

  private slots:
    void foldersMatchWholeComponents();
    void theSamePathsNeedNoConfiguring();
    void aMountElsewhereIsTranslated();
    void onlyTheRemotesLibraryCrossesToIt();
};

void RemoteMountTest::foldersMatchWholeComponents() {
    QVERIFY(path_within("/mnt/nas/Music/a.flac", "/mnt/nas/Music"));
    QVERIFY(path_within("/mnt/nas/Music", "/mnt/nas/Music"));
    QVERIFY(path_within("/mnt/nas/Music/a.flac", "/"));
    // A sibling that merely starts the same is not inside.
    QVERIFY(!path_within("/mnt/nas/Musicals/a.flac", "/mnt/nas/Music"));
    QVERIFY(!path_within("/mnt/nas/Music/a.flac", ""));
}

void RemoteMountTest::theSamePathsNeedNoConfiguring() {
    QTemporaryDir directory;
    const std::filesystem::path track{(directory.path() + QStringLiteral("/a.flac")).toStdString()};
    std::ofstream{track} << "x";
    const RemoteMount same{};
    QCOMPARE(same.to_local(track.string()), std::optional{track.string()});
    // Not there here: the mount is missing, and the track cannot be played
    // or tagged on this computer.
    QVERIFY(!same.to_local(directory.path().toStdString() + "/missing.flac"));
}

void RemoteMountTest::aMountElsewhereIsTranslated() {
    QTemporaryDir directory;
    const auto here = directory.path().toStdString() + "/nas";
    std::filesystem::create_directories(here + "/Artist");
    std::ofstream{here + "/Artist/a.flac"} << "x";
    const RemoteMount mount{.remote_folder = "/srv/music", .local_folder = here};
    QCOMPARE(mount.to_local("/srv/music/Artist/a.flac"), std::optional{here + "/Artist/a.flac"});
    QVERIFY(!mount.to_local("/srv/other/a.flac"));
    QCOMPARE(mount.to_remote(here + "/Artist/a.flac", {"/srv/music"}),
             std::optional<std::string>{"/srv/music/Artist/a.flac"});
}

void RemoteMountTest::onlyTheRemotesLibraryCrossesToIt() {
    const RemoteMount same{};
    // In the remote's library: it has the file.
    QCOMPARE(same.to_remote("/mnt/nas/Music/a.flac", {"/mnt/nas/Music"}),
             std::optional<std::string>{"/mnt/nas/Music/a.flac"});
    // A download on this computer: the remote has no such file.
    QVERIFY(!same.to_remote("/home/me/Downloads/a.flac", {"/mnt/nas/Music"}));
    // Nothing known of its library, nothing known to cross.
    QVERIFY(!same.to_remote("/mnt/nas/Music/a.flac", {}));
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::RemoteMountTest)
#include "remote_mount_test.moc"
