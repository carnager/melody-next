// SPDX-License-Identifier: GPL-3.0-only

#include "ui/server_library_tree_model.hpp"

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <QApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QStyle>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace trackknife::ui {
namespace {

using titleformat::CompileOptions;
using titleformat::FormatContextKind;
using titleformat::Program;

[[nodiscard]] std::string utf8(const QString& value) {
    const auto bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] QString display(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] std::string normalizedFieldName(const std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const auto character : name) {
        if (character == '_' || character == '-' || character == ' ') {
            continue;
        }
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

[[nodiscard]] std::string numericPosition(const std::string_view value) {
    std::size_t length = 0U;
    while (length < value.size() && value[length] >= '0' && value[length] <= '9') {
        ++length;
    }
    if (length == 0U) {
        return {};
    }
    unsigned parsed = 0U;
    const auto [end, error] = std::from_chars(value.data(), value.data() + length, parsed);
    if (error != std::errc{} || end != value.data() + length) {
        return {};
    }
    return std::to_string(parsed);
}

class TrackContext final : public titleformat::EvaluationContext {
  public:
    explicit TrackContext(const mpd::Track& track) : track_(track) {}

    [[nodiscard]] FormatContextKind kind() const noexcept override {
        return FormatContextKind::tree_level;
    }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        auto values = resolveMetadata(name);
        if (!values) {
            return std::nullopt;
        }
        std::string joined;
        for (const auto& value : *values) {
            if (!joined.empty()) {
                joined += "; ";
            }
            joined += value;
        }
        return joined;
    }

    [[nodiscard]] std::optional<MetadataValues>
    resolveMetadata(const std::string_view name) const override {
        if (mpd::ascii_case_equal(name, "uri") || mpd::ascii_case_equal(name, "file")) {
            return MetadataValues{track_.uri};
        }
        const auto normalized = normalizedFieldName(name);
        if (normalized == "tracknumber" || normalized == "discnumber") {
            const auto source =
                track_.metadata.first(normalized == "tracknumber" ? "Track" : "Disc");
            if (!source) {
                return std::nullopt;
            }
            return MetadataValues{numericPosition(*source)};
        }
        MetadataValues copied;
        for (const auto& field : track_.metadata.fields()) {
            if (normalizedFieldName(field.name) == normalized) {
                copied.push_back(field.value);
            }
        }
        if (copied.empty()) {
            return std::nullopt;
        }
        return copied;
    }

  private:
    const mpd::Track& track_;
};

[[nodiscard]] std::optional<Program> compileTree(const QString& source, QString& error) {
    CompileOptions options;
    options.context = FormatContextKind::tree_level;
    auto output = titleformat::compile(utf8(source), options);
    if (!output.isValid()) {
        if (!output.parse_diagnostics.empty()) {
            error = display(output.parse_diagnostics.front().message);
        } else if (!output.diagnostics.empty()) {
            error = display(output.diagnostics.front().message);
        } else {
            error = QStringLiteral("Invalid tkfmt-1 expression");
        }
        return std::nullopt;
    }
    return std::move(output.program);
}

[[nodiscard]] QString evaluateTree(const Program& program, const mpd::Track& track) {
    const TrackContext context{track};
    if (!program.hasExpansions()) {
        auto result = titleformat::evaluate(program, context);
        return result ? display(result->text) : QString{};
    }
    auto results = titleformat::evaluateExpanded(program, context);
    return results && !results->empty() ? display(results->front().text) : QString{};
}

[[nodiscard]] QStringList evaluateTreeExpanded(const Program& program, const mpd::Track& track) {
    const TrackContext context{track};
    auto results = titleformat::evaluateExpanded(program, context);
    if (!results) {
        return {};
    }
    QStringList values;
    values.reserve(static_cast<qsizetype>(results->size()));
    for (const auto& result : *results) {
        values.push_back(display(result.text));
    }
    return values;
}

[[nodiscard]] QString normalizedSort(QString value) { return value.toCaseFolded(); }

[[nodiscard]] QIcon artistPlaceholderIcon() {
    static const QIcon icon = [] {
        QPixmap image{28, 28};
        image.fill(Qt::transparent);
        QPainter painter{&image};
        painter.setRenderHint(QPainter::Antialiasing);
        auto color = QApplication::palette().color(QPalette::Mid);
        color.setAlpha(210);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF{9.0, 4.0, 10.0, 10.0});
        painter.drawRoundedRect(QRectF{4.0, 16.0, 20.0, 10.0}, 5.0, 5.0);
        return QIcon{image};
    }();
    return icon;
}

[[nodiscard]] QString formatDuration(const std::chrono::milliseconds duration) {
    const auto total_seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    const auto hours = total_seconds / 3'600;
    const auto minutes = (total_seconds % 3'600) / 60;
    const auto seconds = total_seconds % 60;
    return hours > 0 ? QStringLiteral("%1:%2:%3")
                           .arg(hours)
                           .arg(minutes, 2, 10, QLatin1Char{'0'})
                           .arg(seconds, 2, 10, QLatin1Char{'0'})
                     : QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char{'0'});
}

[[nodiscard]] QString tracksSummary(const std::vector<mpd::Track>& tracks,
                                    const bool parenthesize_duration) {
    std::chrono::milliseconds duration{0};
    bool has_duration = false;
    for (const auto& track : tracks) {
        if (track.duration) {
            duration += *track.duration;
            has_duration = true;
        }
    }
    auto result = QStringLiteral("%1 Track%2")
                      .arg(tracks.size())
                      .arg(tracks.size() == 1U ? QString{} : QStringLiteral("s"));
    if (has_duration) {
        result += parenthesize_duration ? QStringLiteral(" (%1)").arg(formatDuration(duration))
                                        : QStringLiteral(" · %1").arg(formatDuration(duration));
    }
    return result;
}

} // namespace

