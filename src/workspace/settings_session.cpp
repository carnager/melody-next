// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/settings_session.hpp"

#include "workspace/interface_scale.hpp"

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/local_playback.hpp"
#include "trackknife/discovery/mdns.hpp"
#include "trackknife/metadata/ratings.hpp"
#include "workspace/color_scheme.hpp"

#include <QPointer>
#include <QSettings>
#include <QSysInfo>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

// ADR-0239: what is streamed to this computer's speakers, as kbps of Opus;
// 0 the original files, -1 (where offered) by the route.
[[nodiscard]] QString rateLabel(const int kbps) {
    if (kbps < 0) {
        return QStringLiteral("Automatic");
    }
    return kbps == 0 ? QStringLiteral("Original files") : QStringLiteral("Opus %1 kbps").arg(kbps);
}

const QString notifications_key = QStringLiteral("desktop/notifications");
const QString notifications_background_key =
    QStringLiteral("desktop/notifications-background-only");
const QString panel_animations_key = QStringLiteral("appearance/panel-animations");
const QString lists_display_key = QStringLiteral("appearance/lists-display");
const QString buffer_profile_key = QStringLiteral("playback/buffer-profile");
const QString buffer_capacity_key = QStringLiteral("playback/buffer-capacity-ms");
const QString buffer_threshold_key = QStringLiteral("playback/buffer-start-threshold-ms");
const QString preamp_with_key = QStringLiteral("playback/rg-preamp-with");
const QString preamp_without_key = QStringLiteral("playback/rg-preamp-without");
const QString lastfm_key = QStringLiteral("lastfm/api-key");

} // namespace

SettingsSession::SettingsSession(QObject* parent) : QObject(parent) {
    const QSettings settings;
    // As the type its default has: an INI file hands every value back as text,
    // and the text "false" is true to anything that only asks whether a
    // value is there -- the Qt Quick window's checkboxes showed every option
    // left off as on.
    const auto read = [this, &settings](const QString& key, const QVariant& fallback) {
        auto value = settings.value(key, fallback);
        if (value.metaType() != fallback.metaType() && !value.convert(fallback.metaType())) {
            value = fallback;
        }
        values_.insert(key, value);
    };
    read(notifications_key, false);
    read(notifications_background_key, false);
    read(panel_animations_key, true);
    read(QLatin1String(color_scheme_key), QStringLiteral("system"));
    read(lists_display_key, QStringLiteral("tabs"));
    auto profile = settings.value(buffer_profile_key, QStringLiteral("balanced")).toString();
    const auto capacity = settings.value(buffer_capacity_key, 750).toInt();
    const auto threshold = settings.value(buffer_threshold_key, 100).toInt();
    if (profile == QStringLiteral("custom") &&
        (capacity < 10 || capacity > 10000 || threshold < 1 || threshold > capacity)) {
        profile = QStringLiteral("balanced");
    }
    if (std::ranges::none_of(bufferProfiles(),
                             [&profile](const Choice& choice) { return choice.value == profile; })) {
        profile = QStringLiteral("balanced");
    }
    values_.insert(buffer_capacity_key, std::clamp(capacity, 10, 10000));
    values_.insert(buffer_threshold_key, std::clamp(threshold, 1, 10000));
    setValue(buffer_profile_key, profile);
    read(preamp_with_key, 0.0);
    read(preamp_without_key, 0.0);
    read(QLatin1String(SettingsKeys::ratings_in_tags_key), false);
    read(QLatin1String(SettingsKeys::rating_tag_scale_key), QStringLiteral("off"));
    read(QLatin1String(SettingsKeys::rating_backup_key), false);
    read(QLatin1String(SettingsKeys::rating_backup_tag_key),
         QString::fromLatin1(metadata::default_rating_backup_tag));
    read(QLatin1String(SettingsKeys::engine_password_key), QString{});
    read(QLatin1String(SettingsKeys::library_show_local_key), true);
    read(QLatin1String(SettingsKeys::engine_share_key), false);
    read(QLatin1String(SettingsKeys::engine_listen_key),
         QString::fromLatin1(SettingsKeys::engine_listen_default));
    read(QLatin1String(SettingsKeys::engine_stream_port_key),
         SettingsKeys::engine_stream_port_default);
    read(QLatin1String(SettingsKeys::engine_music_root_key), QString{});
    read(QLatin1String(SettingsKeys::engine_play_for_remote_key), true);
    read(QLatin1String(SettingsKeys::engine_stream_nearby_key),
         SettingsKeys::engine_stream_nearby_default);
    read(QLatin1String(SettingsKeys::engine_stream_away_key),
         SettingsKeys::engine_stream_away_default);
    read(QLatin1String(SettingsKeys::replaygain_sidecar_only_key), false);
    read(QLatin1String(SettingsKeys::replaygain_true_peak_key), false);
    read(QLatin1String(SettingsKeys::artwork_embed_key), true);
    read(QLatin1String(SettingsKeys::artwork_folder_image_key), false);
    read(QLatin1String(SettingsKeys::artwork_folder_image_name_key), QStringLiteral("cover.jpg"));
    read(QLatin1String(SettingsKeys::artwork_fetch_source_key), QStringLiteral("coverartarchive"));
    read(QLatin1String(SettingsKeys::artwork_max_embedded_edge_key), 0);
    read(QLatin1String(SettingsKeys::artwork_max_folder_edge_key), 0);
    read(QLatin1String(SettingsKeys::acoustid_client_key), QString{});
    read(lastfm_key, QString{});

    // ADR-0234: engines on other machines, each connected at once beside
    // this computer's. One to fill in when there are none.
    engines_ = loadRemoteEngines();
    if (engines_.empty()) {
        engines_.push_back({});
    }
    engine_current_ = 0;

    // Engines that announce themselves on the network, by name: chosen
    // rather than typed. The list fills as they answer.
    const QPointer self{this};
    if (auto browser = discovery::Browser::start([self](const std::vector<discovery::Found>& found) {
            QMetaObject::invokeMethod(
                self,
                [self, found] {
                    if (!self) {
                        return;
                    }
                    self->found_.clear();
                    for (const auto& announced : found) {
                        const auto where = QStringLiteral("%1:%2")
                                               .arg(QString::fromStdString(announced.address))
                                               .arg(announced.port);
                        const bool locked =
                            announced.txt.contains("auth") && announced.txt.at("auth") == "1";
                        self->found_.push_back(Found{
                            .label = QStringLiteral("%1 — %2%3")
                                         .arg(QString::fromStdString(announced.instance), where,
                                              locked ? QStringLiteral(" · password") : QString{}),
                            .address = where});
                    }
                    emit self->foundChanged();
                },
                Qt::QueuedConnection);
        })) {
        browser_ = std::move(*browser);
    } else {
        discovery_error_ = QString::fromStdString(browser.error().message);
    }
}

