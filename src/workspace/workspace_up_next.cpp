// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "workspace/workspace_view.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <numeric>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {
QJsonArray continuationIdentity(const LocalTrackRow& row) {
    const auto number = [](const auto& value) {
        return value ? QString::number(*value) : QStringLiteral("none");
    };
    return {QString::fromLatin1(QByteArray::fromStdString(row.raw_path).toBase64()),
            number(row.selection.stream_index), number(row.selection.subsong_index),
            row.segment ? QString::number(row.segment->start_sample) : QStringLiteral("none"),
            row.segment ? number(row.segment->end_sample) : QStringLiteral("none")};
}
} // namespace

void Workspace::enqueueLocalRequests(std::vector<LocalTrackRow> rows, int position,
                                           const EngineKey& engine) {
    const auto count = rows.size();
    // ADR-0227: an ask is a file on one engine's machine, and a queue cannot
    // play files from two. Up Next is one engine's until it is empty again.
    const bool holding =
        !playback_.requests.pending().empty() || playback_.requests.active().has_value();
    if (holding && engine != up_next_engine_) {
        view_->showMessage(QStringLiteral("Up Next holds tracks from %1; finish or clear it "
                                                "before adding tracks from %2")
                                     .arg(engineName(up_next_engine_), engineName(engine)),
                                 6000);
        return;
    }
    if (!holding) {
        up_next_engine_ = engine;
        engine_requests_.clear();
    }
    // Each ask is an occurrence of its own (ADR-0221): the same track asked
    // for twice plays twice, and playing it does not move the list, whose row
    // it was copied from. The engine holds these apart from the list; a copied
    // identity made both asks one entry, and the second never played.
    for (auto& row : rows) {
        row.entry_id = core::StableId::random();
    }
    if (!playback_.requests.insert(std::move(rows), position < 0
                                                        ? playback_.requests.pending().size()
                                                        : static_cast<std::size_t>(position))) {
        view_->showMessage(QStringLiteral("Up Next holds at most 500 tracks."), 5000);
        return;
    }
    persistUpNext();
    view_->refreshUpNext();
    view_->showMessage(QStringLiteral("Added %1 to Up Next").arg(count), 3000);
}


// operation: remove, move up, move down, or drag to an insertion boundary.
void Workspace::editUpNextRows(std::vector<bool> selected, int operation, int destination) {
    const auto count = static_cast<int>(up_next_display_ids_.size());
    selected.resize(static_cast<std::size_t>(count), false);
    if (std::ranges::find(selected, true) == selected.end())
        return;
    std::vector<int> order(static_cast<std::size_t>(count));
    std::iota(order.begin(), order.end(), 0);
    if (operation == 1) {
        std::erase_if(order, [&](int row) { return selected[static_cast<std::size_t>(row)]; });
    } else if (operation == 2) {
        if (selected.front())
            return;
        for (int i = 1; i < count; ++i)
            if (selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])] &&
                !selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i - 1)])])
                std::swap(order[static_cast<std::size_t>(i)],
                          order[static_cast<std::size_t>(i - 1)]);
    } else if (operation == 3) {
        if (selected.back())
            return;
        for (int i = count - 2; i >= 0; --i)
            if (selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])] &&
                !selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i + 1)])])
                std::swap(order[static_cast<std::size_t>(i)],
                          order[static_cast<std::size_t>(i + 1)]);
    } else {
        destination = std::clamp(destination, 0, count);
        std::vector<int> moving;
        int before = 0;
        for (int i = 0; i < count; ++i)
            if (selected[static_cast<std::size_t>(i)]) {
                moving.push_back(i);
                if (i < destination)
                    ++before;
            }
        std::erase_if(order, [&](int row) { return selected[static_cast<std::size_t>(row)]; });
        order.insert(order.begin() + destination - before, moving.begin(), moving.end());
    }
    std::vector<std::uint64_t> ids;
    for (const auto row : order)
        ids.push_back(up_next_display_ids_[static_cast<std::size_t>(row)]);
    if (ids == up_next_display_ids_)
        return;
    if (playback_.requests.retain(ids)) {
        persistUpNext();
        view_->refreshUpNext();
    }
}


