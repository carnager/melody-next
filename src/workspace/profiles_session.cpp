// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/profiles_session.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/operations/output_path_plan.hpp"

#include <QPointer>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] std::string encoded_utf8(const QString& text) {
    const auto bytes = text.toUtf8();
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

template <typename Profiles>
[[nodiscard]] int rowOf(const Profiles& profiles, const std::optional<core::StableId>& id) {
    if (profiles.empty()) {
        return -1;
    }
    if (!id) {
        return 0;
    }
    const auto found = std::ranges::find(profiles, *id, [](const auto& saved) { return saved.id; });
    return found == profiles.end() ? 0 : static_cast<int>(found - profiles.begin());
}

} // namespace

ProfilesSession::ProfilesSession(OutputProfileStore store, QObject* parent)
    : QObject(parent), store_(std::move(store)), places_(store_.places) {
    if (places_.empty()) {
        // Only this computer's: the destinations the store itself holds.
        places_.push_back(DestinationPlace{
            .key = {},
            .name = store_.destinations_on.isEmpty() ? QStringLiteral("this computer")
                                                     : store_.destinations_on,
            .load =
                [load = store_.load](DestinationPlace::LoadCompletion completion) {
                    if (!load) {
                        completion({}, QStringLiteral("Profile storage is unavailable"));
                        return;
                    }
                    load([completion = std::move(completion)](
                             std::vector<persistence::SavedOutputLayoutProfile>,
                             std::vector<persistence::SavedDestinationProfile> destinations,
                             QString error) { completion(std::move(destinations), error); });
                },
            .save = store_.save_destination,
            .remove = store_.remove_destination,
            .folders = {},
            .copyable = {},
        });
    }
    held_.resize(places_.size());
    reload();
}

ProfilesSession::~ProfilesSession() = default;

std::vector<ProfilesSession::Choice> ProfilesSession::sanitizationPolicies() {
    return {{QStringLiteral("Linux filenames"), QStringLiteral("linux")},
            {QStringLiteral("Portable filenames"), QStringLiteral("portable")}};
}

QStringList ProfilesSession::layoutNames() const {
    QStringList names;
    for (const auto& saved : layouts_) {
        names.append(displayText(saved.profile.name));
    }
    return names;
}

QStringList ProfilesSession::destinationNames() const { return destinationNamesOn(place_); }

QStringList ProfilesSession::destinationNamesOn(const int place) const {
    QStringList names;
    if (place < 0 || place >= placeCount()) {
        return names;
    }
    for (const auto& saved : held_[static_cast<std::size_t>(place)].destinations) {
        names.append(displayText(saved.profile.name));
    }
    return names;
}

QString ProfilesSession::destinationRootOn(const int place, const int row) const {
    if (place < 0 || place >= placeCount()) {
        return {};
    }
    const auto& destinations = held_[static_cast<std::size_t>(place)].destinations;
    return row >= 0 && row < static_cast<int>(destinations.size())
               ? QString::fromStdString(core::display_raw_path(
                     destinations[static_cast<std::size_t>(row)].profile.root_raw_path))
               : QString{};
}

QString ProfilesSession::placeNote(const int place) const {
    if (place < 0 || place >= placeCount()) {
        return {};
    }
    const auto& held = held_[static_cast<std::size_t>(place)];
    if (held.loading) {
        return QStringLiteral("Loading…");
    }
    if (!held.error.isEmpty()) {
        return QStringLiteral("Could not load · %1").arg(held.error);
    }
    return held.destinations.empty() ? QStringLiteral("None yet") : QString{};
}

bool ProfilesSession::destinationsAvailable() const {
    return available() && !held_[static_cast<std::size_t>(place_)].loading;
}

QStringList ProfilesSession::placeNames() const {
    QStringList names;
    for (const auto& place : places_) {
        names.append(place.name);
    }
    return names;
}

QString ProfilesSession::placeKey(const int place) const {
    return place >= 0 && place < static_cast<int>(places_.size())
               ? places_[static_cast<std::size_t>(place)].key
               : QString{};
}

int ProfilesSession::layoutRow() const {
    return editing_layout_id_ ? rowOf(layouts_, editing_layout_id_) : -1;
}

int ProfilesSession::destinationRow() const {
    return editing_destination_id_ ? rowOf(shown(), editing_destination_id_) : -1;
}

const DestinationPlace& ProfilesSession::shownPlace() const {
    return places_[static_cast<std::size_t>(place_)];
}

QString ProfilesSession::placeName() const { return shownPlace().name; }

QString ProfilesSession::destinationRoot() const {
    return QString::fromStdString(core::display_raw_path(root_raw_path_));
}