LibraryTreeDefinition defaultLibraryTreeDefinition() {
    return LibraryTreeDefinition{
        .name = QStringLiteral("Album artist / chronological album / disc / tracks"),
        .root_tag = QStringLiteral("AlbumArtist"),
        .levels =
            {{.name = QStringLiteral("Album artist"),
              .grouping_expression = QStringLiteral("$if2(%albumartist%,%artist%,Unknown Artist)"),
              .label_expression = QStringLiteral("$if2(%albumartist%,%artist%,Unknown Artist)"),
              .sort_expression = QStringLiteral(
                  "$if2(%albumartistsort%,%artistsort%,$if2(%albumartist%,%artist%))")},
             {.name = QStringLiteral("Album"),
              .grouping_expression =
                  QStringLiteral("$if2(%musicbrainz_albumid%,$if2(%album%,Unknown Album)|%date%)"),
              .label_expression =
                  QStringLiteral("$if2(%album%,Unknown Album)$if(%date%, \\(%date%\\),)"),
              .sort_expression =
                  QStringLiteral("$if(%date%,0|%date%,1|)|$if2(%album%,Unknown Album)")},
             {.name = QStringLiteral("Disc"),
              .grouping_expression = QStringLiteral("$if2(%discnumber%,1)"),
              .label_expression = QStringLiteral("Disc $num($if2(%discnumber%,1),1)"),
              .sort_expression = QStringLiteral("$num($if2(%discnumber%,1),4)"),
              .omit_when_single = true},
             {.name = QStringLiteral("Tracks"),
              .grouping_expression = QStringLiteral("%uri%"),
              .label_expression = QStringLiteral("$num(%tracknumber%,2). $if2(%title%,%file%)"),
              .sort_expression = QStringLiteral("$num($if2(%discnumber%,1),4)|$num($if2(%"
                                                "tracknumber%,0),6)|$if2(%title%,%file%)")}},
    };
}

