// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/undo_location_session.hpp"

#include "bench/post_back.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_file_work.hpp"

#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] UndoLocationSession::Place place_named(const std::string& name) {
    if (name == "folder") {
        return UndoLocationSession::Place::folder;
    }
    return name == "beside" ? UndoLocationSession::Place::beside
                            : UndoLocationSession::Place::engine;
}

[[nodiscard]] std::string name_of(const UndoLocationSession::Place place) {
    switch (place) {
    case UndoLocationSession::Place::folder:
        return "folder";
    case UndoLocationSession::Place::beside:
        return "beside";
    case UndoLocationSession::Place::engine:
        break;
    }
    return "engine";
}

[[nodiscard]] QString shown(const std::string& raw_path) {
    return QString::fromStdString(core::display_raw_path(raw_path));
}

} // namespace

UndoLocationSession::UndoLocationSession(std::shared_ptr<engine::RemoteFileWork> work,
                                         EngineFolderLister folders, QObject* parent)
    : QObject(parent), work_(std::move(work)), folders_(std::move(folders)) {}

QString UndoLocationSession::folderText() const { return shown(folder_); }

QString UndoLocationSession::keptIn() const { return shown(kept_in_); }

void UndoLocationSession::settle(const Place place, std::string folder, std::string kept_in) {
    place_ = place;
    folder_ = std::move(folder);
    kept_in_ = std::move(kept_in);
    loaded_ = true;
}

void UndoLocationSession::load() {
    if (!work_ || busy_) {
        return;
    }
    busy_ = true;
    status_ = tr("Asking the engine…");
    emit changed();
    const QPointer self{this};
    static_cast<void>(QtConcurrent::run([self, work = work_] {
        auto answer = work->backup_location();
        postBack(self, [self, answer = std::move(answer)]() mutable {
            self->busy_ = false;
            if (!answer && answer.error().code == core::ErrorCode::unsupported) {
                // ADR-0260: older than level 3, it cannot be asked or told.
                self->status_ = tr("This engine is too old to choose this: it keeps undo copies "
                                   "beside each file until it is updated.");
            } else if (!answer) {
                self->status_ = tr("Could not ask the engine · %1")
                                    .arg(QString::fromStdString(answer.error().message));
            } else {
                self->settle(place_named(answer->place), std::move(answer->folder),
                             std::move(answer->kept_in));
                self->status_.clear();
            }
            emit self->changed();
        });
    }));
}

void UndoLocationSession::choose(const Place place, std::string folder) {
    if (!work_ || busy_ || (place == Place::folder && folder.empty())) {
        return;
    }
    busy_ = true;
    status_ = tr("Moving the undo copies there…");
    emit changed();
    const QPointer self{this};
    static_cast<void>(QtConcurrent::run([self, work = work_, place, folder = std::move(folder)] {
        auto answer = work->set_backup_location(name_of(place), folder);
        postBack(self, [self, answer = std::move(answer)]() mutable {
            self->busy_ = false;
            if (!answer) {
                self->status_ = tr("Not changed · %1")
                                    .arg(QString::fromStdString(answer.error().message));
            } else {
                self->settle(place_named(answer->place), std::move(answer->folder),
                             std::move(answer->kept_in));
                self->status_ = tr("Undo copies are kept there now");
            }
            emit self->changed();
        });
    }));
}

} // namespace trackknife::bench