SettingsSession::~SettingsSession() = default;

QStringList SettingsSession::pageTitles() {
    return {QStringLiteral("General"),    QStringLiteral("Playback"),
            QStringLiteral("Library"),    QStringLiteral("Engine"),
            QStringLiteral("Naming"),     QStringLiteral("ReplayGain"),
            QStringLiteral("Covers"),     QStringLiteral("Metadata services"),
            QStringLiteral("Last.fm"),    QStringLiteral("Shortcuts")};
}

QString SettingsSession::saveNote(const Page page) {
    if (page == Page::library) {
        return QStringLiteral("Folder changes save immediately. Cancel does not undo them.");
    }
    if (page == Page::naming) {
        return QStringLiteral("Save layout, Save destination, and Remove take effect immediately. "
                              "Cancel does not undo them.");
    }
    return {};
}

std::vector<SettingsSession::Choice> SettingsSession::listsDisplays() {
    // ADR-0233: the lists as tabs above the tracks, or as a pane beside them.
    return {{QStringLiteral("Tab bar"), QStringLiteral("tabs")},
            {QStringLiteral("Side panel"), QStringLiteral("panel")}};
}

std::vector<SettingsSession::Choice> SettingsSession::bufferProfiles() {
    return {{QStringLiteral("Responsive"), QStringLiteral("responsive")},
            {QStringLiteral("Balanced"), QStringLiteral("balanced")},
            {QStringLiteral("Resilient"), QStringLiteral("resilient")},
            {QStringLiteral("Custom"), QStringLiteral("custom")}};
}

QString SettingsSession::ratingBackupNote(const QString& name) {
    const auto tag = name.trimmed().toStdString();
    if (auto problem = metadata::rating_backup_tag_problem(tag)) {
        return QString::fromStdString(*problem) + QStringLiteral(": nothing is copied.");
    }
    if (metadata::official_tag_name(tag)) {
        return QStringLiteral("%1 is an official tag other players show and use. Whatever it "
                              "holds now is replaced by the rating, in every rated file.")
            .arg(name.trimmed().toUpper());
    }
    return {};
}

