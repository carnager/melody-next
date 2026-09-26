// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/local_folder_tree_model.hpp"

#include "uicommon/local_files_mime_data.hpp"

#include "trackknife/core/local_sources.hpp"

#include <QByteArray>
#include <QFutureWatcher>
#include <QIcon>
#include <QPersistentModelIndex>
#include <QString>
#include <QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace trackknife::ui {
namespace {

constexpr std::size_t children_per_directory_limit = 10'000U;

[[nodiscard]] bool raw_less(const std::string& left, const std::string& right) {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        [](const char first, const char second) {
                                            return static_cast<unsigned char>(first) <
                                                   static_cast<unsigned char>(second);
                                        });
}

[[nodiscard]] QString display_path(const std::string& raw_path, const bool root) {
    const auto displayed = root ? raw_path : std::filesystem::path{raw_path}.filename().native();
    return QString::fromUtf8(core::escape_raw_path(displayed));
}

[[nodiscard]] bool hidden_entry(const std::filesystem::path& path) {
    const auto name = path.filename().native();
    return !name.empty() && name.front() == '.';
}

[[nodiscard]] bool audio_file_name(const std::string& raw_path) {
    const auto dot = raw_path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    auto extension = raw_path.substr(dot + 1U);
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    static constexpr std::array known{"flac", "wv",  "mp3", "ogg", "opus", "m4a", "mp4",
                                      "aac",  "ape", "mpc", "wav", "aiff", "aif", "wma",
                                      "mka",  "dsf", "dff", "cue", "tak",  "tta", "spx"};
    return std::ranges::find(known, extension) != known.end();
}

[[nodiscard]] QIcon entry_icon(const bool directory, const std::string& raw_path) {
    if (directory) {
        static const QIcon folder = QIcon::fromTheme(QStringLiteral("folder"));
        return folder;
    }
    if (audio_file_name(raw_path)) {
        static const QIcon audio = QIcon::fromTheme(QStringLiteral("audio-x-generic"));
        return audio;
    }
    static const QIcon generic = QIcon::fromTheme(QStringLiteral("text-x-generic"));
    return generic;
}

} // namespace

struct LocalFolderTreeModel::Node {
    Node* parent{nullptr};
    std::string raw_path;
    std::vector<std::unique_ptr<Node>> children;
    bool directory{true};
    bool fetched{false};
    bool fetching{false};
};

struct LocalFolderTreeModel::DirectoryEntry {
    std::string raw_path;
    bool directory{false};
};

struct LocalFolderTreeModel::DirectoryListing {
    std::vector<DirectoryEntry> entries;
    std::string error;
};

LocalFolderTreeModel::LocalFolderTreeModel(QObject* parent)
    : QAbstractItemModel(parent), root_(std::make_unique<Node>()) {
    root_->directory = true;
    root_->fetched = true;
}

LocalFolderTreeModel::~LocalFolderTreeModel() = default;

void LocalFolderTreeModel::addRoot(std::string raw_path) {
    if (raw_path.empty()) {
        return;
    }
    const auto duplicate = std::ranges::find_if(
        root_->children, [&raw_path](const auto& child) { return child->raw_path == raw_path; });
    if (duplicate != root_->children.end()) {
        return;
    }
    const auto row = static_cast<int>(root_->children.size());
    beginInsertRows({}, row, row);
    auto node = std::make_unique<Node>();
    node->parent = root_.get();
    node->raw_path = std::move(raw_path);
    root_->children.push_back(std::move(node));
    endInsertRows();
}

std::string LocalFolderTreeModel::rawPath(const QModelIndex& index) const {
    const auto* node = nodeFor(index);
    return node == root_.get() ? std::string{} : node->raw_path;
}

bool LocalFolderTreeModel::isDirectory(const QModelIndex& index) const {
    const auto* node = nodeFor(index);
    return node != root_.get() && node->directory;
}

QModelIndex LocalFolderTreeModel::index(const int row, const int column,
                                        const QModelIndex& parent_index) const {
    if (row < 0 || column != 0 || parent_index.column() > 0) {
        return {};
    }
    auto* parent_node = nodeFor(parent_index);
    if (static_cast<std::size_t>(row) >= parent_node->children.size()) {
        return {};
    }
    return createIndex(row, column, parent_node->children.at(static_cast<std::size_t>(row)).get());
}

QModelIndex LocalFolderTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    const auto* node = nodeFor(child);
    const auto* parent_node = node->parent;
    if (parent_node == nullptr || parent_node == root_.get()) {
        return {};
    }
    return createIndex(rowOf(parent_node), 0, const_cast<Node*>(parent_node));
}

int LocalFolderTreeModel::rowCount(const QModelIndex& parent_index) const {
    if (parent_index.column() > 0) {
        return 0;
    }
    return static_cast<int>(nodeFor(parent_index)->children.size());
}

int LocalFolderTreeModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant LocalFolderTreeModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid()) {
        return {};
    }
    const auto* node = nodeFor(index);
    if (role == Qt::DisplayRole) {
        return display_path(node->raw_path, node->parent == root_.get());
    }
    if (role == Qt::ToolTipRole) {
        return QString::fromUtf8(core::escape_raw_path(node->raw_path));
    }
    if (role == Qt::UserRole) {
        return QByteArray{node->raw_path.data(), static_cast<qsizetype>(node->raw_path.size())};
    }
    if (role == Qt::DecorationRole) {
        return entry_icon(node->directory, node->raw_path);
    }
    return {};
}

Qt::ItemFlags LocalFolderTreeModel::flags(const QModelIndex& index) const {
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled
                           : Qt::NoItemFlags;
}

