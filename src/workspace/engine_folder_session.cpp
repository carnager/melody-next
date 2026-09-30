// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/engine_folder_session.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QPointer>

#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] std::string joined(const std::string& folder, const std::string& name) {
    return folder == "/" ? "/" + name : folder + "/" + name;
}

} // namespace

EngineFolderSession::EngineFolderSession(EngineFolderLister lister, std::string start,
                                         QObject* parent)
    : QObject(parent), lister_(std::move(lister)) {
    browse(std::move(start));
}

QString EngineFolderSession::path() const {
    return shown_ ? QString::fromStdString(core::display_raw_path(shown_->path)) : QString{};
}

QStringList EngineFolderSession::folders() const {
    QStringList names;
    if (shown_) {
        for (const auto& name : shown_->folders) {
            names.append(QString::fromStdString(core::display_raw_path(name)));
        }
    }
    return names;
}

bool EngineFolderSession::canGoUp() const {
    return !listing_ && shown_ && shown_->parent.has_value();
}

bool EngineFolderSession::canChoose() const { return !listing_ && shown_.has_value(); }

std::string EngineFolderSession::choice(const int row) const {
    if (!shown_) {
        return {};
    }
    return row >= 0 && row < static_cast<int>(shown_->folders.size())
               ? joined(shown_->path, shown_->folders[static_cast<std::size_t>(row)])
               : shown_->path;
}

void EngineFolderSession::open(const int row) {
    if (shown_ && row >= 0 && row < static_cast<int>(shown_->folders.size())) {
        browse(joined(shown_->path, shown_->folders[static_cast<std::size_t>(row)]));
    }
}

void EngineFolderSession::up() {
    if (shown_ && shown_->parent) {
        browse(*shown_->parent);
    }
}

void EngineFolderSession::browse(std::string path) {
    const auto request = ++request_;
    listing_ = true;
    status_ = QStringLiteral("Listing folders…");
    emit changed();
    const QPointer self{this};
    lister_(std::move(path), [self, request](core::Result<EngineFolderListing> listed) {
        if (!self || request != self->request_) {
            return;
        }
        self->listing_ = false;
        if (!listed) {
            self->status_ = displayText(listed.error().message);
            emit self->changed();
            return;
        }
        self->shown_ = std::move(*listed);
        self->status_ =
            self->shown_->folders.empty() ? QStringLiteral("No folders here") : QString{};
        emit self->changed();
    });
}

} // namespace trackknife::bench
