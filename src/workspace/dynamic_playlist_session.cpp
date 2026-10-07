// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/dynamic_playlist_session.hpp"

#include "bench/local_list_model.hpp"

#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace trackknife::bench {

DynamicPlaylistSession::DynamicPlaylistSession(QString profile, std::vector<Library> libraries,
                                               LibrarySearch search, QObject* parent)
    : QObject(parent), profile_(std::move(profile)), libraries_(std::move(libraries)),
      service_(new DynamicPlaylistService(
          [this, search = std::move(search)](engine::DynamicSelection selection,
                                             core::CancellationToken cancellation,
                                             DynamicPlaylistService::Completion completion) {
              search(engine(), std::move(selection), std::move(cancellation),
                     std::move(completion));
          },
          this)),
      results_(new LocalListModel(this)) {
    if (libraries_.empty()) {
        libraries_.push_back({EngineKey::local(), QStringLiteral("This computer")});
    }
    results_->setProperty("definition-owned", true);
    refresh_timer_.setSingleShot(true);
    refresh_timer_.setInterval(500);
    connect(&refresh_timer_, &QTimer::timeout, this, [this] {
        if (draft_.source == QStringLiteral("rules"))
            refresh();
    });
    poll_.setInterval(30000);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (shown_ && !busy_ && !draft_.shuffle)
            libraryChanged();
    });
    poll_.start();
    connect(service_, &DynamicPlaylistService::progress, this, [this](const QString& message) {
        status_ = message;
        emit changed();
    });
    connect(service_, &DynamicPlaylistService::finished, this, &DynamicPlaylistSession::finished);
    const auto loaded = dynamicPlaylistCatalog(profile_);
    if (loaded)
        definitions_ = *loaded;
    else
        catalog_writable_ = false;
    loadSelection();
    if (!loaded)
        status_ = QString::fromStdString(loaded.error().message);
}

DynamicPlaylistSession::~DynamicPlaylistSession() { service_->cancel(); }

std::vector<DynamicPlaylistSession::Choice> DynamicPlaylistSession::sources() {
    return {{QStringLiteral("Library rules"), QStringLiteral("rules")},
            {QStringLiteral("Last.fm similar tracks"), QStringLiteral("similar")},
            {QStringLiteral("Last.fm loved tracks"), QStringLiteral("loved")},
            {QStringLiteral("Last.fm top tracks (all time)"), QStringLiteral("top")},
            {QStringLiteral("Last.fm tag tracks"), QStringLiteral("tag")}};
}

QStringList DynamicPlaylistSession::libraryNames() const {
    QStringList names;
    for (const auto& library : libraries_) {
        names.append(library.name);
    }
    return names;
}

QString DynamicPlaylistSession::libraryKey(const int index) const {
    return index >= 0 && index < static_cast<int>(libraries_.size())
               ? libraries_[static_cast<std::size_t>(index)].engine.text()
               : QString{};
}

EngineKey DynamicPlaylistSession::engine() const {
    return libraries_[static_cast<std::size_t>(library_)].engine;
}

QStringList DynamicPlaylistSession::catalogNames() const {
    QStringList names{QStringLiteral("New dynamic playlist…")};
    for (const auto& definition : definitions_) {
        names.append(definition.name);
    }
    return names;
}

bool DynamicPlaylistSession::definitionShipped() const {
    return catalog_index_ > 0 && definitions_[static_cast<qsizetype>(catalog_index_ - 1)].shipped();
}

QString DynamicPlaylistSession::shuffleText() const {
    return draft_.source == QStringLiteral("rules") ? QStringLiteral("Shuffle results on refresh")
                                                    : QStringLiteral("Shuffle selected tracks");
}

QString DynamicPlaylistSession::shuffleTip() const {
    return draft_.source == QStringLiteral("rules")
               ? QString{}
               : QStringLiteral("Each refresh picks a fresh selection, favouring tracks outside "
                                "the previous result. This option also randomizes their order; "
                                "otherwise Last.fm ranking determines the order.");
}

DynamicPlaylistDefinition DynamicPlaylistSession::definition() const {
    auto definition = draft_;
    definition.id = catalog_index_ > 0 ? definitions_[static_cast<qsizetype>(catalog_index_ - 1)].id
                                       : QString{};
    definition.name = draft_.name.trimmed();
    definition.profile = profile_;
    return definition;
}