QStringList LocalFolderTreeModel::mimeTypes() const { return {LocalFilesMimeData::mimeType()}; }

Qt::DropActions LocalFolderTreeModel::supportedDragActions() const { return Qt::CopyAction; }

QMimeData* LocalFolderTreeModel::mimeData(const QModelIndexList& indexes) const {
    std::vector<std::string> paths;
    for (const auto& index : indexes) {
        if (index.column() != 0) {
            continue;
        }
        auto path = rawPath(index);
        if (!path.empty() && std::ranges::find(paths, path) == paths.end()) {
            paths.push_back(std::move(path));
        }
    }
    if (paths.empty()) {
        return nullptr;
    }
    // Nothing is walked while dragging: the paths are handed over as they
    // are, folders included, when the drop asks.
    return new LocalFilesMimeData{[paths = std::move(paths)](LocalFilesMimeData::Completion done) {
        done(paths);
    }};
}

bool LocalFolderTreeModel::hasChildren(const QModelIndex& parent_index) const {
    const auto* node = nodeFor(parent_index);
    return node == root_.get() ? !node->children.empty()
                               : node->directory && (!node->fetched || !node->children.empty());
}

bool LocalFolderTreeModel::isLoaded(const QModelIndex& index) const {
    if (!index.isValid()) {
        return true;
    }
    const auto* node = nodeFor(index);
    return !node->directory || node->fetched;
}

bool LocalFolderTreeModel::canFetchMore(const QModelIndex& parent_index) const {
    if (!parent_index.isValid()) {
        return false;
    }
    const auto* node = nodeFor(parent_index);
    return node->directory && !node->fetched && !node->fetching;
}

void LocalFolderTreeModel::fetchMore(const QModelIndex& parent_index) {
    if (!canFetchMore(parent_index)) {
        return;
    }
    auto* node = nodeFor(parent_index);
    node->fetching = true;
    const QPersistentModelIndex persistent_parent{parent_index};
    const auto raw_parent = node->raw_path;
    auto* watcher = new QFutureWatcher<DirectoryListing>(this);
    connect(watcher, &QFutureWatcher<DirectoryListing>::finished, this,
            [this, watcher, persistent_parent] {
                auto listing = watcher->result();
                watcher->deleteLater();
                if (!persistent_parent.isValid()) {
                    return;
                }
                auto* parent_node = nodeFor(persistent_parent);
                parent_node->fetching = false;
                parent_node->fetched = true;
                if (!listing.entries.empty()) {
                    beginInsertRows(persistent_parent, 0,
                                    static_cast<int>(listing.entries.size()) - 1);
                    parent_node->children.reserve(listing.entries.size());
                    for (auto& entry : listing.entries) {
                        auto child = std::make_unique<Node>();
                        child->parent = parent_node;
                        child->raw_path = std::move(entry.raw_path);
                        child->directory = entry.directory;
                        child->fetched = !entry.directory;
                        parent_node->children.push_back(std::move(child));
                    }
                    endInsertRows();
                }
                if (!listing.error.empty()) {
                    emit directoryError(QString::fromStdString(listing.error));
                }
                emit directoryLoaded(QModelIndex{persistent_parent});
            });
    watcher->setFuture(QtConcurrent::run([raw_parent] {
        DirectoryListing result;
        std::error_code error;
        std::filesystem::directory_iterator iterator{
            std::filesystem::path{raw_parent},
            std::filesystem::directory_options::skip_permission_denied, error};
        const std::filesystem::directory_iterator end;
        if (error) {
            result.error = error.message();
            return result;
        }
        while (iterator != end && result.entries.size() < children_per_directory_limit) {
            const auto child_path = iterator->path();
            if (hidden_entry(child_path)) {
                iterator.increment(error);
                error.clear();
                continue;
            }
            auto raw_child = child_path.native();
            // A music workstation's browser: directories and audio only.
            if (!std::filesystem::is_directory(iterator->symlink_status(error)) &&
                !audio_file_name(raw_child)) {
                iterator.increment(error);
                error.clear();
                continue;
            }
            error.clear();
            auto status = iterator->symlink_status(error);
            if (!error && std::filesystem::is_symlink(status)) {
                status = iterator->status(error);
                if (!error && std::filesystem::is_directory(status)) {
                    iterator.increment(error);
                    error.clear();
                    continue;
                }
            }
            if (!error && (std::filesystem::is_directory(status) ||
                           std::filesystem::is_regular_file(status))) {
                result.entries.push_back(DirectoryEntry{
                    .raw_path = std::move(raw_child),
                    .directory = std::filesystem::is_directory(status),
                });
            }
            error.clear();
            iterator.increment(error);
            if (error) {
                result.error = error.message();
                error.clear();
            }
        }
        if (result.entries.size() == children_per_directory_limit && iterator != end) {
            result.error = "local folder contains more than 10,000 direct entries";
        }
        // Directories first, each group in byte order.
        std::ranges::sort(result.entries, [](const auto& left, const auto& right) {
            if (left.directory != right.directory) {
                return left.directory;
            }
            return raw_less(left.raw_path, right.raw_path);
        });
        return result;
    }));
}

LocalFolderTreeModel::Node* LocalFolderTreeModel::nodeFor(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : root_.get();
}

int LocalFolderTreeModel::rowOf(const Node* node) const {
    const auto& siblings = node->parent->children;
    const auto found = std::ranges::find_if(
        siblings, [node](const auto& candidate) { return candidate.get() == node; });
    return static_cast<int>(std::distance(siblings.begin(), found));
}

} // namespace trackknife::ui