QByteArray serializeLibraryTreeDefinition(const LibraryTreeDefinition& definition) {
    QJsonArray levels;
    for (const auto& level : definition.levels) {
        levels.push_back(QJsonObject{{QStringLiteral("name"), level.name},
                                     {QStringLiteral("group"), level.grouping_expression},
                                     {QStringLiteral("label"), level.label_expression},
                                     {QStringLiteral("sort"), level.sort_expression},
                                     {QStringLiteral("omitWhenSingle"), level.omit_when_single}});
    }
    return QJsonDocument{QJsonObject{{QStringLiteral("schema"), 1},
                                     {QStringLiteral("dialect"), QStringLiteral("tkfmt-1")},
                                     {QStringLiteral("name"), definition.name},
                                     {QStringLiteral("rootTag"), definition.root_tag},
                                     {QStringLiteral("levels"), levels}}}
        .toJson(QJsonDocument::Compact);
}

std::optional<LibraryTreeDefinition> deserializeLibraryTreeDefinition(const QByteArray& bytes,
                                                                      QString* error) {
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(bytes, &parse_error);
    const auto fail = [error](const QString& message) -> std::optional<LibraryTreeDefinition> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("Invalid library-tree JSON"));
    }
    const auto object = document.object();
    if (object.value(QStringLiteral("schema")).toInt() != 1 ||
        object.value(QStringLiteral("dialect")).toString() != QStringLiteral("tkfmt-1")) {
        return fail(QStringLiteral("Unsupported library-tree definition version"));
    }
    LibraryTreeDefinition definition;
    definition.name = object.value(QStringLiteral("name")).toString();
    definition.root_tag = object.value(QStringLiteral("rootTag")).toString();
    for (const auto value : object.value(QStringLiteral("levels")).toArray()) {
        const auto level = value.toObject();
        definition.levels.push_back(
            {.name = level.value(QStringLiteral("name")).toString(),
             .grouping_expression = level.value(QStringLiteral("group")).toString(),
             .label_expression = level.value(QStringLiteral("label")).toString(),
             .sort_expression = level.value(QStringLiteral("sort")).toString(),
             .omit_when_single = level.value(QStringLiteral("omitWhenSingle")).toBool()});
    }
    if (definition.name.isEmpty() || definition.root_tag.isEmpty() ||
        definition.levels.size() < 2U) {
        return fail(QStringLiteral("Library-tree definition is incomplete"));
    }
    return definition;
}

struct ServerLibraryTreeModel::Impl {
    struct CompiledLevel {
        Program group;
        Program label;
        Program sort;
    };

    struct Node {
        Node* parent{nullptr};
        NodeKind kind{NodeKind::branch};
        std::size_t level{0U};
        QString label;
        QString sort_key;
        QString query_value;
        std::optional<std::size_t> album_count;
        bool album_count_failed{false};
        QString filter_text;
        bool loaded{true};
        bool loading{false};
        quint64 request_token{0U};
        std::vector<mpd::Track> tracks;
        std::vector<std::unique_ptr<Node>> children;
        QString artwork_uri;
        QIcon artwork;
        bool artwork_requested{false};
        quint64 artwork_token{0U};

        [[nodiscard]] int row() const {
            if (parent == nullptr) {
                return 0;
            }
            const auto found = std::ranges::find_if(
                parent->children, [this](const auto& child) { return child.get() == this; });
            return found == parent->children.end()
                       ? 0
                       : static_cast<int>(std::distance(parent->children.begin(), found));
        }
    };

    LibraryTreeDefinition definition{defaultLibraryTreeDefinition()};
    std::vector<CompiledLevel> compiled;
    Node root;
    quint64 generation{0U};
    QString active_root_tag;
    QHash<QString, int> newest_rank;
    bool newest_ordering{false};
    bool artwork_enabled{false};

    [[nodiscard]] bool isAlbumLevel(const std::size_t level) const {
        if (level >= definition.levels.size()) {
            return false;
        }
        const auto& configured = definition.levels[level];
        return configured.name.compare(QStringLiteral("Album"), Qt::CaseInsensitive) == 0 ||
               configured.grouping_expression.contains(QStringLiteral("%album%"),
                                                       Qt::CaseInsensitive);
    }