void Workspace::editUpNext(int operation, int row, int destination) {
    if (operation == 0)
        playback_.requests.clear();
    else {
        if (row < 0 || row >= static_cast<int>(playback_.requests.pending().size()))
            return;
        auto id = playback_.requests.pending()[static_cast<std::size_t>(row)].id;
        if (operation == 1)
            playback_.requests.remove(id);
        else {
            if (destination < 0)
                return;
            playback_.requests.move(id, static_cast<std::size_t>(destination));
        }
    }
    persistUpNext();
    view_->refreshUpNext();
}


void Workspace::persistUpNext() {
    if (!persistence_ || !up_next_restored_)
        return;
    QJsonArray rows;
    const auto append = [&](const LocalTrackRow& row) {
        QJsonObject item{{QStringLiteral("path"),
                          QString::fromLatin1(QByteArray::fromStdString(row.raw_path).toBase64())},
                         {QStringLiteral("title"), QString::fromStdString(row.title)},
                         {QStringLiteral("artist"), QString::fromStdString(row.artist)},
                         // The identity the engine holds it by: kept, so after a
                         // restart both still name the same entry.
                         {QStringLiteral("entry"), QString::fromStdString(row.entry_id.to_string())}};
        if (row.selection.stream_index)
            item[QStringLiteral("stream")] = *row.selection.stream_index;
        if (row.selection.subsong_index)
            item[QStringLiteral("subsong")] = static_cast<int>(*row.selection.subsong_index);
        if (row.segment) {
            item[QStringLiteral("start")] = QString::number(row.segment->start_sample);
            if (row.segment->end_sample)
                item[QStringLiteral("end")] = QString::number(*row.segment->end_sample);
        }
        item[QStringLiteral("album")] = QString::fromStdString(row.album);
        item[QStringLiteral("albumArtist")] = QString::fromStdString(row.album_artist);
        item[QStringLiteral("date")] = QString::fromStdString(row.date);
        if (row.logical_reference)
            item[QStringLiteral("logical")] = QString::fromStdString(*row.logical_reference);
        if (row.duration_ms)
            item[QStringLiteral("duration")] = QString::number(*row.duration_ms);
        QJsonArray fields;
        for (const auto& field : row.metadata.fields) {
            QJsonArray values;
            for (const auto& value : field.values)
                values.push_back(QString::fromStdString(value));
            QJsonObject fieldJson{
                {QStringLiteral("name"), QString::fromStdString(field.canonical_name)},
                {QStringLiteral("native"), QString::fromStdString(field.native_name)},
                {QStringLiteral("values"), values},
                {QStringLiteral("provenance"), static_cast<int>(field.provenance)}};
            if (field.qualifier.language)
                fieldJson[QStringLiteral("language")] =
                    QString::fromStdString(*field.qualifier.language);
            if (field.qualifier.description)
                fieldJson[QStringLiteral("description")] =
                    QString::fromStdString(*field.qualifier.description);
            fields.push_back(fieldJson);
        }
        item[QStringLiteral("fields")] = fields;
        rows.push_back(item);
    };
    if (playback_.requests.active())
        append(playback_.requests.active()->source);
    for (const auto& entry : playback_.requests.pending())
        append(entry.source);
    QJsonObject state{
        {QStringLiteral("version"), 1},
        {QStringLiteral("rows"), rows},
        // Whose files these are: Up Next holds one engine's asks.
        {QStringLiteral("engine"), up_next_engine_.text()},
        {QStringLiteral("document"), document_text(playback_.anchors.document)},
        {QStringLiteral("row"), resolvePlaybackRow(tabForDocument(playback_.anchors.document))}};
    state[QStringLiteral("anchor")] = QString::fromLatin1(
        QByteArray::fromStdString(playback_.anchors.source.raw_path).toBase64());
    if (!playback_.anchors.request_return.is_nil()) {
        if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            if (const auto row = tab->model->rowOfEntry(playback_.anchors.request_return, -1);
                row >= 0) {
                state[QStringLiteral("returnRow")] = row;
                state[QStringLiteral("returnSource")] =
                    continuationIdentity(tab->model->rows().at(static_cast<std::size_t>(row)));
            }
        }
    }
    persistence_->saveUiState(
        QStringLiteral("playback/up-next/v1"), QJsonDocument(state).toJson(QJsonDocument::Compact),
        [this](QString error) {
            if (!error.isEmpty())
                view_->showMessage(
                    QStringLiteral("Up Next could not be saved: %1").arg(error), 8000);
        });
}


