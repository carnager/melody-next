// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractItemModel>

#include <memory>
#include <string>
#include <vector>

namespace trackknife::ui {

class LocalFolderTreeModel final : public QAbstractItemModel {
    Q_OBJECT

  public:
    explicit LocalFolderTreeModel(QObject* parent = nullptr);
    ~LocalFolderTreeModel() override;

    void addRoot(std::string raw_path);
    [[nodiscard]] std::string rawPath(const QModelIndex& index) const;
    [[nodiscard]] bool isDirectory(const QModelIndex& index) const;

    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] bool hasChildren(const QModelIndex& parent = {}) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    // Whether a directory's children are all present. Distinct from
    // !canFetchMore, which is also false while a listing is in flight -- and a
    // caller treating "in flight" as "loaded" sees an empty directory.
    [[nodiscard]] bool isLoaded(const QModelIndex& index) const;
    void fetchMore(const QModelIndex& parent) override;

  signals:
    void directoryError(const QString& message);
    // A directory finished loading, including when it turned out to be empty
    // and so inserted no rows at all.
    void directoryLoaded(const QModelIndex& index);

  private:
    struct Node;
    struct DirectoryEntry;
    struct DirectoryListing;

    [[nodiscard]] Node* nodeFor(const QModelIndex& index) const;
    [[nodiscard]] int rowOf(const Node* node) const;

    std::unique_ptr<Node> root_;
};

} // namespace trackknife::ui