    [[nodiscard]] Node* node(const QModelIndex& index) {
        return index.isValid() ? static_cast<Node*>(index.internalPointer()) : &root;
    }
    [[nodiscard]] const Node* node(const QModelIndex& index) const {
        return index.isValid() ? static_cast<const Node*>(index.internalPointer()) : &root;
    }
};

ServerLibraryTreeModel::ServerLibraryTreeModel(QObject* parent)
    : QAbstractItemModel(parent), implementation_(std::make_unique<Impl>()) {
    const auto result = setDefinition(defaultLibraryTreeDefinition());
    Q_ASSERT(result.isEmpty());
}

ServerLibraryTreeModel::~ServerLibraryTreeModel() = default;

QModelIndex ServerLibraryTreeModel::index(const int row, const int column,
                                          const QModelIndex& parent_index) const {
    if (column < 0 || column >= columnCount() || row < 0) {
        return {};
    }
    const auto* parent_node = implementation_->node(parent_index);
    if (static_cast<std::size_t>(row) >= parent_node->children.size()) {
        return {};
    }
    return createIndex(row, column, parent_node->children[static_cast<std::size_t>(row)].get());
}

QModelIndex ServerLibraryTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    const auto* node = implementation_->node(child);
    if (node->parent == nullptr || node->parent == &implementation_->root) {
        return {};
    }
    return createIndex(node->parent->row(), 0, node->parent);
}

int ServerLibraryTreeModel::rowCount(const QModelIndex& parent_index) const {
    if (parent_index.column() > 0) {
        return 0;
    }
    return static_cast<int>(implementation_->node(parent_index)->children.size());
}

int ServerLibraryTreeModel::columnCount(const QModelIndex&) const { return 1; }

QVariant ServerLibraryTreeModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid()) {
        return {};
    }
    const auto* node = implementation_->node(index);
    const auto secondary = [this, node] {
        if (node->loading) {
            return QStringLiteral("Loading…");
        }
        if (node->kind == NodeKind::track) {
            if (!node->tracks.empty() && node->tracks.front().duration) {
                return formatDuration(*node->tracks.front().duration);
            }
            return QString{};
        }
        if (node->level == 0U) {
            if (node->album_count) {
                return QStringLiteral("%1 album%2")
                    .arg(*node->album_count)
                    .arg(*node->album_count == 1U ? QString{} : QStringLiteral("s"));
            }
            if (!node->loaded) {
                if (implementation_->active_root_tag == QStringLiteral("AlbumArtist") ||
                    implementation_->active_root_tag == QStringLiteral("Artist"))
                    return node->album_count_failed ? QStringLiteral("Album count unavailable")
                                                    : QStringLiteral("Counting albums…");
                return QStringLiteral("Expand to browse");
            }
            const auto child_count = node->children.size();
            return QStringLiteral("%1 album%2")
                .arg(child_count)
                .arg(child_count == 1U ? QString{} : QStringLiteral("s"));
        }
        return tracksSummary(node->tracks, implementation_->isAlbumLevel(node->level));
    }();
    switch (role) {
    case Qt::DisplayRole:
        return node->label;
    case Qt::DecorationRole:
        if (node->level == 0U && implementation_->active_root_tag.contains(QStringLiteral("artist"),
                                                                           Qt::CaseInsensitive)) {
            return QIcon::fromTheme(QStringLiteral("avatar-default"), artistPlaceholderIcon());
        }
        if (implementation_->isAlbumLevel(node->level)) {
            return node->artwork.isNull()
                       ? QIcon::fromTheme(QStringLiteral("media-optical-audio"),
                                          QApplication::style()->standardIcon(QStyle::SP_FileIcon))
                       : node->artwork;
        }
        if (node->kind == NodeKind::track) {
            return QIcon::fromTheme(QStringLiteral("audio-x-generic"),
                                    QApplication::style()->standardIcon(QStyle::SP_MediaVolume));
        }
        return {};
    case Qt::ToolTipRole:
        return node->kind == NodeKind::track
                   ? QStringLiteral("%1").arg(display(node->tracks.front().uri))
                   : QStringLiteral("%1 track%2")
                         .arg(node->tracks.size())
                         .arg(node->tracks.size() == 1U ? QString{} : QStringLiteral("s"));
    case KindRole:
        return static_cast<int>(node->kind);
    case TrackCountRole:
        return static_cast<qulonglong>(node->tracks.size());
    case LoadingRole:
        return node->loading;
    case LevelRole:
        return static_cast<qulonglong>(node->level);
    case AlbumRole:
        return implementation_->isAlbumLevel(node->level);
    case SecondaryTextRole:
        return secondary;
    case FilterTextRole:
        // Precomputed at build time: the filter re-reads this for every row on
        // every keystroke, so it must never allocate or scan metadata here.
        return node->filter_text.isEmpty() ? node->label : node->filter_text;
    case QueryValueRole:
        return node->query_value;
    case Qt::AccessibleTextRole:
        return secondary.isEmpty() ? node->label
                                   : QStringLiteral("%1, %2").arg(node->label, secondary);
    default:
        return {};
    }
}

