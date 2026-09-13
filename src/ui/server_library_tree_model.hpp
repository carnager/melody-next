// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/mpd/model.hpp"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QImage>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace trackknife::ui {

struct LibraryTreeLevelDefinition {
    QString name;
    QString grouping_expression;
    QString label_expression;
    QString sort_expression;
    bool omit_when_single{false};

    friend bool operator==(const LibraryTreeLevelDefinition&,
                           const LibraryTreeLevelDefinition&) = default;
};

struct LibraryTreeDefinition {
    QString name;
    QString root_tag;
    std::vector<LibraryTreeLevelDefinition> levels;

    friend bool operator==(const LibraryTreeDefinition&, const LibraryTreeDefinition&) = default;
};

[[nodiscard]] LibraryTreeDefinition defaultLibraryTreeDefinition();
[[nodiscard]] QByteArray serializeLibraryTreeDefinition(const LibraryTreeDefinition& definition);
[[nodiscard]] std::optional<LibraryTreeDefinition>
deserializeLibraryTreeDefinition(const QByteArray& bytes, QString* error = nullptr);

class ServerLibraryTreeModel final : public QAbstractItemModel {
    Q_OBJECT

  public:
    enum ExtraRole {
        KindRole = Qt::UserRole + 1,
        TrackCountRole,
        LoadingRole,
        LevelRole,
        AlbumRole,
        SecondaryTextRole,
        FilterTextRole,
        QueryValueRole
    };
    enum class NodeKind { branch, track };

    explicit ServerLibraryTreeModel(QObject* parent = nullptr);
    ~ServerLibraryTreeModel() override;

    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] bool hasChildren(const QModelIndex& parent = {}) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    [[nodiscard]] const LibraryTreeDefinition& definition() const noexcept;
    [[nodiscard]] QString activeRootTag() const;
    [[nodiscard]] QString setDefinition(LibraryTreeDefinition definition);
    [[nodiscard]] std::vector<mpd::Track> tracks(const QModelIndex& index) const;
    void setArtworkEnabled(bool enabled);
    void reload();
    // Newest-first root ordering for Latest browsing: listed values rank in
    // order, everything else follows alphabetically. Clearing restores the
    // pure alphabetical order.
    void setRootOrdering(const QStringList& newest_first);
    void clearRootOrdering();

  public slots:
    void acceptRoot(quint64 token, const QString& tag, const QStringList& values,
                    const QString& error);
    void acceptAlbumCounts(quint64 token, const std::vector<mpd::ArtistAlbumCount>& counts,
                           const QString& error);
    void acceptBranch(quint64 token, const std::vector<mpd::Track>& tracks, const QString& error);
    void acceptArtwork(quint64 token, const QImage& image);

  signals:
    void rootRequested(quint64 token, const QString& preferred_tag);
    void branchRequested(quint64 token, const QString& tag, const QString& value);
    void artworkRequested(quint64 token, const QString& uri);
    void browseError(const QString& message);

  private:
    struct Impl;
    void requestNextArtwork();
    std::unique_ptr<Impl> implementation_;
};

// Recursive filter over the lazy server library tree. Loaded rows match on
// FilterTextRole; unloaded (or locally unmatched) roots additionally stay
// visible when a bounded server-side search reported matching descendants for
// their exact root-tag value.
class ServerLibraryFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT

  public:
    explicit ServerLibraryFilterModel(QObject* parent = nullptr);

    void setServerMatches(const QStringList& root_values);
    void clearServerMatches();

  protected:
    [[nodiscard]] bool filterAcceptsRow(int source_row,
                                        const QModelIndex& source_parent) const override;

  private:
    QSet<QString> server_matches_;
};

} // namespace trackknife::ui