EngineFolderLister ProfilesSession::folders() const { return shownPlace().folders; }

bool ProfilesSession::canEditLayouts() const { return available() && bool{store_.save_layout}; }

bool ProfilesSession::canSaveLayout() const {
    return canEditLayouts() && !layout_name_.isEmpty() && !basename_expression_.isEmpty();
}

bool ProfilesSession::canRemoveLayout() const {
    return available() && bool{store_.remove_layout} && editing_layout_id_.has_value();
}

bool ProfilesSession::canEditDestinations() const {
    return destinationsAvailable() && bool{shownPlace().save};
}

bool ProfilesSession::canSaveDestination() const {
    return canEditDestinations() && !destination_name_.isEmpty() && !root_raw_path_.empty();
}

bool ProfilesSession::canRemoveDestination() const {
    return destinationsAvailable() && bool{shownPlace().remove} &&
           editing_destination_id_.has_value();
}

int ProfilesSession::placeOf(const QString& key) const {
    const auto found = std::ranges::find(places_, key, &DestinationPlace::key);
    return found == places_.end() ? -1 : static_cast<int>(found - places_.begin());
}

void ProfilesSession::reload() {
    if (!store_.load) {
        status_ = QStringLiteral("Profile storage is unavailable");
        emit changed();
        return;
    }
    loading_ = true;
    emit changed();
    const QPointer self{this};
    store_.load([self](std::vector<persistence::SavedOutputLayoutProfile> layouts,
                       std::vector<persistence::SavedDestinationProfile>, QString error) {
        if (!self) {
            return;
        }
        self->loading_ = false;
        if (!error.isEmpty()) {
            self->status_ = QStringLiteral("Could not load output profiles · %1").arg(error);
            emit self->changed();
            return;
        }
        self->layouts_ = std::move(layouts);
        self->rebuildLists(self->editing_layout_id_, self->editing_destination_id_);
        for (int place = 0; place < self->placeCount(); ++place) {
            self->reloadDestinations(place);
        }
    });
}

void ProfilesSession::reloadDestinations(const int place) {
    const auto& loaded = places_[static_cast<std::size_t>(place)];
    if (!loaded.load) {
        return;
    }
    held_[static_cast<std::size_t>(place)].loading = true;
    emit changed();
    const QPointer self{this};
    loaded.load([self, place](std::vector<persistence::SavedDestinationProfile> destinations,
                              QString error) {
        if (!self) {
            return;
        }
        auto& held = self->held_[static_cast<std::size_t>(place)];
        held.loading = false;
        held.error = error;
        if (error.isEmpty()) {
            std::ranges::sort(destinations, {},
                              [](const auto& profile) { return profile.profile.name; });
            held.destinations = std::move(destinations);
        } else {
            held.destinations.clear();
        }
        if (place == self->place_) {
            self->rebuildLists(self->editing_layout_id_, self->editing_destination_id_);
        } else {
            emit self->listsChanged();
        }
        self->countCopyable();
        self->status_ = self->summary();
        emit self->changed();
    });
}

void ProfilesSession::countCopyable() {
    const auto& place = shownPlace();
    const auto copyable = place.copyable ? place.copyable() : decltype(place.copyable()){};
    copyable_ = static_cast<int>(std::ranges::count_if(copyable, [this](const auto& candidate) {
        return std::ranges::none_of(shown(), [&candidate](const auto& held) {
            return held.profile.root_raw_path == candidate.profile.root_raw_path;
        });
    }));
}

QString ProfilesSession::summary() const {
    std::size_t destinations = 0;
    for (const auto& held : held_) {
        destinations += held.destinations.size();
    }
    auto text = QStringLiteral("%1 naming %2 · %3 move %4")
                    .arg(layouts_.size())
                    .arg(layouts_.size() == 1U ? QStringLiteral("layout")
                                               : QStringLiteral("layouts"))
                    .arg(destinations)
                    .arg(destinations == 1U ? QStringLiteral("destination")
                                            : QStringLiteral("destinations"));
    // One place: which one; more: each lists its own.
    if (places_.size() == 1U) {
        text += QStringLiteral(" on %1").arg(places_.front().name);
    }
    return text;
}

void ProfilesSession::rebuildLists(const std::optional<core::StableId> layout_id,
                                   const std::optional<core::StableId> destination_id) {
    const auto layout_row = rowOf(layouts_, layout_id);
    const auto destination_row = rowOf(shown(), destination_id);
    emit listsChanged();
    selectLayout(layout_row);
    selectDestination(destination_row);
}