Qt::ItemFlags ServerLibraryTreeModel::flags(const QModelIndex& index) const {
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled
                           : Qt::NoItemFlags;
}

bool ServerLibraryTreeModel::hasChildren(const QModelIndex& parent_index) const {
    if (!parent_index.isValid()) {
        return !implementation_->root.children.empty();
    }
    if (parent_index.column() > 0) {
        return false;
    }
    const auto* node = implementation_->node(parent_index);
    return node->kind == NodeKind::branch && (!node->loaded || !node->children.empty());
}

bool ServerLibraryTreeModel::canFetchMore(const QModelIndex& parent_index) const {
    const auto* node = implementation_->node(parent_index);
    return parent_index.isValid() && node->kind == NodeKind::branch && !node->loaded &&
           !node->loading;
}

void ServerLibraryTreeModel::fetchMore(const QModelIndex& parent_index) {
    auto* node = implementation_->node(parent_index);
    if (!canFetchMore(parent_index)) {
        return;
    }
    node->loading = true;
    node->request_token = ++implementation_->generation;
    emit dataChanged(parent_index, parent_index,
                     {Qt::DisplayRole, LoadingRole, SecondaryTextRole, FilterTextRole});
    emit branchRequested(node->request_token, implementation_->active_root_tag, node->query_value);
}

const LibraryTreeDefinition& ServerLibraryTreeModel::definition() const noexcept {
    return implementation_->definition;
}

QString ServerLibraryTreeModel::activeRootTag() const {
    return implementation_->active_root_tag.isEmpty() ? implementation_->definition.root_tag
                                                      : implementation_->active_root_tag;
}

QString ServerLibraryTreeModel::setDefinition(LibraryTreeDefinition definition) {
    if (definition.root_tag.trimmed().isEmpty() || definition.levels.size() < 2U) {
        return QStringLiteral("A tree needs a root tag and at least one branch plus tracks");
    }
    std::vector<Impl::CompiledLevel> compiled;
    compiled.reserve(definition.levels.size());
    for (const auto& level : definition.levels) {
        QString error;
        auto group = compileTree(level.grouping_expression, error);
        if (!group) {
            return QStringLiteral("%1 grouping: %2").arg(level.name, error);
        }
        auto label = compileTree(level.label_expression, error);
        if (!label) {
            return QStringLiteral("%1 label: %2").arg(level.name, error);
        }
        auto sort = compileTree(level.sort_expression, error);
        if (!sort) {
            return QStringLiteral("%1 sort: %2").arg(level.name, error);
        }
        compiled.push_back(
            {.group = std::move(*group), .label = std::move(*label), .sort = std::move(*sort)});
    }
    beginResetModel();
    implementation_->definition = std::move(definition);
    implementation_->compiled = std::move(compiled);
    implementation_->root.children.clear();
    implementation_->root.loaded = true;
    implementation_->active_root_tag.clear();
    ++implementation_->generation;
    endResetModel();
    return {};
}

