// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "uicommon/local_folder_tree_model.hpp"

#include <QObject>
#include <QPersistentModelIndex>
#include <QVariantList>

#include <string>
#include <vector>

namespace trackknife::bench {

// The Sources panel's Folders: this computer's filesystem as a lazy tree,
// its root "/" and home shown, and the bookmarked folders beside it. A
// folder is revealed by walking to it a level at a time; the view is asked
// to open each level and to put its cursor on the folder. Both windows draw
// it.
class FolderBrowser final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)

  public:
    explicit FolderBrowser(QObject* parent = nullptr);

    [[nodiscard]] ui::LocalFolderTreeModel* model() const { return model_; }
    [[nodiscard]] QVariantList bookmarks() const;
    [[nodiscard]] const std::vector<std::string>& bookmarkPaths() const { return bookmarks_; }

    // Walks the tree to `raw_path`; a folder under no root becomes one.
    void reveal(const std::string& raw_path);
    Q_INVOKABLE void revealBookmark(int row);
    void addBookmark(const std::string& raw_path);
    Q_INVOKABLE void addBookmark(const QModelIndex& index);
    Q_INVOKABLE void removeBookmark(int row);
    Q_INVOKABLE [[nodiscard]] bool isDirectory(const QModelIndex& index) const;
    [[nodiscard]] std::string rawPath(const QModelIndex& index) const;

  signals:
    void bookmarksChanged();
    void expandRequested(const QModelIndex& index);
    void currentRequested(const QModelIndex& index);

  private:
    void revealStep(const QPersistentModelIndex& parent, const std::string& raw_path);

    ui::LocalFolderTreeModel* model_{nullptr};
    std::vector<std::string> bookmarks_;
};

} // namespace trackknife::bench