void DynamicPlaylistSession::chooseLibrary(int index) {
    // None chosen: this computer's, which is first.
    index = std::max(index, 0);
    if (index >= static_cast<int>(libraries_.size()) || index == library_) {
        return;
    }
    library_ = index;
    emit libraryChosen(engine());
    // The same definition, run against the other library.
    const bool saved_rules = catalog_index_ > 0 && draft_.source == QStringLiteral("rules");
    discardResults();
    status_ = QStringLiteral("Choose Refresh to evaluate this definition.");
    emit changed();
    if (saved_rules)
        refresh();
}

void DynamicPlaylistSession::followLibrary(const EngineKey& engine) {
    const auto wanted = std::ranges::find(libraries_, engine, &Library::engine);
    if (wanted != libraries_.end())
        chooseLibrary(static_cast<int>(wanted - libraries_.begin()));
}

void DynamicPlaylistSession::selectDefinition(const int index) {
    catalog_index_ = std::clamp(index, 0, static_cast<int>(definitions_.size()));
    loadSelection();
}

void DynamicPlaylistSession::loadSelection() {
    loading_ = true;
    DynamicPlaylistDefinition selected;
    if (catalog_index_ > 0)
        selected = definitions_[static_cast<qsizetype>(catalog_index_ - 1)];
    draft_ = selected;
    loading_ = false;
    discardResults();
    status_ = QStringLiteral("Choose Refresh to evaluate this definition.");
    emit catalogChanged();
    emit changed();
    if (!selected.id.isEmpty() && selected.source == QStringLiteral("rules"))
        refresh();
}

void DynamicPlaylistSession::edited() {
    discardResults();
    emit changed();
}

void DynamicPlaylistSession::setName(const QString& name) {
    draft_.name = name;
    emit changed();
}

void DynamicPlaylistSession::setSource(const QString& source) {
    draft_.source = source;
    edited();
}

void DynamicPlaylistSession::setQuery(const QString& query) {
    draft_.query = query;
    edited();
}

void DynamicPlaylistSession::setArtist(const QString& artist) {
    draft_.artist = artist;
    edited();
}

void DynamicPlaylistSession::setTrack(const QString& track) {
    draft_.track = track;
    edited();
}

void DynamicPlaylistSession::setUser(const QString& user) {
    draft_.user = user;
    edited();
}

void DynamicPlaylistSession::setTag(const QString& tag) {
    draft_.tag = tag;
    edited();
}

void DynamicPlaylistSession::setLimit(const int limit) {
    draft_.limit = limit;
    edited();
}

void DynamicPlaylistSession::setShuffle(const bool on) {
    draft_.shuffle = on;
    edited();
}

void DynamicPlaylistSession::setGroupBy(const QString& expression) {
    draft_.group_by = expression;
    edited();
}

void DynamicPlaylistSession::setGroups(const int groups) {
    draft_.groups = groups;
    edited();
}

void DynamicPlaylistSession::setPerGroup(const int per_group) {
    draft_.per_group = per_group;
    edited();
}

namespace {
// What is written: never the shipped definitions.
[[nodiscard]] QVector<DynamicPlaylistDefinition>
saved_only(QVector<DynamicPlaylistDefinition> definitions) {
    definitions.removeIf([](const auto& definition) { return definition.shipped(); });
    return definitions;
}
} // namespace

void DynamicPlaylistSession::save() {
    auto saved_definition = definition();
    if (saved_definition.shipped()) {
        // A shipped rule is kept as it ships; what is saved is a copy.
        saved_definition.id.clear();
        if (saved_definition.name ==
            definitions_[static_cast<qsizetype>(catalog_index_ - 1)].name) {
            saved_definition.name = tr("%1 (copy)").arg(saved_definition.name);
        }
    }
    if (saved_definition.name.isEmpty()) {
        status_ = QStringLiteral("Give the playlist a name");
        emit changed();
        return;
    }
    if (saved_definition.source == QStringLiteral("rules")) {
        const auto compiled = query::compile_tkq(saved_definition.query.toStdString());
        if (!compiled) {
            status_ = QString::fromStdString(compiled.error().message);
            emit changed();
            return;
        }
    }
    auto next = definitions_;
    if (saved_definition.id.isEmpty()) {
        saved_definition.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        next.push_back(saved_definition);
    } else
        for (auto& entry : next)
            if (entry.id == saved_definition.id)
                entry = saved_definition;
    const auto saved = saveDynamicPlaylists(profile_, saved_only(next));
    if (!saved) {
        status_ = QString::fromStdString(saved.error().message);
        emit changed();
        return;
    }
    definitions_ = std::move(next);
    const auto found =
        std::ranges::find(definitions_, saved_definition.id, &DynamicPlaylistDefinition::id);
    catalog_index_ =
        found == definitions_.end() ? 0 : static_cast<int>(found - definitions_.begin()) + 1;
    status_ = QStringLiteral("Definition saved");
    emit definitionsSaved();
    emit catalogChanged();
    emit changed();
}