std::vector<mpd::Track> ServerLibraryTreeModel::tracks(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {};
    }
    const auto* node = implementation_->node(index);
    if (node->children.empty()) {
        return node->tracks;
    }
    std::vector<mpd::Track> ordered;
    std::unordered_map<std::string, bool> seen;
    const auto collect = [&ordered, &seen](auto&& self, const Impl::Node* current) -> void {
        if (current->children.empty()) {
            for (const auto& track : current->tracks) {
                if (seen.emplace(track.uri, true).second) {
                    ordered.push_back(track);
                }
            }
            return;
        }
        for (const auto& child : current->children) {
            self(self, child.get());
        }
    };
    collect(collect, node);
    return ordered;
}

void ServerLibraryTreeModel::setArtworkEnabled(const bool enabled) {
    if (implementation_->artwork_enabled == enabled) {
        return;
    }
    implementation_->artwork_enabled = enabled;
    if (enabled) {
        requestNextArtwork();
    }
}

void ServerLibraryTreeModel::reload() {
    beginResetModel();
    implementation_->root.children.clear();
    implementation_->active_root_tag.clear();
    const auto token = ++implementation_->generation;
    implementation_->root.request_token = token;
    endResetModel();
    emit rootRequested(token, implementation_->definition.root_tag);
}

void ServerLibraryTreeModel::acceptRoot(const quint64 token, const QString& tag,
                                        const QStringList& values, const QString& error) {
    if (token != implementation_->root.request_token) {
        return;
    }
    if (!error.isEmpty()) {
        emit browseError(error);
        return;
    }
    implementation_->active_root_tag = tag;
    std::vector<std::unique_ptr<Impl::Node>> children;
    children.reserve(static_cast<std::size_t>(values.size()));
    for (const auto& value : values) {
        mpd::Track synthetic;
        synthetic.metadata = mpd::Metadata{{mpd::Pair{.name = utf8(tag), .value = utf8(value)}}};
        auto child = std::make_unique<Impl::Node>();
        child->parent = &implementation_->root;
        child->level = 0U;
        child->label = evaluateTree(implementation_->compiled.front().label, synthetic);
        if (child->label.isEmpty()) {
            child->label = value;
        }
        child->sort_key =
            normalizedSort(evaluateTree(implementation_->compiled.front().sort, synthetic));
        child->query_value = value;
        child->filter_text = child->label;
        child->loaded = false;
        children.push_back(std::move(child));
    }
    std::ranges::stable_sort(children, {}, [](const auto& child) { return child->sort_key; });
    if (implementation_->newest_ordering) {
        std::ranges::stable_sort(children, {}, [this](const auto& child) {
            return implementation_->newest_rank.value(child->query_value,
                                                      std::numeric_limits<int>::max());
        });
    }
    beginResetModel();
    implementation_->root.children = std::move(children);
    endResetModel();
}

void ServerLibraryTreeModel::acceptAlbumCounts(const quint64 token,
                                               const std::vector<mpd::ArtistAlbumCount>& counts,
                                               const QString& error) {
    if (token != implementation_->root.request_token)
        return;
    QHash<QString, std::size_t> by_artist;
    if (error.isEmpty()) {
        for (const auto& count : counts)
            by_artist.insert(display(count.artist), count.albums);
    }
    for (const auto& node : implementation_->root.children) {
        const auto found = by_artist.constFind(node->query_value);
        node->album_count_failed = !error.isEmpty() || found == by_artist.cend();
        node->album_count =
            node->album_count_failed ? std::nullopt : std::optional<std::size_t>{*found};
    }
    if (rowCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {SecondaryTextRole});
}

void ServerLibraryTreeModel::setRootOrdering(const QStringList& newest_first) {
    implementation_->newest_rank.clear();
    for (qsizetype rank = 0; rank < newest_first.size(); ++rank) {
        implementation_->newest_rank.insert(newest_first[rank], static_cast<int>(rank));
    }
    implementation_->newest_ordering = true;
    beginResetModel();
    std::ranges::stable_sort(implementation_->root.children, {},
                             [](const auto& child) { return child->sort_key; });
    std::ranges::stable_sort(implementation_->root.children, {}, [this](const auto& child) {
        return implementation_->newest_rank.value(child->query_value,
                                                  std::numeric_limits<int>::max());
    });
    endResetModel();
}

