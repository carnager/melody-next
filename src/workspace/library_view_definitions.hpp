// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/persistence/local_library.hpp"

#include <QObject>
#include <QString>

#include <vector>

namespace trackknife::bench {

// ADR-0254: a library view as the window keeps it -- its levels, which the
// engine groups by. The shipped ones are written here and cannot be changed;
// the user's own live in Settings.
struct LibraryViewDefinition {
    QString id;
    QString name;
    std::vector<persistence::LibraryViewLevel> levels;
    bool builtin{false};
    // Shown with the library's own tree -- the artist tree, Recently added,
    // Folders -- not grouped by `levels`, which say the same for a copy to
    // start from (Folders has none: no levels say what it does).
    bool own_tree{false};

    friend bool operator==(const LibraryViewDefinition&, const LibraryViewDefinition&) = default;
};

// The artist tree as it always was: no levels, the engine's own queries.
inline const QString default_library_view_id = QStringLiteral("artist-album");
// Albums newest first, as they came into the library.
inline const QString recent_library_view_id = QStringLiteral("recently-added");
// The library's folders as the engine indexed them, from its roots down.
inline const QString folders_library_view_id = QStringLiteral("folders");

[[nodiscard]] std::vector<LibraryViewDefinition> builtinLibraryViews();
// The shipped views, then the user's, in the order they were made.
[[nodiscard]] std::vector<LibraryViewDefinition> libraryViews();
[[nodiscard]] core::Result<std::vector<LibraryViewDefinition>> customLibraryViews();
// Replaces the user's views. Each must have a name and at least one level,
// and every level must compile as a tree-level tkfmt-1 expression.
[[nodiscard]] core::Result<void>
saveCustomLibraryViews(const std::vector<LibraryViewDefinition>& views);
// Why a level does not compile, or empty when it does.
[[nodiscard]] QString libraryViewLevelError(const QString& source);

// Says when the user's views were saved, so every library shown -- this
// computer's and each engine's -- reads them again.
class LibraryViewCatalog final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static LibraryViewCatalog& instance();

  signals:
    void changed();
};

} // namespace trackknife::bench
