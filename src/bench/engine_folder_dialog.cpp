// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_folder_dialog.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {
namespace {

constexpr int path_role = Qt::UserRole + 1;

} // namespace

EngineFolderDialog::EngineFolderDialog(QString engine_name, Lister lister, std::string start,
                                       QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("bench-engine-folder-dialog"));
    setWindowTitle(QStringLiteral("Choose a folder on %1").arg(engine_name));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);

    // Where: up, the path -- typed, Enter going there -- and its steps.
    auto* where = new QHBoxLayout;
    up_ = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogToParent),
                          QStringLiteral("Up"), this);
    up_->setObjectName(QStringLiteral("bench-engine-folder-up"));
    up_->setToolTip(QStringLiteral("The folder this is in (Backspace)"));
    where->addWidget(up_);
    path_ = new QLineEdit(this);
    path_->setObjectName(QStringLiteral("bench-engine-folder-path"));
    path_->setPlaceholderText(QStringLiteral("A path on %1").arg(engine_name));
    path_->setToolTip(
        QStringLiteral("Type or paste a path on %1, and press Enter to go there").arg(engine_name));
    where->addWidget(path_, 1);
    layout->addLayout(where);
    crumbs_ = new QHBoxLayout;
    crumbs_->setSpacing(0);
    layout->addLayout(crumbs_);

    // Places to start from, beside the folders here.
    auto* split = new QSplitter(Qt::Horizontal, this);
    places_ = new QListWidget(split);
    places_->setObjectName(QStringLiteral("bench-engine-folder-places"));
    places_->setAccessibleName(QStringLiteral("Places on %1").arg(engine_name));
    folders_ = new QListWidget(split);
    folders_->setObjectName(QStringLiteral("bench-engine-folder-list"));
    folders_->setAccessibleName(QStringLiteral("Folders"));
    split->addWidget(places_);
    split->addWidget(folders_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({160, 360});
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);

    auto* below = new QHBoxLayout;
    hidden_ = new QCheckBox(QStringLiteral("Show hidden folders"), this);
    hidden_->setObjectName(QStringLiteral("bench-engine-folder-hidden"));
    below->addWidget(hidden_);
    below->addStretch(1);
    make_ = new QPushButton(QStringLiteral("New folder…"), this);
    make_->setObjectName(QStringLiteral("bench-engine-folder-make"));
    make_->setVisible(static_cast<bool>(lister.make));
    below->addWidget(make_);
    layout->addLayout(below);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-engine-folder-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bench-engine-folder-buttons"));
    choose_ = buttons->addButton(QStringLiteral("Choose"), QDialogButtonBox::AcceptRole);
    choose_->setObjectName(QStringLiteral("bench-engine-folder-choose"));
    // Enter goes into a folder, and goes to a path typed; Choose is a click.
    choose_->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    layout->addWidget(buttons);
    resize(640, 460);

    session_ = new EngineFolderSession(std::move(lister), std::move(start), this);
    connect(up_, &QPushButton::clicked, session_, &EngineFolderSession::up);
    connect(folders_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { session_->open(folders_->row(item)); });
    connect(places_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        session_->browse(item->data(path_role).toByteArray().toStdString());
    });
    connect(path_, &QLineEdit::returnPressed, this, [this] {
        const auto typed = QFile::encodeName(path_->text().trimmed());
        if (!typed.isEmpty()) {
            session_->browse(typed.toStdString());
            folders_->setFocus();
        }
    });
    connect(hidden_, &QCheckBox::toggled, session_, &EngineFolderSession::setShowHidden);
    connect(make_, &QPushButton::clicked, this, &EngineFolderDialog::makeFolder);
    // Backspace in the folders goes out, as in a file manager.
    folders_->installEventFilter(this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!session_->canChoose()) {
            return;
        }
        const auto chosen = choice();
        emit folderChosen(QByteArray{chosen.data(), static_cast<qsizetype>(chosen.size())});
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(session_, &EngineFolderSession::changed, this, &EngineFolderDialog::sync);
    sync();
    folders_->setFocus();
}

std::string EngineFolderDialog::choice() const {
    const auto* selected = folders_->currentItem();
    return session_->choice(selected != nullptr && selected->isSelected() ? folders_->row(selected)
                                                                          : -1);
}

void EngineFolderDialog::browse(std::string path) { session_->browse(std::move(path)); }

bool EngineFolderDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == folders_ && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Backspace) {
        session_->up();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void EngineFolderDialog::makeFolder() {
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("New folder"),
                              QStringLiteral("A new folder in %1:").arg(session_->path()),
                              QLineEdit::Normal, {}, &accepted);
    if (accepted) {
        session_->make(name);
    }
}

void EngineFolderDialog::sync() {
    if (!path_->hasFocus() || path_->text().isEmpty()) {
        path_->setText(session_->path());
    }
    // The steps of the path, each going back there.
    while (auto* step = crumbs_->takeAt(0)) {
        delete step->widget();
        delete step;
    }
    const auto crumbs = session_->crumbs();
    for (std::size_t at = 0; at < crumbs.size(); ++at) {
        auto* step = new QToolButton(this);
        step->setObjectName(QStringLiteral("bench-engine-folder-crumb"));
        step->setText(crumbs[at].name);
        step->setAutoRaise(true);
        step->setCheckable(true);
        step->setChecked(at + 1 == crumbs.size());
        connect(step, &QToolButton::clicked, this,
                [this, path = crumbs[at].path] { session_->browse(path); });
        crumbs_->addWidget(step);
        if (at + 1 < crumbs.size() && at > 0) {
            auto* separator = new QLabel(QStringLiteral("›"), this);
            separator->setForegroundRole(QPalette::PlaceholderText);
            crumbs_->addWidget(separator);
        }
    }
    crumbs_->addStretch(1);

    // The places, kept as they are while they are the same.
    const auto places = session_->places();
    bool same = places_->count() == static_cast<int>(places.size());
    for (int row = 0; same && row < places_->count(); ++row) {
        same = places_->item(row)->data(path_role).toByteArray().toStdString() ==
               places[static_cast<std::size_t>(row)].path;
    }
    if (!same) {
        places_->clear();
        for (const auto& place : places) {
            auto* item = new QListWidgetItem(style()->standardIcon(place.path == "/"
                                                                       ? QStyle::SP_DriveHDIcon
                                                                       : QStyle::SP_DirHomeIcon),
                                             place.name, places_);
            if (place.name != tr("Home") && place.path != "/") {
                item->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
            }
            item->setToolTip(QString::fromStdString(core::display_raw_path(place.path)));
            item->setData(path_role, QByteArray::fromStdString(place.path));
        }
    }

    const auto names = session_->folders();
    const auto shown = [&] {
        if (folders_->count() != names.size()) {
            return false;
        }
        for (int row = 0; row < names.size(); ++row) {
            if (folders_->item(row)->text() != names.at(row)) {
                return false;
            }
        }
        return true;
    }();
    if (!shown) {
        folders_->clear();
        for (int row = 0; row < names.size(); ++row) {
            auto* item = new QListWidgetItem(style()->standardIcon(session_->linked(row)
                                                                       ? QStyle::SP_DirLinkIcon
                                                                       : QStyle::SP_DirIcon),
                                             names.at(row), folders_);
            if (session_->linked(row)) {
                item->setToolTip(QStringLiteral("A link to a folder"));
            }
        }
    }
    status_->setText(session_->status());
    status_->setVisible(!session_->status().isEmpty());
    up_->setEnabled(session_->canGoUp());
    make_->setEnabled(session_->canMake());
    choose_->setEnabled(session_->canChoose());
}

} // namespace trackknife::bench
