// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The library tree's rows, held as what they are -- an entry, the query that
// lists what is under it, whether that is loaded -- rather than as a bag of
// variants per item. Changes come a level at a time: a level of ten thousand
// artists is one insertion, not ten thousand, and a row said again is one
// change, not one per role. What a row shows is the subclass's to say.

#include "trackknife/persistence/local_library.hpp"

#include <QAbstractItemModel>
#include <QIcon>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

namespace trackknife::bench {

class LibraryTreeModel : public QAbstractItemModel {
  public:
    struct Row {
        QString text;
        QString tooltip;
        QIcon icon;
        // An entry's row; none for a heading, "Show more…", or "No matches".
        std::optional<persistence::LibraryEntry> entry;
        // What opening it lists -- or, for "Show more…", the rest.
        std::optional<persistence::LibraryQuery> query;
        bool loaded{false};
        // Said by an entry's description, not a search's flat results.
        bool described{false};
        bool more{false};
        bool enabled{true};
        bool draggable{false};
        // An empty level's line: whether it is the whole library that is
        // empty, rather than a search.
        std::optional<bool> empty_state;

        [[nodiscard]] Row* parent() const noexcept { return parent_; }
        [[nodiscard]] int position() const noexcept { return position_; }
        [[nodiscard]] int count() const noexcept { return static_cast<int>(children_.size()); }
        [[nodiscard]] Row* child(int position) const;

      private:
        friend class LibraryTreeModel;
        Row* parent_{nullptr};
        int position_{0};
        std::vector<std::unique_ptr<Row>> children_;
    };

    explicit LibraryTreeModel(QObject* parent = nullptr);

    // The invisible row above the top level.
    [[nodiscard]] Row& root() noexcept { return root_; }
    // The row at an index; the root for an invalid one.
    [[nodiscard]] Row* rowAt(const QModelIndex& index) const;
    [[nodiscard]] QModelIndex indexOf(const Row& row) const;

    void clear();
    // Added at the end, as one insertion; the first of them returned.
    Row* append(Row& parent, std::vector<std::unique_ptr<Row>> rows);
    Row& append(Row& parent, std::unique_ptr<Row> row);
    Row& insert(Row& parent, int position, std::unique_ptr<Row> row);
    // Moved with all it holds, its persistent indexes following.
    void move(Row& parent, int from, int to);
    void remove(Row& parent, int first, int count);
    // What a row shows changed.
    void changed(const Row& row);

    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    // One column, under every row from the first: QML's tree adapter cannot
    // follow a column inserted under a nested row.
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    // Rows whose children are not loaded yet say they have some, so they
    // can be opened.
    [[nodiscard]] bool hasChildren(const QModelIndex& parent = {}) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;

  private:
    static void renumber(Row& parent, int from);

    Row root_;
};

} // namespace trackknife::bench