void ServerLibraryTreeModel::clearRootOrdering() {
    if (!implementation_->newest_ordering) {
        return;
    }
    implementation_->newest_ordering = false;
    implementation_->newest_rank.clear();
    beginResetModel();
    std::ranges::stable_sort(implementation_->root.children, {},
                             [](const auto& child) { return child->sort_key; });
    endResetModel();
}

void ServerLibraryTreeModel::acceptBranch(const quint64 token,
                                          const std::vector<mpd::Track>& tracks,
                                          const QString& error) {
    Impl::Node* branch = nullptr;
    for (auto& child : implementation_->root.children) {
        if (child->request_token == token) {
            branch = child.get();
            break;
        }
    }
    if (branch == nullptr) {
        return;
    }
    const auto branch_index = createIndex(branch->row(), 0, branch);
    branch->loading = false;
    if (!error.isEmpty()) {
        emit dataChanged(branch_index, branch_index,
                         {Qt::DisplayRole, LoadingRole, SecondaryTextRole, FilterTextRole});
        emit browseError(error);
        return;
    }
    branch->loaded = true;
    branch->tracks = tracks;

    const auto build = [this](auto&& self, Impl::Node* parent, const std::size_t level,
                              const std::vector<mpd::Track>& source) -> void {
        if (level >= implementation_->compiled.size()) {
            return;
        }
        const auto& definition = implementation_->definition.levels[level];
        const auto& compiled = implementation_->compiled[level];
        const bool track_level = level + 1U == implementation_->compiled.size();
        std::unordered_map<std::string, std::size_t> positions;
        std::vector<std::unique_ptr<Impl::Node>> children;
        for (const auto& track : source) {
            auto keys = evaluateTreeExpanded(compiled.group, track);
            const auto labels = evaluateTreeExpanded(compiled.label, track);
            const auto sorts = evaluateTreeExpanded(compiled.sort, track);
            if (keys.isEmpty()) {
                keys.push_back(display(track.uri));
            }
            for (qsizetype expansion = 0; expansion < keys.size(); ++expansion) {
                auto key = utf8(keys[expansion]);
                if (key.empty()) {
                    key = track.uri;
                }
                const auto [position, inserted] = positions.emplace(key, children.size());
                if (inserted) {
                    const auto expandedValue = [expansion](const QStringList& values) {
                        return values.isEmpty() ? QString{}
                                                : values[std::min(expansion, values.size() - 1)];
                    };
                    auto child = std::make_unique<Impl::Node>();
                    child->parent = parent;
                    child->kind = track_level ? NodeKind::track : NodeKind::branch;
                    child->level = level;
                    child->label = expandedValue(labels);
                    child->sort_key = normalizedSort(expandedValue(sorts));
                    child->loaded = true;
                    // Branch rows match on their visible label only — hint
                    // text must never satisfy a query. Track rows also match
                    // on their descriptive tags so a query naming an artist,
                    // album, or genre keeps exactly the rows that belong to
                    // it. Built once here; the filter reads it per keystroke.
                    child->filter_text = child->label;
                    if (track_level) {
                        static constexpr std::array searchable_tags{
                            "Artist", "AlbumArtist", "Album",    "Date",
                            "Genre",  "Composer",    "Performer"};
                        for (const auto* tag : searchable_tags) {
                            for (const auto value : track.metadata.values(tag)) {
                                child->filter_text += QLatin1Char{' '};
                                child->filter_text += display(value);
                            }
                        }
                    }
                    child->tracks.push_back(track);
                    if (implementation_->isAlbumLevel(level)) {
                        child->artwork_uri = display(track.uri);
                    }
                    children.push_back(std::move(child));
                } else {
                    children[position->second]->tracks.push_back(track);
                }
            }
        }
        std::ranges::stable_sort(children, {}, [](const auto& child) { return child->sort_key; });
        if (definition.omit_when_single && children.size() == 1U && !track_level) {
            self(self, parent, level + 1U, source);
            return;
        }
        parent->children = std::move(children);
        if (!track_level) {
            for (auto& child : parent->children) {
                self(self, child.get(), level + 1U, child->tracks);
            }
        }
    };
    Impl::Node staging;
    staging.tracks = branch->tracks;
    build(build, &staging, 1U, staging.tracks);
    const auto child_count = static_cast<int>(staging.children.size());
    if (child_count > 0) {
        beginInsertRows(branch_index, 0, child_count - 1);
        branch->children = std::move(staging.children);
        for (auto& child : branch->children) {
            child->parent = branch;
        }
        endInsertRows();
    }
    emit dataChanged(
        branch_index, branch_index,
        {Qt::DisplayRole, TrackCountRole, LoadingRole, SecondaryTextRole, FilterTextRole});
    requestNextArtwork();
}

