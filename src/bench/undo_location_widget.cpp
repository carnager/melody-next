// SPDX-License-Identifier: GPL-3.0-only
#include "bench/undo_location_widget.hpp"

#include "bench/engine_folder_dialog.hpp"
#include "workspace/undo_location_session.hpp"

#include <QButtonGroup>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {

UndoLocationWidget::UndoLocationWidget(UndoLocationSession& session, QString engine_name,
                                       QWidget* parent)
    : QWidget(parent), session_(session), engine_name_(std::move(engine_name)),
      places_(new QButtonGroup(this)) {
    setObjectName(QStringLiteral("bench-undo-location"));
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setColumnStretch(1, 1);

    engine_ = new QRadioButton(tr("The engine's own folder"), this);
    engine_->setObjectName(QStringLiteral("bench-undo-location-engine"));
    engine_folder_ = new QLabel(this);
    engine_folder_->setObjectName(QStringLiteral("bench-undo-location-engine-folder"));
    engine_folder_->setForegroundRole(QPalette::PlaceholderText);
    engine_folder_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folder_ = new QRadioButton(tr("A folder:"), this);
    folder_->setObjectName(QStringLiteral("bench-undo-location-folder"));
    folder_path_ = new QLineEdit(this);
    folder_path_->setObjectName(QStringLiteral("bench-undo-location-folder-path"));
    folder_path_->setReadOnly(true);
    folder_path_->setPlaceholderText(tr("Choose a folder"));
    choose_ = new QPushButton(tr("Choose…"), this);
    choose_->setObjectName(QStringLiteral("bench-undo-location-choose"));
    beside_ = new QRadioButton(tr("Beside each file"), this);
    beside_->setObjectName(QStringLiteral("bench-undo-location-beside"));
    places_->addButton(engine_, static_cast<int>(UndoLocationSession::Place::engine));
    places_->addButton(folder_, static_cast<int>(UndoLocationSession::Place::folder));
    places_->addButton(beside_, static_cast<int>(UndoLocationSession::Place::beside));

    layout->addWidget(engine_, 0, 0);
    layout->addWidget(engine_folder_, 0, 1, 1, 2);
    layout->addWidget(folder_, 1, 0);
    layout->addWidget(folder_path_, 1, 1);
    layout->addWidget(choose_, 1, 2);
    layout->addWidget(beside_, 2, 0, 1, 3);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("bench-undo-location-note"));
    note_->setWordWrap(true);
    note_->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note_, 3, 0, 1, 3);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-undo-location-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_, 4, 0, 1, 3);

    connect(places_, &QButtonGroup::idClicked, this, [this](const int id) {
        const auto place = static_cast<UndoLocationSession::Place>(id);
        if (place == UndoLocationSession::Place::folder) {
            if (session_.folder().empty()) {
                chooseFolder();
                sync();
                return;
            }
            session_.choose(place, session_.folder());
            return;
        }
        session_.choose(place);
    });
    connect(choose_, &QPushButton::clicked, this, &UndoLocationWidget::chooseFolder);
    connect(&session_, &UndoLocationSession::changed, this, &UndoLocationWidget::sync);
    sync();
    if (!session_.loaded()) {
        session_.load();
    }
}

void UndoLocationWidget::chooseFolder() {
    const auto start = session_.folder();
    if (const auto& folders = session_.folders()) {
        auto* chooser = new EngineFolderDialog(engine_name_, folders, start, this);
        connect(chooser, &EngineFolderDialog::folderChosen, this, [this](const QByteArray& chosen) {
            session_.choose(UndoLocationSession::Place::folder,
                            std::string{chosen.constData(), static_cast<std::size_t>(chosen.size())});
        });
        chooser->show();
        return;
    }
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Keep undo copies in"),
        start.empty() ? QString{}
                      : QFile::decodeName(QByteArray{start.data(),
                                                     static_cast<qsizetype>(start.size())}),
        QFileDialog::ShowDirsOnly);
    if (selected.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(selected);
    session_.choose(UndoLocationSession::Place::folder,
                    std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
}

void UndoLocationWidget::sync() {
    {
        const QSignalBlocker blocker{places_};
        if (auto* button = places_->button(static_cast<int>(session_.place()))) {
            button->setChecked(session_.loaded());
        }
    }
    const bool usable = session_.loaded() && !session_.busy();
    for (auto* widget : std::initializer_list<QWidget*>{engine_, folder_, beside_, choose_}) {
        widget->setEnabled(usable);
    }
    folder_path_->setText(session_.folderText());
    engine_folder_->setText(session_.place() == UndoLocationSession::Place::engine
                                ? session_.keptIn()
                                : QString{});
    switch (session_.place()) {
    case UndoLocationSession::Place::beside:
        note_->setText(tr("A hidden copy of each file written stays in its folder until it is "
                          "let go, and goes wherever that folder is copied or backed up."));
        break;
    case UndoLocationSession::Place::folder:
        note_->setText(tr("On the same filesystem as the music, a copy is a link and costs no "
                          "space; elsewhere it is a full copy."));
        break;
    case UndoLocationSession::Place::engine:
        note_->setText(tr("Outside every library. Music on another filesystem is copied there "
                          "in full when it is written."));
        break;
    }
    note_->setVisible(session_.loaded());
    status_->setText(session_.status());
    status_->setVisible(!session_.status().isEmpty());
}

} // namespace trackknife::bench