void ProfilesSession::selectLayout(const int row) {
    if (row < 0 || row >= static_cast<int>(layouts_.size())) {
        editing_layout_id_.reset();
        layout_name_.clear();
        directory_expression_.clear();
        basename_expression_.clear();
        sanitization_ = QStringLiteral("linux");
        emit changed();
        return;
    }
    const auto& saved = layouts_[static_cast<std::size_t>(row)];
    editing_layout_id_ = saved.id;
    layout_name_ = displayText(saved.profile.name);
    directory_expression_ = displayText(saved.profile.relative_directory_expression);
    basename_expression_ = displayText(saved.profile.basename_expression);
    const auto policy = displayText(saved.profile.sanitization_policy.name);
    const auto policies = sanitizationPolicies();
    sanitization_ = std::ranges::any_of(policies,
                                        [&policy](const Choice& choice) {
                                            return choice.value.toString() == policy;
                                        })
                        ? policy
                        : QStringLiteral("linux");
    emit changed();
}

void ProfilesSession::setLayoutName(const QString& text) {
    layout_name_ = text;
    emit changed();
}

void ProfilesSession::setDirectoryExpression(const QString& text) {
    directory_expression_ = text;
    emit changed();
}

void ProfilesSession::setBasenameExpression(const QString& text) {
    basename_expression_ = text;
    emit changed();
}

void ProfilesSession::setSanitization(const QString& policy) {
    sanitization_ = policy;
    emit changed();
}

void ProfilesSession::selectPlace(const int index) {
    if (index < 0 || index >= static_cast<int>(places_.size()) || index == place_) {
        return;
    }
    place_ = index;
    editing_destination_id_.reset();
    countCopyable();
    rebuildLists(editing_layout_id_, {});
    if (!held_[static_cast<std::size_t>(index)].error.isEmpty()) {
        reloadDestinations(index);
    }
}

void ProfilesSession::selectDestinationOn(const int place, const int row) {
    if (place < 0 || place >= placeCount()) {
        return;
    }
    if (place != place_) {
        place_ = place;
        countCopyable();
    }
    selectDestination(row);
}

void ProfilesSession::selectDestination(const int row) {
    if (row < 0 || row >= static_cast<int>(shown().size())) {
        editing_destination_id_.reset();
        root_raw_path_.clear();
        destination_name_.clear();
        emit changed();
        return;
    }
    const auto& saved = shown()[static_cast<std::size_t>(row)];
    editing_destination_id_ = saved.id;
    root_raw_path_ = saved.profile.root_raw_path;
    destination_name_ = displayText(saved.profile.name);
    emit changed();
}

void ProfilesSession::setDestinationName(const QString& text) {
    destination_name_ = text;
    emit changed();
}

void ProfilesSession::chooseRoot(std::string raw_path) {
    root_raw_path_ = std::move(raw_path);
    emit changed();
}

void ProfilesSession::copyDestinations() {
    const auto& place = shownPlace();
    if (!place.copyable || !place.save || mutation_running_) {
        return;
    }
    std::vector<persistence::SavedDestinationProfile> missing;
    for (auto candidate : place.copyable()) {
        if (std::ranges::none_of(shown(), [&candidate](const auto& held) {
                return held.profile.root_raw_path == candidate.profile.root_raw_path;
            })) {
            candidate.id = core::StableId::random();
            missing.push_back(std::move(candidate));
        }
    }
    if (missing.empty()) {
        return;
    }
    mutation_running_ = true;
    emit changed();
    // One after another, then the list as the engine has it.
    auto remaining =
        std::make_shared<std::vector<persistence::SavedDestinationProfile>>(std::move(missing));
    auto next = std::make_shared<std::function<void(QString)>>();
    const QPointer self{this};
    const auto asked = place_;
    *next = [self, asked, remaining, next](QString error) {
        if (!self) {
            return;
        }
        if (!error.isEmpty() || remaining->empty()) {
            self->mutation_running_ = false;
            self->reloadDestinations(asked);
            if (!error.isEmpty()) {
                self->status_ = QStringLiteral("Could not copy a move destination · %1").arg(error);
            }
            emit self->profilesChanged();
            *next = {};
            return;
        }
        auto destination = std::move(remaining->back());
        remaining->pop_back();
        self->places_[static_cast<std::size_t>(asked)].save(std::move(destination), *next);
    };
    (*next)(QString{});
}

