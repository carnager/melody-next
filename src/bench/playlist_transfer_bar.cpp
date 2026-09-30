// SPDX-License-Identifier: GPL-3.0-only
#include "bench/playlist_transfer_bar.hpp"

#include <QAction>
#include <QLabel>

namespace trackknife::bench {
PlaylistTransferBar::PlaylistTransferBar(QWidget* parent)
    : QToolBar(tr("Portable playlist"), parent), transfer_(new PlaylistTransfer(this)) {
    setObjectName(QStringLiteral("bench-playlist-transfer"));
    setMovable(false);
    setFloatable(false);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-playlist-transfer-status"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    addWidget(status_);
    cancel_action_ = addAction(tr("Cancel"));
    cancel_action_->setObjectName(QStringLiteral("action-cancel-playlist-transfer"));
    connect(cancel_action_, &QAction::triggered, transfer_, &PlaylistTransfer::dismiss);
    connect(transfer_, &PlaylistTransfer::changed, this, [this] {
        status_->setText(transfer_->status());
        cancel_action_->setText(transfer_->active() ? tr("Cancel") : tr("Close"));
        setVisible(transfer_->shown());
    });
    connect(transfer_, &PlaylistTransfer::imported, this, &PlaylistTransferBar::imported);
    connect(transfer_, &PlaylistTransfer::completed, this, &PlaylistTransferBar::completed);
    hide();
}
PlaylistTransferBar::~PlaylistTransferBar() { stop(); }
} // namespace trackknife::bench
