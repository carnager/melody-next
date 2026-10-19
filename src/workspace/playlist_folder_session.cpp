// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/playlist_folder_session.hpp"

#include "bench/post_back.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_file_work.hpp"

#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString shown(const std::string& raw_path) {
    return QString::fromStdString(core::display_raw_path(raw_path));
}

} // namespace

PlaylistFolderSession::PlaylistFolderSession(std::shared_ptr<engine::RemoteFileWork> work,
                                             EngineFolderLister folders, QObject* parent)
    : QObject(parent), work_(std::move(work)), folders_(std::move(folders)) {}

QString PlaylistFolderSession::chosenText() const { return shown(chosen_); }

QString PlaylistFolderSession::defaultText() const { return shown(default_); }

QString PlaylistFolderSession::folderText() const { return shown(folder_); }

void PlaylistFolderSession::load() {
    if (!work_ || busy_) {
        return;
    }
    busy_ = true;
    status_ = tr("Asking the engine…");
    emit changed();
    const QPointer self{this};
    static_cast<void>(QtConcurrent::run([self, work = work_] {
        auto answer = work->playlist_folder();
        postBack(self, [self, answer = std::move(answer)]() mutable {
            self->busy_ = false;
            if (!answer && answer.error().code == core::ErrorCode::unsupported) {
                // ADR-0260: older than level 19, its playlists are its own.
                self->status_ = tr("This engine is too old to keep its playlists as files: they "
                                   "are its own until it is updated.");
            } else if (!answer) {
                self->status_ = tr("Could not ask the engine · %1")
                                    .arg(QString::fromStdString(answer.error().message));
            } else {
                self->folder_ = std::move(answer->folder);
                self->default_ = std::move(answer->default_folder);
                self->chosen_ = std::move(answer->chosen);
                self->style_ = std::move(answer->style);
                self->loaded_ = true;
                self->status_.clear();
            }
            emit self->changed();
        });
    }));
}

void PlaylistFolderSession::choose(std::optional<std::string> folder,
                                   std::optional<std::string> style) {
    if (!work_ || busy_ || (!folder && !style)) {
        return;
    }
    busy_ = true;
    status_ = folder ? tr("Writing the playlists there…") : QString{};
    emit changed();
    const QPointer self{this};
    static_cast<void>(QtConcurrent::run([self, work = work_, folder = std::move(folder),
                                         style = std::move(style)] {
        auto answer = work->set_playlist_folder(folder, style);
        postBack(self, [self, answer = std::move(answer), moved = folder.has_value()]() mutable {
            self->busy_ = false;
            if (!answer) {
                self->status_ =
                    tr("Not changed · %1").arg(QString::fromStdString(answer.error().message));
            } else {
                self->folder_ = std::move(answer->folder);
                self->default_ = std::move(answer->default_folder);
                self->chosen_ = std::move(answer->chosen);
                self->style_ = std::move(answer->style);
                self->loaded_ = true;
                self->status_ = moved ? tr("The playlists are there now") : QString{};
            }
            emit self->changed();
        });
    }));
}

} // namespace trackknife::bench