std::vector<SettingsSession::Choice> SettingsSession::colorSchemes() {
    return {{QStringLiteral("As the system"), QStringLiteral("system")},
            {QStringLiteral("Light"), QStringLiteral("light")},
            {QStringLiteral("Dark"), QStringLiteral("dark")}};
}

// ADR-0251: on top of the desktop's own scaling, which applies by itself.
std::vector<SettingsSession::Choice> SettingsSession::interfaceScales() {
    std::vector<Choice> choices;
    for (const auto scale : bench::interfaceScales()) {
        const auto percent = QString::number(qRound(scale * 100.0));
        choices.push_back({scale == 1.0 ? QStringLiteral("As the system (100%)")
                                        : percent + QLatin1Char('%'),
                           QString::number(scale)});
    }
    return choices;
}

std::vector<SettingsSession::Choice> SettingsSession::ratingScales() {
    return {{QStringLiteral("Don't import"), QStringLiteral("off")},
            {QStringLiteral("1–5 (foobar2000)"), QStringLiteral("5")},
            {QStringLiteral("0–10"), QStringLiteral("10")},
            {QStringLiteral("0–100 (MusicBee, MediaMonkey)"), QStringLiteral("100")}};
}

std::vector<SettingsSession::Choice> SettingsSession::fetchSources() {
    return {{QStringLiteral("Cover Art Archive (front)"), QStringLiteral("coverartarchive")}};
}

QStringList SettingsSession::folderImageNames() {
    return {QStringLiteral("cover.jpg"), QStringLiteral("folder.jpg")};
}

double SettingsSession::maximumPreamp() {
    return static_cast<double>(audio::maximum_replay_gain_preamp_db);
}

std::vector<SettingsSession::Choice>
SettingsSession::rates(const std::initializer_list<int> offered, const int current) const {
    std::vector<Choice> choices;
    for (const auto kbps : offered) {
        choices.push_back({rateLabel(kbps), kbps});
    }
    // Set by hand to a rate not offered: kept, and shown as it is.
    if (std::ranges::find(offered, current) == offered.end()) {
        choices.push_back({rateLabel(current), current});
    }
    return choices;
}

std::vector<SettingsSession::Choice> SettingsSession::nearbyRates() const {
    return rates({0, 192, 160, 128},
                 values_.value(QLatin1String(SettingsKeys::engine_stream_nearby_key)).toInt());
}

std::vector<SettingsSession::Choice> SettingsSession::awayRates() const {
    return rates({192, 160, 128, 96, 64, 0},
                 values_.value(QLatin1String(SettingsKeys::engine_stream_away_key)).toInt());
}

std::vector<SettingsSession::Choice> SettingsSession::engineRates() const {
    const auto current = engine().stream_kbps;
    return rates({-1, 0, 192, 160, 128, 96, 64}, current < 0 ? -1 : current);
}

void SettingsSession::setValue(const QString& key, const QVariant& value) {
    values_.insert(key, value);
    if (key == buffer_profile_key) {
        const auto id = value.toString().toStdString();
        if (const auto preset = audio::playback_buffer_preset_from_id(id)) {
            const auto config = audio::playback_buffer_preset_config(*preset);
            values_.insert(buffer_capacity_key, static_cast<int>(config.capacity.count()));
            values_.insert(buffer_threshold_key, static_cast<int>(config.start_threshold.count()));
        }
    } else if (key == buffer_capacity_key) {
        // The buffer starts playing before it is full, never after.
        values_.insert(buffer_threshold_key,
                       std::min(values_.value(buffer_threshold_key).toInt(), value.toInt()));
    }
    emit changed();
}

bool SettingsSession::bufferCustom() const {
    return !audio::playback_buffer_preset_from_id(
        values_.value(buffer_profile_key).toString().toStdString());
}

// What to run on the machine with the speakers, kept in step with the
// fields: the one thing the sharing part of the page is for, spelled out.
QString SettingsSession::agentCommand() const {
    if (!values_.value(QLatin1String(SettingsKeys::engine_share_key)).toBool()) {
        return QStringLiteral(
            "Not shared: only this computer plays, and Trackknife here controls it.");
    }
    const auto listen = values_.value(QLatin1String(SettingsKeys::engine_listen_key))
                            .toString()
                            .trimmed();
    const auto colon = listen.lastIndexOf(QLatin1Char(':'));
    auto host = colon > 0 ? listen.left(colon) : listen;
    if (host.isEmpty() || host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::")) {
        host = QSysInfo::machineHostName();
    }
    const auto port = colon > 0 ? listen.mid(colon + 1) : QString{};
    if (values_.value(QLatin1String(SettingsKeys::engine_password_key)).toString().isEmpty()) {
        return QStringLiteral("Set the password above to share: every connection from the network "
                              "must give it.");
    }
    const auto command = QStringLiteral("melody-agent --server %1:%2 --password …").arg(host, port);
    return QStringLiteral("On a machine with speakers, run:\n%1\nAdd --music-root DIR where it has "
                          "the music itself; without it, it streams. Changing these restarts this "
                          "computer's engine; playback comes back paused.")
        .arg(command);
}

