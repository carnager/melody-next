// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_folder_dialog.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {
EngineFolderDialog::EngineFolderDialog(QString engine_name, Lister lister, std::string start,
                                       QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("bench-engine-folder-dialog"));
    setWindowTitle(QStringLiteral("Choose a folder on %1").arg(engine_name));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    auto* where = new QHBoxLayout;
    up_ = new QPushButton(QStringLiteral("Up"), this);
    up_->setObjectName(QStringLiteral("bench-engine-folder-up"));
    where->addWidget(up_);
    path_ = new QLabel(this);
    path_->setObjectName(QStringLiteral("bench-engine-folder-path"));
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    where->addWidget(path_, 1);
    layout->addLayout(where);
    folders_ = new QListWidget(this);
    folders_->setObjectName(QStringLiteral("bench-engine-folder-list"));
    layout->addWidget(folders_, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-engine-folder-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bench-engine-folder-buttons"));
    choose_ = buttons->addButton(QStringLiteral("Choose"), QDialogButtonBox::AcceptRole);
    choose_->setObjectName(QStringLiteral("bench-engine-folder-choose"));
    layout->addWidget(buttons);
    resize(520, 420);

    session_ = new EngineFolderSession(std::move(lister), std::move(start), this);
    connect(up_, &QPushButton::clicked, session_, &EngineFolderSession::up);
    connect(folders_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { session_->open(folders_->row(item)); });
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
}

std::string EngineFolderDialog::choice() const {
    const auto* selected = folders_->currentItem();
    return session_->choice(selected != nullptr && selected->isSelected() ? folders_->row(selected)
                                                                          : -1);
}

void EngineFolderDialog::browse(std::string path) { session_->browse(std::move(path)); }

void EngineFolderDialog::sync() {
    path_->setText(session_->path());
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
        folders_->addItems(names);
    }
    status_->setText(session_->status());
    up_->setEnabled(session_->canGoUp());
    choose_->setEnabled(session_->canChoose());
}

} // namespace trackknife::bench
