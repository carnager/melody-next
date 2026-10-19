// SPDX-License-Identifier: GPL-3.0-only
#include "bench/playlist_folder_widget.hpp"

#include "bench/engine_folder_dialog.hpp"
#include "workspace/playlist_folder_session.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>

#include <utility>

namespace trackknife::bench {

PlaylistFolderWidget::PlaylistFolderWidget(PlaylistFolderSession& session, QString engine_name,
                                           QWidget* parent)
    : QWidget(parent), session_(session), engine_name_(std::move(engine_name)),
      places_(new QButtonGroup(this)) {
    setObjectName(QStringLiteral("bench-playlist-folder"));
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setColumnStretch(1, 1);

    default_ = new QRadioButton(this);
    default_->setObjectName(QStringLiteral("bench-playlist-folder-default"));
    chosen_ = new QRadioButton(tr("A folder:"), this);
    chosen_->setObjectName(QStringLiteral("bench-playlist-folder-chosen"));
    chosen_path_ = new QLineEdit(this);
    chosen_path_->setObjectName(QStringLiteral("bench-playlist-folder-path"));
    chosen_path_->setReadOnly(true);
    chosen_path_->setPlaceholderText(tr("Choose a folder -- MPD's playlist_directory, say"));
    choose_ = new QPushButton(tr("Choose…"), this);
    choose_->setObjectName(QStringLiteral("bench-playlist-folder-choose"));
    places_->addButton(default_, 0);
    places_->addButton(chosen_, 1);
    style_ = new QComboBox(this);
    style_->setObjectName(QStringLiteral("bench-playlist-folder-style"));
    style_->addItem(tr("Relative to the music folder, as MPD names them"), QStringLiteral("root"));
    style_->addItem(tr("Relative to the playlist"), QStringLiteral("playlist"));
    style_->addItem(tr("Whole paths"), QStringLiteral("absolute"));
    auto* style_label = new QLabel(tr("New playlists name their tracks:"), this);
    style_label->setBuddy(style_);
    auto* note = new QLabel(tr("Every playlist file there is a playlist here, whoever put it "
                               "there; a playlist saved here is a file there. A file already "
                               "there keeps how it names its tracks."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-playlist-folder-status"));
    status_->setWordWrap(true);

    layout->addWidget(default_, 0, 0, 1, 3);
    layout->addWidget(chosen_, 1, 0);
    layout->addWidget(chosen_path_, 1, 1);
    layout->addWidget(choose_, 1, 2);
    layout->addWidget(style_label, 2, 0);
    layout->addWidget(style_, 2, 1, 1, 2);
    layout->addWidget(note, 3, 0, 1, 3);
    layout->addWidget(status_, 4, 0, 1, 3);

    connect(places_, &QButtonGroup::idClicked, this, [this](const int id) {
        if (id == 0) {
            session_.choose(std::string{});
            return;
        }
        if (session_.chosen().empty()) {
            chooseFolder();
            sync();
            return;
        }
        session_.choose(session_.chosen());
    });
    connect(choose_, &QPushButton::clicked, this, &PlaylistFolderWidget::chooseFolder);
    connect(style_, &QComboBox::activated, this, [this](const int index) {
        session_.choose(std::nullopt, style_->itemData(index).toString().toStdString());
    });
    connect(&session_, &PlaylistFolderSession::changed, this, &PlaylistFolderWidget::sync);
    sync();
    if (!session_.loaded()) {
        session_.load();
    }
}

void PlaylistFolderWidget::chooseFolder() {
    const auto start = session_.chosen();
    if (const auto& folders = session_.folders()) {
        auto* chooser = new EngineFolderDialog(engine_name_, folders, start, this);
        connect(chooser, &EngineFolderDialog::folderChosen, this, [this](const QByteArray& chosen) {
            session_.choose(
                std::string{chosen.constData(), static_cast<std::size_t>(chosen.size())});
        });
        chooser->show();
        return;
    }
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Keep the playlists in"),
        start.empty()
            ? QString{}
            : QFile::decodeName(QByteArray{start.data(), static_cast<qsizetype>(start.size())}),
        QFileDialog::ShowDirsOnly);
    if (selected.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(selected);
    session_.choose(std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
}

void PlaylistFolderWidget::sync() {
    const bool loaded = session_.loaded();
    {
        const QSignalBlocker blocker{places_};
        default_->setChecked(loaded && session_.chosen().empty());
        chosen_->setChecked(loaded && !session_.chosen().empty());
    }
    default_->setText(session_.defaultText().isEmpty()
                          ? tr("Playlists in the music folder (none yet: add a music folder)")
                          : tr("Playlists in the music folder: %1").arg(session_.defaultText()));
    chosen_path_->setText(session_.chosenText());
    {
        const QSignalBlocker blocker{style_};
        style_->setCurrentIndex(
            std::max(0, style_->findData(QString::fromStdString(session_.style()))));
    }
    const bool usable = loaded && !session_.busy();
    for (auto* widget : std::initializer_list<QWidget*>{default_, chosen_, choose_, style_}) {
        widget->setEnabled(usable);
    }
    status_->setText(session_.status());
    status_->setVisible(!session_.status().isEmpty());
}

} // namespace trackknife::bench
