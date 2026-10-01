// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_settings.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "workspace/workspace.hpp"

#include <QFile>
#include <QKeySequence>

#include <algorithm>
#include <utility>

namespace trackknife::quick {
namespace {

template <typename Choices>
[[nodiscard]] QVariantList choiceList(const Choices& choices) {
    QVariantList list;
    for (const auto& choice : choices) {
        list.append(QVariantMap{{QStringLiteral("label"), choice.label},
                                {QStringLiteral("value"), choice.value}});
    }
    return list;
}

} // namespace

QuickEngineFolder::QuickEngineFolder(const QString& engine_name, bench::EngineFolderLister lister,
                                     std::string start, QObject* parent)
    : QObject(parent), title_(tr("Choose a folder on %1").arg(engine_name)),
      session_(new bench::EngineFolderSession(std::move(lister), std::move(start), this)) {
    connect(session_, &bench::EngineFolderSession::changed, this, &QuickEngineFolder::changed);
}

QVariantMap QuickEngineFolder::state() const {
    return {{QStringLiteral("path"), session_->path()},
            {QStringLiteral("folders"), session_->folders()},
            {QStringLiteral("status"), session_->status()},
            {QStringLiteral("canUp"), session_->canGoUp()},
            {QStringLiteral("canChoose"), session_->canChoose()}};
}

void QuickEngineFolder::choose(const int row) {
    if (!session_->canChoose()) {
        return;
    }
    const auto chosen_path = session_->choice(row);
    emit chosen(QByteArray{chosen_path.data(), static_cast<qsizetype>(chosen_path.size())});
}

QuickSettings::QuickSettings(bench::Workspace& work, bench::OutputProfileStore profiles,
                             QObject* parent)
    : QObject(parent), work_(work), sharing_before_(bench::localEngineSharing()),
      session_(new bench::SettingsSession(this)),
      profiles_(new bench::ProfilesSession(std::move(profiles), this)),
      lastfm_(new bench::LastFmSettingsSession(work, this)) {
    connect(session_, &bench::SettingsSession::changed, this, &QuickSettings::changed);
    connect(session_, &bench::SettingsSession::enginesChanged, this, &QuickSettings::changed);
    connect(session_, &bench::SettingsSession::foundChanged, this, &QuickSettings::changed);
    connect(profiles_, &bench::ProfilesSession::changed, this, &QuickSettings::profilesChanged);
    connect(profiles_, &bench::ProfilesSession::listsChanged, this,
            &QuickSettings::profilesChanged);
    connect(profiles_, &bench::ProfilesSession::profilesChanged, this,
            &QuickSettings::outputProfilesChanged);
    connect(lastfm_, &bench::LastFmSettingsSession::changed, this, &QuickSettings::lastFmChanged);
    connect(lastfm_, &bench::LastFmSettingsSession::enginesChanged, this,
            &QuickSettings::lastFmChanged);
    // Settings shows the reused key too, so its Save cannot overwrite it.
    connect(lastfm_, &bench::LastFmSettingsSession::keyReused, this,
            [this](const QString& key) { session_->setValue(QStringLiteral("lastfm/api-key"), key); });
}

QVariantMap QuickSettings::options() {
    return {
        {QStringLiteral("pages"), bench::SettingsSession::pageTitles()},
        {QStringLiteral("listsDisplays"), choiceList(bench::SettingsSession::listsDisplays())},
        {QStringLiteral("bufferProfiles"), choiceList(bench::SettingsSession::bufferProfiles())},
        {QStringLiteral("ratingScales"), choiceList(bench::SettingsSession::ratingScales())},
        {QStringLiteral("colorSchemes"), choiceList(bench::SettingsSession::colorSchemes())},
        {QStringLiteral("fetchSources"), choiceList(bench::SettingsSession::fetchSources())},
        {QStringLiteral("folderImageNames"), bench::SettingsSession::folderImageNames()},
        {QStringLiteral("maximumPreamp"), bench::SettingsSession::maximumPreamp()},
        {QStringLiteral("sanitizationPolicies"),
         choiceList(bench::ProfilesSession::sanitizationPolicies())},
    };
}

QString QuickSettings::saveNote(const int page) {
    return bench::SettingsSession::saveNote(static_cast<bench::SettingsSession::Page>(page));
}

void QuickSettings::testNotification() {
    if (notifier_ == nullptr) {
        notifier_ = new bench::DesktopNotifier(this);
        connect(notifier_, &bench::DesktopNotifier::deliveryFinished, this,
                [this](const QString& error) {
                    notification_status_ =
                        error.isEmpty()
                            ? tr("Accepted by your desktop. If no popup appears, check Do Not "
                                 "Disturb and desktop notification rules.")
                            : tr("Notification failed: %1").arg(error);
                    emit changed();
                });
    }
    notification_status_ = tr("Sending…");
    emit changed();
    notifier_->sendTest();
}

QVariantMap QuickSettings::state() const {
    const auto engine = session_->engine();
    QVariantList found;
    for (const auto& announced : session_->found()) {
        found.append(QVariantMap{{QStringLiteral("label"), announced.label},
                                 {QStringLiteral("address"), announced.address}});
    }
    return {
        {QStringLiteral("values"), session_->values()},
        {QStringLiteral("bufferCustom"), session_->bufferCustom()},
        {QStringLiteral("agentCommand"), session_->agentCommand()},
        {QStringLiteral("nearbyRates"), choiceList(session_->nearbyRates())},
        {QStringLiteral("awayRates"), choiceList(session_->awayRates())},
        {QStringLiteral("engineRates"), choiceList(session_->engineRates())},
        {QStringLiteral("engineLabels"), session_->engineLabels()},
        {QStringLiteral("currentEngine"), session_->currentEngine()},
        {QStringLiteral("engine"),
         QVariantMap{{QStringLiteral("address"), engine.address},
                     {QStringLiteral("password"), engine.password},
                     {QStringLiteral("musicFolder"), engine.music_folder},
                     {QStringLiteral("reachableAt"), engine.reachable_at},
                     {QStringLiteral("streamKbps"), engine.stream_kbps < 0 ? -1 : engine.stream_kbps}}},
        {QStringLiteral("found"), found},
        {QStringLiteral("discoveryError"), session_->discoveryError()},
        {QStringLiteral("notificationStatus"), notification_status_},
    };
}

void QuickSettings::editEngine(const QVariantMap& edited) {
    auto engine = session_->engine();
    engine.address = edited.value(QStringLiteral("address"), engine.address).toString();
    engine.password = edited.value(QStringLiteral("password"), engine.password).toString();
    engine.music_folder =
        edited.value(QStringLiteral("musicFolder"), engine.music_folder).toString();
    engine.reachable_at =
        edited.value(QStringLiteral("reachableAt"), engine.reachable_at).toString();
    engine.stream_kbps = edited.value(QStringLiteral("streamKbps"), engine.stream_kbps).toInt();
    session_->editEngine(engine);
}

int QuickSettings::save() {
    if (shortcuts_ != nullptr && !shortcuts_->apply()) {
        return static_cast<int>(bench::SettingsSession::Page::shortcuts);
    }
    if (const auto refused = session_->save()) {
        return static_cast<int>(*refused);
    }
    work_.settingsSaved(sharing_before_);
    emit saved();
    return -1;
}

QVariantMap QuickSettings::profiles() const {
    return {
        {QStringLiteral("layouts"), profiles_->layoutNames()},
        {QStringLiteral("destinations"), profiles_->destinationNames()},
        {QStringLiteral("places"), profiles_->placeNames()},
        {QStringLiteral("layoutRow"), profiles_->layoutRow()},
        {QStringLiteral("destinationRow"), profiles_->destinationRow()},
        {QStringLiteral("place"), profiles_->place()},
        {QStringLiteral("placeName"), profiles_->placeName()},
        {QStringLiteral("layoutName"), profiles_->layoutName()},
        {QStringLiteral("directoryExpression"), profiles_->directoryExpression()},
        {QStringLiteral("basenameExpression"), profiles_->basenameExpression()},
        {QStringLiteral("sanitization"), profiles_->sanitization()},
        {QStringLiteral("destinationName"), profiles_->destinationName()},
        {QStringLiteral("destinationRoot"), profiles_->destinationRoot()},
        {QStringLiteral("status"), profiles_->status()},
        {QStringLiteral("copyable"), profiles_->copyable()},
        {QStringLiteral("available"), profiles_->available()},
        {QStringLiteral("canChoosePlace"), profiles_->canChoosePlace()},
        {QStringLiteral("canEditLayouts"), profiles_->canEditLayouts()},
        {QStringLiteral("canSaveLayout"), profiles_->canSaveLayout()},
        {QStringLiteral("canRemoveLayout"), profiles_->canRemoveLayout()},
        {QStringLiteral("canEditDestinations"), profiles_->canEditDestinations()},
        {QStringLiteral("canSaveDestination"), profiles_->canSaveDestination()},
        {QStringLiteral("canRemoveDestination"), profiles_->canRemoveDestination()},
    };
}

void QuickSettings::chooseLocalRoot(const QUrl& folder) {
    const auto encoded = QFile::encodeName(folder.toLocalFile());
    if (!encoded.isEmpty()) {
        profiles_->chooseRoot(std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
    }
}

QuickEngineFolder* QuickSettings::browseRoot() {
    const auto folders = profiles_->folders();
    if (!folders) {
        return nullptr;
    }
    auto* chooser =
        new QuickEngineFolder(profiles_->placeName(), folders, profiles_->destinationRootRawPath(), this);
    connect(chooser, &QuickEngineFolder::chosen, profiles_, [this](const QByteArray& chosen) {
        profiles_->chooseRoot(std::string{chosen.constData(), static_cast<std::size_t>(chosen.size())});
    });
    QQmlEngine::setObjectOwnership(chooser, QQmlEngine::CppOwnership);
    return chooser;
}

QVariantMap QuickSettings::lastFm() const {
    QVariantList engines;
    for (int row = 0; const auto& engine : lastfm_->engines()) {
        engines.append(QVariantMap{{QStringLiteral("name"), engine.name},
                                   {QStringLiteral("id"), engine.id},
                                   {QStringLiteral("state"), engine.state},
                                   {QStringLiteral("inUse"), lastfm_->inUse(row)}});
        ++row;
    }
    return {
        {QStringLiteral("key"), lastfm_->key()},
        {QStringLiteral("secret"), lastfm_->secret()},
        {QStringLiteral("reuseKey"), lastfm_->reuseKey()},
        {QStringLiteral("credentialsSaved"), lastfm_->credentialsSaved()},
        {QStringLiteral("connectText"), lastfm_->connectText()},
        {QStringLiteral("canConnect"), lastfm_->canConnect()},
        {QStringLiteral("waiting"), lastfm_->waiting()},
        {QStringLiteral("connected"), lastfm_->connected()},
        {QStringLiteral("scrobbling"), lastfm_->scrobbling()},
        {QStringLiteral("status"), lastfm_->status()},
        {QStringLiteral("engines"), engines},
    };
}

void QuickSettings::loadShortcuts(const QVariantList& commands) {
    if (shortcuts_ != nullptr) {
        return;
    }
    std::vector<bench::ShortcutSession::Command> listed;
    for (const auto& entry : commands) {
        const auto command = entry.toMap();
        const auto id = command.value(QStringLiteral("id")).toString();
        const QKeySequence default_key(command.value(QStringLiteral("key")).toString(),
                                       QKeySequence::PortableText);
        // Every command with a key by default, and every workspace command
        // whether it has one or not.
        if (default_key.isEmpty() &&
            std::ranges::none_of(bench::workspace_command_ids, [&id](const char* command_id) {
                return id == QLatin1String(command_id);
            })) {
            continue;
        }
        auto label = command.value(QStringLiteral("label")).toString();
        label.remove(QLatin1Char('&'));
        listed.push_back({id, label, bench::ShortcutSession::saved(id, default_key), default_key});
    }
    std::ranges::sort(listed, [](const auto& a, const auto& b) {
        return a.label.localeAwareCompare(b.label) < 0;
    });
    shortcuts_ = new bench::ShortcutSession(std::move(listed), this);
    connect(shortcuts_, &bench::ShortcutSession::changed, this, &QuickSettings::shortcutsChanged);
    emit shortcutsChanged();
}

void QuickSettings::setShortcut(const int row, const QString& key) {
    if (shortcuts_ != nullptr) {
        shortcuts_->setKey(row, QKeySequence(key, QKeySequence::PortableText));
    }
}

void QuickSettings::restoreShortcuts() {
    if (shortcuts_ != nullptr) {
        shortcuts_->restoreDefaults();
    }
}

QVariantMap QuickSettings::shortcuts() const {
    QVariantList commands;
    if (shortcuts_ != nullptr) {
        for (const auto& command : shortcuts_->commands()) {
            commands.append(QVariantMap{
                {QStringLiteral("id"), command.id},
                {QStringLiteral("label"), command.label},
                {QStringLiteral("key"), command.key.toString(QKeySequence::PortableText)},
                {QStringLiteral("shown"), command.key.toString(QKeySequence::NativeText)}});
        }
    }
    return {{QStringLiteral("commands"), commands},
            {QStringLiteral("error"), shortcuts_ != nullptr ? shortcuts_->error() : QString{}}};
}

QString QuickSettings::keyText(const int key, const int modifiers) {
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_unknown:
        return {};
    default:
        break;
    }
    const auto chord = QKeyCombination(
        static_cast<Qt::KeyboardModifiers>(modifiers) &
            (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier),
        static_cast<Qt::Key>(key));
    return QKeySequence(chord).toString(QKeySequence::PortableText);
}

QString QuickSettings::nativeKeyText(const QString& portable) {
    return QKeySequence(portable, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

} // namespace trackknife::quick