void Workspace::restoreUpNext() {
    if (!persistence_)
        return;
    const auto request_revision = playback_.requests.revision();
    persistence_->loadUiState(QStringLiteral("playback/up-next/v1"), [this, request_revision](
                                                                         QByteArray payload,
                                                                         QString error) {
        if (!error.isEmpty()) {
            view_->showMessage(error, 5000);
            return;
        }
        if (!payload.isEmpty()) {
            const auto document = QJsonDocument::fromJson(payload);
            const auto state = document.object();
            if (state.value(QStringLiteral("version")).toInt() != 1) {
                view_->showMessage(
                    QStringLiteral("Unsupported Up Next state; it has been preserved."), 8000);
                return;
            }
            std::vector<LocalTrackRow> rows;
            if (state.value(QStringLiteral("rows")).toArray().size() >
                static_cast<qsizetype>(audio::RequestQueue<LocalTrackRow>::limit + 1)) {
                view_->showMessage(
                    tr("Saved Up Next exceeds the queue limit; it has been preserved."), 8000);
                return;
            }
            for (const auto& value : state.value(QStringLiteral("rows")).toArray()) {
                const auto item = value.toObject();
                LocalTrackRow row;
                row.raw_path =
                    QByteArray::fromBase64(item.value(QStringLiteral("path")).toString().toLatin1())
                        .toStdString();
                if (row.raw_path.empty())
                    continue;
                row.title = item.value(QStringLiteral("title")).toString().toStdString();
                row.artist = item.value(QStringLiteral("artist")).toString().toStdString();
                // Saved before identities were: a new one, as then.
                if (const auto identity = core::StableId::parse(
                        item.value(QStringLiteral("entry")).toString().toStdString())) {
                    row.entry_id = *identity;
                }
                if (item.contains(QStringLiteral("stream")))
                    row.selection.stream_index = item.value(QStringLiteral("stream")).toInt();
                if (item.contains(QStringLiteral("subsong")))
                    row.selection.subsong_index = item.value(QStringLiteral("subsong")).toInt();
                if (item.contains(QStringLiteral("start")))
                    row.segment = formats::SampleRange{
                        item.value(QStringLiteral("start")).toString().toLongLong(),
                        item.contains(QStringLiteral("end"))
                            ? std::optional<std::int64_t>{item.value(QStringLiteral("end"))
                                                              .toString()
                                                              .toLongLong()}
                            : std::nullopt};
                row.album = item.value(QStringLiteral("album")).toString().toStdString();
                row.album_artist =
                    item.value(QStringLiteral("albumArtist")).toString().toStdString();
                row.date = item.value(QStringLiteral("date")).toString().toStdString();
                if (item.contains(QStringLiteral("logical")))
                    row.logical_reference =
                        item.value(QStringLiteral("logical")).toString().toStdString();
                if (item.contains(QStringLiteral("duration")))
                    row.duration_ms =
                        item.value(QStringLiteral("duration")).toString().toLongLong();
                for (const auto& fieldValue : item.value(QStringLiteral("fields")).toArray()) {
                    const auto fieldJson = fieldValue.toObject();
                    metadata::MetadataField field;
                    field.canonical_name =
                        fieldJson.value(QStringLiteral("name")).toString().toStdString();
                    field.native_name =
                        fieldJson.value(QStringLiteral("native")).toString().toStdString();
                    const auto provenance = fieldJson.value(QStringLiteral("provenance")).toInt();
                    if (provenance < 0 ||
                        provenance > static_cast<int>(metadata::FieldProvenance::sidecar))
                        continue;
                    field.provenance = static_cast<metadata::FieldProvenance>(provenance);
                    for (const auto& valueText :
                         fieldJson.value(QStringLiteral("values")).toArray())
                        field.values.push_back(valueText.toString().toStdString());
                    if (fieldJson.contains(QStringLiteral("language")))
                        field.qualifier.language =
                            fieldJson.value(QStringLiteral("language")).toString().toStdString();
                    if (fieldJson.contains(QStringLiteral("description")))
                        field.qualifier.description =
                            fieldJson.value(QStringLiteral("description")).toString().toStdString();
                    row.metadata.fields.push_back(std::move(field));
                }
                rows.push_back(std::move(row));
            }
            const bool untouched = request_revision == playback_.requests.revision() &&
                                   playback_.requests.pending().empty() &&
                                   !playback_.requests.active();
            if (untouched) {
                // Saved before the engine was: whose the tab it returns to is.
                // An older release said only whether it was the remote's.
                if (state.contains(QStringLiteral("engine"))) {
                    up_next_engine_ =
                        EngineKey::fromText(state.value(QStringLiteral("engine")).toString());
                } else if (state.contains(QStringLiteral("remote"))) {
                    up_next_engine_ = state.value(QStringLiteral("remote")).toBool()
                                          ? EngineKey::remote()
                                          : EngineKey::local();
                } else if (const auto* returns_to =
                               tabForDocument(state.value(QStringLiteral("document")).toString())) {
                    up_next_engine_ = EngineKey::of(returns_to->document);
                }
                if (!playback_.requests.insert(std::move(rows), 0)) {
                    view_->showMessage(
                        tr("Saved Up Next exceeds the pending queue limit; it has been preserved."),
                        8000);
                    return;
                }
            }
            if (playback_.anchors.document.is_nil() && !playback_.requests.pending().empty()) {
                const auto id = state.value(QStringLiteral("document")).toString();
                const auto anchor = QByteArray::fromBase64(
                                        state.value(QStringLiteral("anchor")).toString().toLatin1())
                                        .toStdString();
                if (auto* tab = tabForDocument(id)) {
                    const auto row = state.value(QStringLiteral("row")).toInt(-1);
                    if (row >= 0 && row < tab->model->rowCount() &&
                        tab->model->rawPath(row) == anchor) {
                        playback_.anchors.document = document_identity(id);
                        playback_.anchors.current =
                            tab->model->rows().at(static_cast<std::size_t>(row)).entry_id;
                        playback_.row = row;
                        playback_.anchors.source = tab->model->source(row);
                    }
                }
            }
        }
        playback_.requests.forgetUndo();
        up_next_restored_ = true;
        view_->refreshUpNext();
    });
}

bool Workspace::syncUpNextModel() {
    bool replaced = false;
    if (up_next_local_revision_ != playback_.requests.revision()) {
        std::vector<LocalTrackRow> rows;
        for (const auto& entry : playback_.requests.pending())
            rows.push_back(entry.source);
        up_next_local_model_->replaceRows(std::move(rows));
        replaced = true;
        up_next_display_ids_.clear();
        for (const auto& entry : playback_.requests.pending())
            up_next_display_ids_.push_back(entry.id);
        up_next_local_revision_ = playback_.requests.revision();
    }
    // Covers as the lists have them, or fetched when no list does.
    for (int row = 0; row < up_next_local_model_->rowCount(); ++row) {
        const auto key = up_next_local_model_->groupKey(row);
        if (up_next_local_model_->hasArtwork(key)) {
            continue;
        }
        if (const auto cover = coverFor(
                up_next_local_model_->rows()[static_cast<std::size_t>(row)], up_next_engine_);
            !cover.isNull()) {
            up_next_local_model_->setArtwork(key, cover);
        }
    }
    // The engine decides what plays next, so it has to be told. Guarded on
    // the order actually changing, because this runs on every refresh.
    syncEngineRequests();
    return replaced;
}

} // namespace trackknife::bench
