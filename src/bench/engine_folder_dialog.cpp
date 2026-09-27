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
namespace {

[[nodiscard]] std::string joined(const std::string& folder, const std::string& name) {
    return folder == "/" ? "/" + name : folder + "/" + name;
}

} // namespace

EngineFolderDialog::EngineFolderDialog(QString engine_name, Lister lister, std::string start,
                                       QWidget* parent)
    : QDialog(parent), lister_(std::move(lister)) {
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

    connect(up_, &QPushButton::clicked, this, [this] {
        if (shown_ && shown_->parent) {
            browse(*shown_->parent);
        }
    });
    connect(folders_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (shown_ && item != nullptr) {
            browse(joined(shown_->path, item->data(Qt::UserRole).toByteArray().toStdString()));
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!shown_) {
            return;
        }
        const auto chosen = choice();
        emit folderChosen(QByteArray{chosen.data(), static_cast<qsizetype>(chosen.size())});
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    browse(std::move(start));
}

std::string EngineFolderDialog::choice() const {
    if (!shown_) {
        return {};
    }
    const auto* selected = folders_->currentItem();
    return selected != nullptr && selected->isSelected()
               ? joined(shown_->path, selected->data(Qt::UserRole).toByteArray().toStdString())
               : shown_->path;
}

void EngineFolderDialog::browse(std::string path) {
    const auto request = ++request_;
    status_->setText(QStringLiteral("Listing folders…"));
    up_->setEnabled(false);
    choose_->setEnabled(false);
    const QPointer self{this};
    lister_(std::move(path), [self, request](core::Result<Listing> listed) {
        if (!self || request != self->request_) {
            return;
        }
        if (!listed) {
            self->status_->setText(displayText(listed.error().message));
            self->up_->setEnabled(self->shown_ && self->shown_->parent);
            self->choose_->setEnabled(self->shown_.has_value());
            return;
        }
        self->present(*listed);
    });
}

void EngineFolderDialog::present(const Listing& listing) {
    shown_ = listing;
    path_->setText(QString::fromStdString(core::display_raw_path(listing.path)));
    folders_->clear();
    for (const auto& name : listing.folders) {
        auto* item =
            new QListWidgetItem(QString::fromStdString(core::display_raw_path(name)), folders_);
        item->setData(Qt::UserRole, QByteArray{name.data(), static_cast<qsizetype>(name.size())});
    }
    status_->setText(listing.folders.empty() ? QStringLiteral("No folders here") : QString{});
    up_->setEnabled(listing.parent.has_value());
    choose_->setEnabled(true);
}

} // namespace trackknife::bench
