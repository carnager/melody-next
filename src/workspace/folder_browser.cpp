// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/folder_browser.hpp"

#include "workspace/sources.hpp"

#include <QDir>
#include <QFile>
#include <QSettings>

#include <algorithm>
#include <memory>

namespace trackknife::bench {

FolderBrowser::FolderBrowser(QObject* parent)
    : QObject(parent), model_(new ui::LocalFolderTreeModel(this)),
      bookmarks_(loadFolderBookmarks()) {}

QVariantList FolderBrowser::bookmarks() const {
    QVariantList list;
    for (const auto& path : bookmarks_) {
        list.push_back(QVariantMap{{QStringLiteral("label"), folderBookmarkLabel(path)},
                                   {QStringLiteral("tooltip"), folderBookmarkTooltip(path)}});
    }
    return list;
}

bool FolderBrowser::isDirectory(const QModelIndex& index) const {
    return index.isValid() && model_->isDirectory(index);
}

std::string FolderBrowser::rawPath(const QModelIndex& index) const {
    return model_->rawPath(index);
}

void FolderBrowser::revealBookmark(const int row) {
    if (row >= 0 && row < static_cast<int>(bookmarks_.size())) {
        reveal(bookmarks_[static_cast<std::size_t>(row)]);
    }
}

void FolderBrowser::addBookmark(const QModelIndex& index) {
    if (isDirectory(index)) {
        addBookmark(model_->rawPath(index));
    }
}

void FolderBrowser::addBookmark(const std::string& raw_path) {
    if (std::ranges::find(bookmarks_, raw_path) != bookmarks_.end()) {
        return;
    }
    bookmarks_.push_back(raw_path);
    storeFolderBookmarks(bookmarks_);
    emit bookmarksChanged();
}

void FolderBrowser::removeBookmark(const int row) {
    if (row < 0 || row >= static_cast<int>(bookmarks_.size())) {
        return;
    }
    bookmarks_.erase(bookmarks_.begin() + row);
    storeFolderBookmarks(bookmarks_);
    emit bookmarksChanged();
}

// Reveals a directory in the lazy tree: walk the path from its root,
// fetching one level at a time and continuing when the rows arrive.
void FolderBrowser::reveal(const std::string& raw_path) {
    for (int row = 0; row < model_->rowCount(); ++row) {
        const auto root_index = model_->index(row, 0);
        const auto root_path = model_->rawPath(root_index);
        if (raw_path == root_path) {
            emit currentRequested(root_index);
            emit expandRequested(root_index);
            return;
        }
        const auto prefix = root_path == "/" ? std::string{"/"} : root_path + '/';
        if (raw_path.starts_with(prefix)) {
            revealStep(QPersistentModelIndex{root_index}, raw_path);
            return;
        }
    }
    // Not under any library root yet: the bookmark becomes a root.
    model_->addRoot(raw_path);
    QSettings settings;
    auto roots = settings.value(QStringLiteral("library/roots")).toList();
    roots.push_back(QByteArray{raw_path.data(), static_cast<qsizetype>(raw_path.size())});
    settings.setValue(QStringLiteral("library/roots"), roots);
    for (int row = 0; row < model_->rowCount(); ++row) {
        const auto root_index = model_->index(row, 0);
        if (model_->rawPath(root_index) == raw_path) {
            emit currentRequested(root_index);
            return;
        }
    }
}

void FolderBrowser::revealStep(const QPersistentModelIndex& parent_index,
                               const std::string& raw_path) {
    if (!parent_index.isValid()) {
        return;
    }
    const QModelIndex parent{parent_index};
    if (!model_->isLoaded(parent)) {
        // Waits for the listing whether this starts it or another reveal
        // already has: an in-flight listing is not a loaded one.
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            connect(model_, &ui::LocalFolderTreeModel::directoryLoaded, this,
                    [this, connection, parent_index, raw_path](const QModelIndex& loaded) {
                        if (loaded != QModelIndex{parent_index}) {
                            return;
                        }
                        disconnect(*connection);
                        revealStep(parent_index, raw_path);
                    });
        if (model_->canFetchMore(parent)) {
            model_->fetchMore(parent);
        }
        return;
    }
    emit expandRequested(parent);
    for (int row = 0; row < model_->rowCount(parent); ++row) {
        const auto child = model_->index(row, 0, parent);
        const auto child_path = model_->rawPath(child);
        if (child_path == raw_path) {
            emit currentRequested(child);
            return;
        }
        if (raw_path.starts_with(child_path + '/')) {
            revealStep(QPersistentModelIndex{child}, raw_path);
            return;
        }
    }
}

} // namespace trackknife::bench
