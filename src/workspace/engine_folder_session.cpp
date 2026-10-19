// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/engine_folder_session.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QPointer>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] std::string joined(const std::string& folder, const std::string& name) {
    return folder == "/" ? "/" + name : folder + "/" + name;
}

[[nodiscard]] QString shown(const std::string& raw) {
    return QString::fromStdString(core::display_raw_path(raw));
}

} // namespace

EngineFolderSession::EngineFolderSession(EngineFolderLister lister, std::string start,
                                         QObject* parent)
    : QObject(parent), lister_(std::move(lister)) {
    browse(std::move(start));
}

QString EngineFolderSession::path() const { return shown_ ? shown(shown_->path) : QString{}; }

std::optional<std::size_t> EngineFolderSession::indexOf(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= visible_.size()) {
        return std::nullopt;
    }
    return visible_[static_cast<std::size_t>(row)];
}

QStringList EngineFolderSession::folders() const {
    QStringList names;
    if (shown_) {
        for (const auto index : visible_) {
            names.append(shown(shown_->folders[index]));
        }
    }
    return names;
}

bool EngineFolderSession::linked(const int row) const {
    const auto index = indexOf(row);
    return index && shown_ && std::ranges::contains(shown_->linked, shown_->folders[*index]);
}

std::vector<EngineFolderSession::Place> EngineFolderSession::places() const {
    std::vector<Place> places;
    if (!shown_) {
        return places;
    }
    if (!shown_->home.empty()) {
        places.push_back({.name = tr("Home"), .path = shown_->home});
    }
    for (const auto& root : shown_->roots) {
        places.push_back({.name = shown(root), .path = root});
    }
    places.push_back({.name = tr("Root"), .path = "/"});
    return places;
}

std::vector<EngineFolderSession::Place> EngineFolderSession::crumbs() const {
    std::vector<Place> crumbs;
    if (!shown_) {
        return crumbs;
    }
    crumbs.push_back({.name = QStringLiteral("/"), .path = "/"});
    std::string walked;
    std::size_t at = 1;
    const auto& path = shown_->path;
    while (at < path.size()) {
        const auto end = std::min(path.find('/', at), path.size());
        if (end > at) {
            walked += "/" + path.substr(at, end - at);
            crumbs.push_back({.name = shown(path.substr(at, end - at)), .path = walked});
        }
        at = end + 1;
    }
    return crumbs;
}

bool EngineFolderSession::canGoUp() const {
    return !listing_ && shown_ && shown_->parent.has_value();
}

bool EngineFolderSession::canChoose() const { return !listing_ && shown_.has_value(); }

bool EngineFolderSession::canMake() const {
    return !listing_ && shown_.has_value() && static_cast<bool>(lister_.make);
}

std::string EngineFolderSession::choice(const int row) const {
    if (!shown_) {
        return {};
    }
    const auto index = indexOf(row);
    return index ? joined(shown_->path, shown_->folders[*index]) : shown_->path;
}

void EngineFolderSession::open(const int row) {
    if (const auto index = indexOf(row); index && shown_) {
        browse(joined(shown_->path, shown_->folders[*index]));
    }
}

void EngineFolderSession::up() {
    if (shown_ && shown_->parent) {
        browse(*shown_->parent);
    }
}

void EngineFolderSession::setShowHidden(const bool shown) {
    if (show_hidden_ == shown) {
        return;
    }
    show_hidden_ = shown;
    refilter();
    emit changed();
}

void EngineFolderSession::refilter() {
    visible_.clear();
    if (!shown_) {
        return;
    }
    for (std::size_t index = 0; index < shown_->folders.size(); ++index) {
        if (show_hidden_ || !shown_->folders[index].starts_with('.')) {
            visible_.push_back(index);
        }
    }
    status_ = visible_.empty() ? tr("No folders here") : QString{};
}

void EngineFolderSession::browse(std::string path) {
    const auto request = ++request_;
    listing_ = true;
    status_ = tr("Listing folders…");
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
        // Places come with every listing; an older engine says none, and
        // the ones known are kept.
        if (self->shown_ && listed->home.empty() && listed->roots.empty()) {
            listed->home = self->shown_->home;
            listed->roots = self->shown_->roots;
        }
        self->shown_ = std::move(*listed);
        self->refilter();
        emit self->changed();
    });
}

void EngineFolderSession::make(const QString& name) {
    const auto trimmed = name.trimmed();
    if (!canMake() || trimmed.isEmpty()) {
        return;
    }
    listing_ = true;
    status_ = tr("Making the folder…");
    emit changed();
    const QPointer self{this};
    lister_.make(shown_->path, trimmed.toStdString(), [self](core::Result<std::string> made) {
        if (!self) {
            return;
        }
        self->listing_ = false;
        if (!made) {
            self->status_ = tr("Not made · %1").arg(displayText(made.error().message));
            emit self->changed();
            return;
        }
        self->browse(std::move(*made));
    });
}

} // namespace trackknife::bench