void DynamicPlaylistSession::remove() {
    if (catalog_index_ <= 0 || definitionShipped())
        return;
    const auto id = definitions_[static_cast<qsizetype>(catalog_index_ - 1)].id;
    auto next = definitions_;
    next.removeIf([&id](const auto& entry) { return entry.id == id; });
    const auto saved = saveDynamicPlaylists(profile_, saved_only(next));
    if (!saved) {
        status_ = QString::fromStdString(saved.error().message);
        emit changed();
        return;
    }
    definitions_ = std::move(next);
    catalog_index_ = 0;
    emit definitionsSaved();
    loadSelection();
}

void DynamicPlaylistSession::discardResults() {
    if (loading_)
        return;
    auto_refresh_ = false;
    refresh_pending_ = false;
    refresh_timer_.stop();
    service_->cancel();
    busy_ = false;
    open_enabled_ = false;
    tracks_.clear();
    emit resultsAboutToChange();
    results_->replaceRows({});
    emit resultsChanged();
}

void DynamicPlaylistSession::refresh() {
    if (!authority_valid_)
        return;
    // Definition edits discard explicitly; refresh retains presentation anchors.
    refresh_timer_.stop();
    service_->cancel();
    open_enabled_ = false;
    auto_refresh_ = true;
    refresh_pending_ = false;
    busy_ = true;
    emit changed();
    service_->refresh(definition(), QSettings{}.value(QStringLiteral("lastfm/api-key")).toString());
}

void DynamicPlaylistSession::stop() {
    auto_refresh_ = false;
    refresh_pending_ = false;
    refresh_timer_.stop();
    service_->cancel();
    busy_ = false;
    status_ = QStringLiteral("Stopped");
    emit changed();
}

void DynamicPlaylistSession::libraryChanged() {
    if (!authority_valid_ || !auto_refresh_ || draft_.source != QStringLiteral("rules") ||
        draft_.query.isEmpty())
        return;
    if (busy_)
        refresh_pending_ = true;
    else
        refresh_timer_.start();
}

void DynamicPlaylistSession::invalidateAuthority() {
    authority_valid_ = false;
    discardResults();
    status_ = QStringLiteral(
        "The server connection changed. Reopen Dynamic playlists for the current library.");
    emit changed();
}

void DynamicPlaylistSession::finished(const DynamicPlaylistService::Tracks& tracks,
                                      const int unmatched, const QString& error) {
    busy_ = false;
    if (refresh_pending_) {
        refresh_pending_ = false;
        // A database change during this query invalidates its snapshot.
        // Retain the last displayed result until a fresh evaluation finishes.
        emit changed();
        libraryChanged();
        return;
    }
    if (!error.isEmpty()) {
        discardResults();
        status_ = error;
        emit changed();
        return;
    }
    tracks_ = tracks;
    emit resultsAboutToChange();
    const auto& previous = results_->rows();
    const bool unchanged =
        tracks_.size() == previous.size() &&
        std::equal(tracks_.begin(), tracks_.end(), previous.begin(),
                   [](const auto& a, const auto& b) {
                       return a == b && a.rating == b.rating && a.album_rating == b.album_rating;
                   });
    if (!unchanged)
        results_->replaceRows(tracks_);
    emit resultsChanged();
    const auto count = tracks_.size();
    open_enabled_ = count > 0;
    status_ =
        draft_.source == QStringLiteral("rules")
            ? QStringLiteral("%1 tracks · rules update automatically while this window is open")
                  .arg(count)
            : QStringLiteral("%1 tracks selected from %2 library matches · %3 Last.fm "
                             "tracks not found%4")
                  .arg(count)
                  .arg(service_->matchedPoolSize())
                  .arg(unmatched)
                  .arg(service_->matchedPoolSize() <= static_cast<std::size_t>(draft_.limit)
                           ? QStringLiteral(" · all available matches included")
                           : QString{});
    emit changed();
}

} // namespace trackknife::bench