void ProfilesSession::saveLayout() {
    if (!store_.save_layout || mutation_running_) {
        return;
    }
    persistence::SavedOutputLayoutProfile saved{
        .id = editing_layout_id_.value_or(core::StableId::random()),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = encoded_utf8(layout_name_),
                .dialect = {},
                .relative_directory_expression = encoded_utf8(directory_expression_),
                .basename_expression = encoded_utf8(basename_expression_),
                .sanitization_policy = {encoded_utf8(sanitization_), 1U},
            },
    };
    if (auto valid = operations::validate_output_layout_profile(saved.profile); !valid) {
        status_ = QStringLiteral("Naming layout is not valid · %1")
                      .arg(displayText(valid.error().message));
        emit changed();
        return;
    }
    mutation_running_ = true;
    emit changed();
    const QPointer self{this};
    auto retained = saved;
    store_.save_layout(std::move(saved), [self, saved = std::move(retained)](QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_ = QStringLiteral("Could not save naming layout · %1").arg(error);
            emit self->changed();
            return;
        }
        const auto found =
            std::ranges::find(self->layouts_, saved.id, &persistence::SavedOutputLayoutProfile::id);
        if (found == self->layouts_.end()) {
            self->layouts_.push_back(saved);
        } else {
            *found = saved;
        }
        std::ranges::sort(self->layouts_, {},
                          [](const auto& profile) { return profile.profile.name; });
        self->editing_layout_id_ = saved.id;
        self->rebuildLists(saved.id, self->editing_destination_id_);
        self->status_ = QStringLiteral("Naming layout saved");
        emit self->changed();
        emit self->profilesChanged();
    });
}

void ProfilesSession::saveDestination() {
    if (!shownPlace().save || mutation_running_) {
        return;
    }
    persistence::SavedDestinationProfile saved{
        .id = editing_destination_id_.value_or(core::StableId::random()),
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = encoded_utf8(destination_name_),
                .root_raw_path = root_raw_path_,
                .containment_policy = {"lexical-beneath-root", 1U},
            },
    };
    if (auto valid = operations::validate_destination_profile(saved.profile); !valid) {
        status_ = QStringLiteral("Move destination is not valid · %1")
                      .arg(displayText(valid.error().message));
        emit changed();
        return;
    }
    mutation_running_ = true;
    emit changed();
    const QPointer self{this};
    auto retained = saved;
    const auto asked = place_;
    shownPlace().save(std::move(saved), [self, asked, saved = std::move(retained)](QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_ = QStringLiteral("Could not save move destination · %1").arg(error);
            emit self->changed();
            return;
        }
        auto& destinations = self->held_[static_cast<std::size_t>(asked)].destinations;
        const auto found =
            std::ranges::find(destinations, saved.id, &persistence::SavedDestinationProfile::id);
        if (found == destinations.end()) {
            destinations.push_back(saved);
        } else {
            *found = saved;
        }
        std::ranges::sort(destinations, {},
                          [](const auto& profile) { return profile.profile.name; });
        if (asked == self->place_) {
            self->editing_destination_id_ = saved.id;
            self->rebuildLists(self->editing_layout_id_, saved.id);
        } else {
            emit self->listsChanged();
        }
        self->status_ = QStringLiteral("Move destination saved");
        emit self->changed();
        emit self->profilesChanged();
    });
}

void ProfilesSession::removeLayout() {
    if (!editing_layout_id_ || !store_.remove_layout || mutation_running_) {
        return;
    }
    const auto id = *editing_layout_id_;
    mutation_running_ = true;
    emit changed();
    const QPointer self{this};
    store_.remove_layout(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_ = QStringLiteral("Could not remove naming layout · %1").arg(error);
            emit self->changed();
            return;
        }
        std::erase_if(self->layouts_, [id](const auto& saved) { return saved.id == id; });
        self->editing_layout_id_.reset();
        self->rebuildLists({}, self->editing_destination_id_);
        self->status_ = QStringLiteral("Naming layout removed");
        emit self->changed();
        emit self->profilesChanged();
    });
}

void ProfilesSession::removeDestination() {
    if (!editing_destination_id_ || !shownPlace().remove || mutation_running_) {
        return;
    }
    const auto id = *editing_destination_id_;
    mutation_running_ = true;
    emit changed();
    const QPointer self{this};
    const auto asked = place_;
    shownPlace().remove(id, [self, asked, id](QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_ = QStringLiteral("Could not remove move destination · %1").arg(error);
            emit self->changed();
            return;
        }
        std::erase_if(self->held_[static_cast<std::size_t>(asked)].destinations,
                      [id](const auto& saved) { return saved.id == id; });
        if (asked == self->place_) {
            self->editing_destination_id_.reset();
            self->rebuildLists(self->editing_layout_id_, {});
        } else {
            emit self->listsChanged();
        }
        self->status_ = QStringLiteral("Move destination removed");
        emit self->changed();
        emit self->profilesChanged();
    });
}

} // namespace trackknife::bench
