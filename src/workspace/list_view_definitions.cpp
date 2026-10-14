// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/list_view_definitions.hpp"

#include "trackknife/core/stable_id.hpp"

#include <QHash>
#include <QSettings>

#include <algorithm>

namespace trackknife::bench {
namespace {

const QString settings_key = QStringLiteral("lists/views");

[[nodiscard]] core::Error invalid(const std::string& message) {
    return {.code = core::ErrorCode::invalid_argument, .message = message, .context = {}};
}

struct CompiledEntry {
    lists::ListView view;
    std::shared_ptr<const lists::CompiledListView> compiled;
};

[[nodiscard]] QHash<QString, CompiledEntry>& compiled_cache() {
    static QHash<QString, CompiledEntry> cache;
    return cache;
}

} // namespace

core::Result<std::vector<lists::ListView>> customListViews() {
    const auto bytes = QSettings{}.value(settings_key).toByteArray();
    if (bytes.isEmpty()) {
        return std::vector<lists::ListView>{};
    }
    return lists::deserialize_list_views(
        std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())});
}

std::vector<lists::ListView> listViews() {
    auto views = lists::shipped_list_views();
    if (auto custom = customListViews()) {
        std::ranges::move(*custom, std::back_inserter(views));
    }
    return views;
}

core::Result<void> saveCustomListViews(const std::vector<lists::ListView>& views) {
    if (const auto saved = customListViews(); !saved) {
        return std::unexpected(invalid("The saved list views cannot be read, so they are not "
                                       "written over: " +
                                       saved.error().message));
    }
    std::vector<lists::ListView> custom;
    for (const auto& view : views) {
        if (view.builtin) {
            continue;
        }
        if (const auto problems = lists::validate_list_view(view); !problems.empty()) {
            return std::unexpected(invalid(problems.front().message));
        }
        custom.push_back(view);
    }
    const auto text = lists::serialize_list_views(custom);
    QSettings{}.setValue(settings_key,
                         QByteArray{text.data(), static_cast<qsizetype>(text.size())});
    emit ListViewCatalog::instance().changed();
    return {};
}

std::optional<lists::ListView> findListView(const QString& id) {
    const auto text = id.toStdString();
    if (const auto* shipped = lists::shipped_list_view(text)) {
        return *shipped;
    }
    if (auto custom = customListViews()) {
        if (const auto found = std::ranges::find(*custom, text, &lists::ListView::id);
            found != custom->end()) {
            return *found;
        }
    }
    return std::nullopt;
}

std::shared_ptr<const lists::CompiledListView> compiledListView(const QString& id) {
    auto view = findListView(id);
    if (!view) {
        return nullptr;
    }
    auto& cache = compiled_cache();
    if (const auto found = cache.constFind(id); found != cache.constEnd() && found->view == *view) {
        return found->compiled;
    }
    auto compiled = lists::CompiledListView::compile(*view);
    if (!compiled) {
        return nullptr;
    }
    auto shared = std::make_shared<const lists::CompiledListView>(std::move(*compiled));
    cache.insert(id, {.view = std::move(*view), .compiled = shared});
    return shared;
}

QString newListViewId() {
    return QStringLiteral("view-") + QString::fromStdString(core::StableId::random().to_string());
}

core::Result<QString> listViewCopy(const lists::ListView& from,
                                   std::vector<lists::ViewColumn> arranged, const QString& name) {
    auto custom = customListViews();
    if (!custom) {
        return std::unexpected(std::move(custom.error()));
    }
    auto copy = from;
    copy.builtin = false;
    copy.name = name.toStdString();
    copy.columns = std::move(arranged);
    for (const auto& existing : *custom) {
        auto same = existing;
        same.id = copy.id;
        if (same == copy) {
            return QString::fromStdString(existing.id);
        }
    }
    copy.id = newListViewId().toStdString();
    custom->push_back(copy);
    if (auto saved = saveCustomListViews(*custom); !saved) {
        return std::unexpected(std::move(saved.error()));
    }
    return QString::fromStdString(copy.id);
}

ListViewCatalog& ListViewCatalog::instance() {
    static ListViewCatalog catalog;
    return catalog;
}

} // namespace trackknife::bench