void ServerLibraryTreeModel::acceptArtwork(const quint64 token, const QImage& image) {
    Impl::Node* found = nullptr;
    const auto locate = [&found, token](auto&& self, Impl::Node* node) -> void {
        if (node->artwork_token == token) {
            found = node;
            return;
        }
        for (auto& child : node->children) {
            self(self, child.get());
            if (found != nullptr) {
                return;
            }
        }
    };
    locate(locate, &implementation_->root);
    if (found == nullptr) {
        return;
    }
    if (!image.isNull()) {
        found->artwork = QIcon{QPixmap::fromImage(image)};
        const auto index = createIndex(found->row(), 0, found);
        emit dataChanged(index, index, {Qt::DecorationRole});
    }
    requestNextArtwork();
}

void ServerLibraryTreeModel::requestNextArtwork() {
    if (!implementation_->artwork_enabled) {
        return;
    }
    Impl::Node* found = nullptr;
    const auto locate = [this, &found](auto&& self, Impl::Node* node) -> void {
        if (implementation_->isAlbumLevel(node->level) && !node->artwork_requested &&
            !node->artwork_uri.isEmpty()) {
            found = node;
            return;
        }
        for (auto& child : node->children) {
            self(self, child.get());
            if (found != nullptr) {
                return;
            }
        }
    };
    locate(locate, &implementation_->root);
    if (found == nullptr) {
        return;
    }
    found->artwork_requested = true;
    found->artwork_token = ++implementation_->generation;
    emit artworkRequested(found->artwork_token, found->artwork_uri);
}

ServerLibraryFilterModel::ServerLibraryFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent) {
    setFilterKeyColumn(0);
    setFilterRole(ServerLibraryTreeModel::FilterTextRole);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    // Strict filtering: every visible row matches itself or has a matching
    // descendant. A directly matched row never drags its whole subtree in —
    // track rows carry their descriptive tags in FilterTextRole instead, so an
    // artist or album query keeps exactly the rows that belong to it.
    setRecursiveFilteringEnabled(true);
    setAutoAcceptChildRows(false);
}

void ServerLibraryFilterModel::setServerMatches(const QStringList& root_values) {
    QSet<QString> matches{root_values.begin(), root_values.end()};
    if (matches == server_matches_) {
        return;
    }
    beginFilterChange();
    server_matches_ = std::move(matches);
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void ServerLibraryFilterModel::clearServerMatches() { setServerMatches({}); }

bool ServerLibraryFilterModel::filterAcceptsRow(const int source_row,
                                                const QModelIndex& source_parent) const {
    if (QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent)) {
        return true;
    }
    // A bounded server-side search can prove that an unexpanded (or locally
    // unmatched) root contains matching descendants the client has not loaded.
    if (server_matches_.isEmpty() || source_parent.isValid() || sourceModel() == nullptr) {
        return false;
    }
    const auto index = sourceModel()->index(source_row, 0, source_parent);
    return server_matches_.contains(index.data(ServerLibraryTreeModel::QueryValueRole).toString());
}

} // namespace trackknife::ui
