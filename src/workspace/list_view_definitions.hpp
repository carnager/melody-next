// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/lists/list_view.hpp"

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace trackknife::bench {

// ADR-0282: the list views Trackknife knows -- the shipped ones, which cannot
// be changed, then the user's, kept in Settings (`lists/views`).

inline const QString default_list_view_id =
    QString::fromLatin1(lists::albums_beside_view_id.data(),
                        static_cast<qsizetype>(lists::albums_beside_view_id.size()));

[[nodiscard]] std::vector<lists::ListView> listViews();
// The user's own; an error when the saved ones cannot be read -- a newer
// Trackknife's, say -- and then they are not written over either.
[[nodiscard]] core::Result<std::vector<lists::ListView>> customListViews();
// Replaces the user's views; each must be valid.
[[nodiscard]] core::Result<void> saveCustomListViews(const std::vector<lists::ListView>& views);
// A view by id, shipped or the user's.
[[nodiscard]] std::optional<lists::ListView> findListView(const QString& id);
// Compiled, and kept while it is unchanged: every tab of one view shares it.
[[nodiscard]] std::shared_ptr<const lists::CompiledListView> compiledListView(const QString& id);
// An id no view has.
[[nodiscard]] QString newListViewId();
// `from` with its columns arranged as `arranged` -- the user's own copy, named
// after it, which is added and returned; an existing copy that is already so
// is reused rather than made again.
[[nodiscard]] core::Result<QString> listViewCopy(const lists::ListView& from,
                                                 std::vector<lists::ViewColumn> arranged,
                                                 const QString& name);

// Says when the user's views changed, so every list drawn by one redraws.
class ListViewCatalog final : public QObject {
    Q_OBJECT

  public:
    static ListViewCatalog& instance();

  signals:
    void changed();
};

} // namespace trackknife::bench