// The engines elsewhere.

QStringList SettingsSession::engineLabels() const {
    QStringList labels;
    for (const auto& engine : engines_) {
        labels.push_back(engine.address.isEmpty() ? QStringLiteral("New engine") : engine.address);
    }
    return labels;
}

RemoteEngineSetting SettingsSession::engine() const {
    return engine_current_ >= 0 && engine_current_ < static_cast<int>(engines_.size())
               ? engines_[static_cast<std::size_t>(engine_current_)]
               : RemoteEngineSetting{};
}

void SettingsSession::selectEngine(const int row) {
    engine_current_ = row;
    emit enginesChanged(row);
}

void SettingsSession::editEngine(const RemoteEngineSetting& edited) {
    if (engine_current_ < 0 || engine_current_ >= static_cast<int>(engines_.size())) {
        return;
    }
    auto& chosen = engines_[static_cast<std::size_t>(engine_current_)];
    const auto address = edited.address.trimmed();
    // Another address may be another engine: its id is learned anew.
    if (address != chosen.address) {
        chosen.id.clear();
    }
    chosen.address = address;
    chosen.password = edited.password.trimmed();
    chosen.music_folder = edited.music_folder.trimmed();
    chosen.reachable_at = edited.reachable_at.trimmed();
    chosen.stream_kbps = edited.stream_kbps;
    emit changed();
}

void SettingsSession::addEngine() {
    engines_.push_back({});
    selectEngine(static_cast<int>(engines_.size()) - 1);
}

void SettingsSession::removeEngine() {
    if (engine_current_ < 0 || engine_current_ >= static_cast<int>(engines_.size())) {
        return;
    }
    engines_.erase(engines_.begin() + engine_current_);
    if (engines_.empty()) {
        engines_.push_back({});
    }
    selectEngine(std::min(engine_current_, static_cast<int>(engines_.size()) - 1));
}

void SettingsSession::chooseFoundEngine(const QString& address) {
    // Known already: chosen. Otherwise into the one being filled in, if it
    // is still empty, or as one more.
    for (int row = 0; row < static_cast<int>(engines_.size()); ++row) {
        if (engines_[static_cast<std::size_t>(row)].address == address) {
            selectEngine(row);
            return;
        }
    }
    const bool blank = engine_current_ >= 0 &&
                       engine_current_ < static_cast<int>(engines_.size()) &&
                       engines_[static_cast<std::size_t>(engine_current_)].address.isEmpty();
    if (!blank) {
        engines_.push_back({});
        engine_current_ = static_cast<int>(engines_.size()) - 1;
    }
    auto edited = engine();
    edited.address = address;
    editEngine(edited);
    selectEngine(engine_current_);
}

// Saving.

std::optional<SettingsSession::Page> SettingsSession::save() {
    // ADR-0223: sharing without a password is not a thing to save.
    if (values_.value(QLatin1String(SettingsKeys::engine_share_key)).toBool() &&
        values_.value(QLatin1String(SettingsKeys::engine_password_key)).toString().isEmpty()) {
        return Page::engine;
    }
    QSettings settings;
    const auto trimmed = [this](const QString& key) { return values_.value(key).toString().trimmed(); };
    for (auto entry = values_.cbegin(); entry != values_.cend(); ++entry) {
        settings.setValue(entry.key(), entry.value());
    }
    for (const auto* key :
         {SettingsKeys::acoustid_client_key, SettingsKeys::engine_listen_key,
          SettingsKeys::engine_music_root_key, SettingsKeys::artwork_folder_image_name_key}) {
        settings.setValue(QLatin1String(key), trimmed(QLatin1String(key)));
    }
    settings.setValue(lastfm_key, trimmed(lastfm_key));
    // ADR-0234: the engines elsewhere, those with an address.
    std::vector<RemoteEngineSetting> kept;
    for (const auto& entry : engines_) {
        if (!entry.address.isEmpty()) {
            kept.push_back(entry);
        }
    }
    saveRemoteEngines(kept);
    return std::nullopt;
}

} // namespace trackknife::bench
