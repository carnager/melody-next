// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/desktop_notifier.hpp"
#include "bench/engine_launcher.hpp"
#include "workspace/engine_folder_session.hpp"
#include "workspace/lastfm_settings_session.hpp"
#include "workspace/profiles_session.hpp"
#include "workspace/settings_session.hpp"
#include "workspace/shortcut_session.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace trackknife::bench {
class Workspace;
}

namespace trackknife::quick {

// Choosing a folder on an engine's machine: an EngineFolderSession, as QML
// draws it.
class QuickEngineFolder final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the settings")
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)

  public:
    QuickEngineFolder(const QString& engine_name, bench::EngineFolderLister lister,
                      std::string start, QObject* parent = nullptr);

    [[nodiscard]] QString title() const { return title_; }
    [[nodiscard]] QVariantMap state() const;

    Q_INVOKABLE void open(int row) { session_->open(row); }
    Q_INVOKABLE void up() { session_->up(); }
    // The folder shown, or `row` in it, taken.
    Q_INVOKABLE void choose(int row);
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void chosen(const QByteArray& raw_path);

  private:
    QString title_;
    bench::EngineFolderSession* session_;
};

// The Settings window in Qt Quick: the settings' draft, the naming layouts
// and move destinations, Last.fm and the keyboard shortcuts, each a
// workspace session; as QML draws them.
class QuickSettings final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(QVariantMap options READ options CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantMap profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QVariantMap lastFm READ lastFm NOTIFY lastFmChanged)
    Q_PROPERTY(QVariantMap shortcuts READ shortcuts NOTIFY shortcutsChanged)

  public:
    QuickSettings(bench::Workspace& work, bench::OutputProfileStore profiles,
                  QObject* parent = nullptr);

    [[nodiscard]] static QVariantMap options();
    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QVariantMap profiles() const;
    [[nodiscard]] QVariantMap lastFm() const;
    [[nodiscard]] QVariantMap shortcuts() const;

    Q_INVOKABLE static QString saveNote(int page);
    // A notification sent to see that the desktop shows it; what came of
    // it in state.notificationStatus.
    Q_INVOKABLE void testNotification();

    // The draft.
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value) {
        session_->setValue(key, value);
    }
    Q_INVOKABLE void selectEngine(int row) { session_->selectEngine(row); }
    Q_INVOKABLE void editEngine(const QVariantMap& engine);
    Q_INVOKABLE void addEngine() { session_->addEngine(); }
    Q_INVOKABLE void removeEngine() { session_->removeEngine(); }
    Q_INVOKABLE void chooseFoundEngine(const QString& address) {
        session_->chooseFoundEngine(address);
    }
    // Written, and everything that follows them told: -1. Otherwise the
    // page whose value is refused, nothing written.
    Q_INVOKABLE int save();

    // Naming layouts and move destinations.
    Q_INVOKABLE void selectLayout(int row) { profiles_->selectLayout(row); }
    Q_INVOKABLE void newLayout() { profiles_->newLayout(); }
    Q_INVOKABLE void setLayoutName(const QString& text) { profiles_->setLayoutName(text); }
    Q_INVOKABLE void setDirectoryExpression(const QString& text) {
        profiles_->setDirectoryExpression(text);
    }
    Q_INVOKABLE void setBasenameExpression(const QString& text) {
        profiles_->setBasenameExpression(text);
    }
    Q_INVOKABLE void setSanitization(const QString& policy) { profiles_->setSanitization(policy); }
    Q_INVOKABLE void saveLayout() { profiles_->saveLayout(); }
    Q_INVOKABLE void removeLayout() { profiles_->removeLayout(); }
    Q_INVOKABLE void selectPlace(int index) { profiles_->selectPlace(index); }
    Q_INVOKABLE int placeOf(const QString& key) const { return profiles_->placeOf(key); }
    Q_INVOKABLE void selectDestination(int row) { profiles_->selectDestination(row); }
    Q_INVOKABLE void newDestination() { profiles_->newDestination(); }
    Q_INVOKABLE void setDestinationName(const QString& text) {
        profiles_->setDestinationName(text);
    }
    // A root on this computer, from its folder dialog.
    Q_INVOKABLE void chooseLocalRoot(const QUrl& folder);
    // The root chosen on the engine's machine; null for this computer,
    // whose folder dialog is used.
    Q_INVOKABLE trackknife::quick::QuickEngineFolder* browseRoot();
    Q_INVOKABLE void saveDestination() { profiles_->saveDestination(); }
    Q_INVOKABLE void removeDestination() { profiles_->removeDestination(); }
    Q_INVOKABLE void copyDestinations() { profiles_->copyDestinations(); }

    // Last.fm.
    Q_INVOKABLE void setLastFmKey(const QString& key) { lastfm_->setKey(key); }
    Q_INVOKABLE void setLastFmSecret(const QString& secret) { lastfm_->setSecret(secret); }
    Q_INVOKABLE void setLastFmReuseKey(bool on) { lastfm_->setReuseKey(on); }
    Q_INVOKABLE void connectLastFm() { lastfm_->connectAccount(); }
    Q_INVOKABLE void stopWaitingForLastFm() { lastfm_->stopWaiting(); }
    Q_INVOKABLE void disconnectLastFm() { lastfm_->disconnectAccount(); }
    Q_INVOKABLE void setScrobbling(bool on) { lastfm_->setScrobbling(on); }
    Q_INVOKABLE void useLastFmAccount(int row) { lastfm_->useAccount(row); }
    // The error, if it is not an engine's address.
    Q_INVOKABLE QString addLastFmEngine(const QString& address, const QString& password) {
        return lastfm_->addEngine(address, password);
    }

    // Keyboard shortcuts: the window's commands, as its menus hold them --
    // {id, label, key}, the key as the menu gives it by default.
    Q_INVOKABLE void loadShortcuts(const QVariantList& commands);
    Q_INVOKABLE void setShortcut(int row, const QString& key);
    Q_INVOKABLE void restoreShortcuts();
    // A key press, as a shortcut's portable text; empty for a modifier
    // alone.
    Q_INVOKABLE static QString keyText(int key, int modifiers);
    Q_INVOKABLE static QString nativeKeyText(const QString& portable);

    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void profilesChanged();
    void lastFmChanged();
    void shortcutsChanged();
    // Saved layouts or destinations changed: the tag editors follow.
    void outputProfilesChanged();
    // Written: the window follows.
    void saved();

  private:
    bench::Workspace& work_;
    bench::LocalEngineSharing sharing_before_;
    bench::SettingsSession* session_;
    bench::ProfilesSession* profiles_;
    bench::LastFmSettingsSession* lastfm_;
    bench::ShortcutSession* shortcuts_{nullptr};
    bench::DesktopNotifier* notifier_{nullptr};
    QString notification_status_;
};

} // namespace trackknife::quick
