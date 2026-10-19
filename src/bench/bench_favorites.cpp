// SPDX-License-Identifier: GPL-3.0-only

// ADR-0297: the playing track made a favourite -- into the Favorites
// playlist of the engine playing it -- and, when asked, loved on Last.fm.

#include "bench/bench_main_window.hpp"
#include "bench/lastfm_service.hpp"

#include <QAction>
#include <QFile>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QSettings>
#include <QToolButton>

namespace trackknife::bench {

void BenchMainWindow::buildFavorite(QWidget* header, QHBoxLayout* layout) {
    favorite_action_ = new QAction(QStringLiteral("Favourite"), this);
    favorite_action_->setObjectName(QStringLiteral("action-favorite"));
    favorite_action_->setCheckable(true);
    favorite_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+H")));
    favorite_action_->setToolTip(
        QStringLiteral("Add what plays to the Favorites playlist, or take it out"));
    favorite_action_->setEnabled(false);
    // The check follows the engine's answer, not the click.
    connect(favorite_action_, &QAction::triggered, this, [this] {
        favorite_action_->setChecked(!favorite_action_->isChecked());
        toggleFavorite();
    });
    favorite_button_ = new QToolButton(header);
    favorite_button_->setObjectName(QStringLiteral("bench-favorite"));
    favorite_button_->setAutoRaise(true);
    favorite_button_->setDefaultAction(favorite_action_);
    favorite_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto font = favorite_button_->font();
    font.setPointSizeF(font.pointSizeF() * 1.3);
    favorite_button_->setFont(font);
    favorite_button_->hide();
    showFavorite(false);
    layout->addWidget(favorite_button_);
}

void BenchMainWindow::showFavorite(const bool favorite) {
    favorite_action_->setChecked(favorite);
    favorite_action_->setText(favorite ? QStringLiteral("♥") : QStringLiteral("♡"));
    favorite_action_->setIconText(favorite_action_->text());
    favorite_button_->setToolTip(favorite ? QStringLiteral("A favourite: in the Favorites "
                                                           "playlist (Ctrl+Shift+H takes it out)")
                                          : QStringLiteral("Make it a favourite: into the "
                                                           "Favorites playlist (Ctrl+Shift+H)"));
    favorite_button_->setAccessibleName(favorite ? QStringLiteral("Favourite")
                                                 : QStringLiteral("Not a favourite"));
}

void BenchMainWindow::refreshFavorite(const EnginePlayback::State& state) {
    const bool stopped = state.status == QStringLiteral("stopped") || state.entry.isEmpty();
    const auto entry = stopped ? QString{} : state.entry;
    if (entry == favorite_entry_) {
        return;
    }
    favorite_entry_ = entry;
    const auto raw = state.path.toLocal8Bit();
    favorite_path_ = entry.isEmpty()
                         ? std::string{}
                         : std::string{raw.constData(), static_cast<std::size_t>(raw.size())};
    favorite_button_->setVisible(!entry.isEmpty());
    favorite_action_->setEnabled(!entry.isEmpty());
    showFavorite(false);
    if (entry.isEmpty() || transport_ == nullptr || favorite_path_.empty()) {
        return;
    }
    transport_->request(
        QStringLiteral("favorites.state"),
        protocol::Json{
            {"paths", protocol::Json::array({protocol::encode_raw_path(favorite_path_)})}},
        [this, entry](core::Result<protocol::Json> answer) {
            if (entry != favorite_entry_) {
                return;
            }
            // An engine older than favourites has none to offer.
            if (!answer) {
                favorite_button_->setVisible(answer.error().code != core::ErrorCode::unsupported);
                return;
            }
            const auto said = answer->value("favorites", protocol::Json::array());
            showFavorite(!said.empty() && said.front().is_boolean() && said.front().get<bool>());
        });
}

void BenchMainWindow::toggleFavorite() {
    if (transport_ == nullptr || favorite_entry_.isEmpty() || favorite_path_.empty()) {
        return;
    }
    // Named before it is asked: the track may change on the way.
    QString artist;
    QString title;
    if (const auto* row = playingRow(favorite_entry_)) {
        artist = QString::fromStdString(row->artist);
        title = QString::fromStdString(row->title);
    }
    favorite_action_->setEnabled(false);
    transport_->request(
        QStringLiteral("favorites.toggle"),
        protocol::Json{{"path", protocol::encode_raw_path(favorite_path_)}},
        [this, entry = favorite_entry_, artist, title](core::Result<protocol::Json> answer) {
            favorite_action_->setEnabled(!favorite_entry_.isEmpty());
            if (!answer) {
                showMessage(answer.error().code == core::ErrorCode::unsupported
                                ? QStringLiteral("This engine is too old for favourites: update "
                                                 "melodyd")
                                : QStringLiteral("Not changed: %1")
                                      .arg(QString::fromStdString(answer.error().message)),
                            6'000);
                return;
            }
            const bool favorite = answer->value("favorite", false);
            if (entry == favorite_entry_) {
                showFavorite(favorite);
            }
            showMessage(favorite ? QStringLiteral("Added to Favorites")
                                 : QStringLiteral("Taken out of Favorites"),
                        3'000);
            // Loved on Last.fm too, when asked and signed in.
            auto* lastfm = workspace_.lastfm_;
            if (lastfm != nullptr && !artist.isEmpty() && !title.isEmpty() &&
                QSettings{}.value(QStringLiteral("lastfm/love-favorites"), false).toBool() &&
                !workspace_.lastfm_user_.isEmpty()) {
                lastfm->execute(favorite ? QStringLiteral("love") : QStringLiteral("unlove"),
                                {artist, title});
            }
        });
}

} // namespace trackknife::bench
