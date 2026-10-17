// SPDX-License-Identifier: GPL-3.0-only

#include "bench/plugin_list_window.hpp"

#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace trackknife::bench {

PluginListWindow::PluginListWindow(const QString& plugin, const PluginList& list, QWidget* parent)
    : QDialog(parent), list_(list) {
    setObjectName(QStringLiteral("bench-plugin-list"));
    setWindowTitle(plugin);
    setModal(false);
    resize(520, 420);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(list.title.isEmpty() ? plugin : list.title, this);
    heading->setObjectName(QStringLiteral("bench-plugin-list-title"));
    auto font = heading->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.15);
    heading->setFont(font);
    heading->setWordWrap(true);
    layout->addWidget(heading);

    rows_ = new QListWidget(this);
    rows_->setObjectName(QStringLiteral("bench-plugin-list-rows"));
    rows_->setWordWrap(true);
    rows_->setAlternatingRowColors(true);
    for (const auto& item : list.items) {
        auto* row = new QListWidgetItem(
            item.detail.isEmpty() ? item.text : item.text + QLatin1Char('\n') + item.detail, rows_);
        if (!item.url.isEmpty()) {
            row->setToolTip(item.url);
            row->setData(Qt::UserRole, item.url);
        }
    }
    if (list.items.empty()) {
        rows_->addItem(QStringLiteral("Nothing"));
        rows_->item(0)->setFlags(Qt::NoItemFlags);
    }
    connect(rows_, &QListWidget::itemActivated, this, [](QListWidgetItem* row) {
        if (const auto url = row->data(Qt::UserRole).toString(); !url.isEmpty()) {
            QDesktopServices::openUrl(QUrl{url});
        }
    });
    layout->addWidget(rows_, 1);
    if (std::ranges::any_of(list.items, [](const auto& item) { return !item.url.isEmpty(); })) {
        auto* hint = new QLabel(QStringLiteral("Double-click a row to open it."), this);
        hint->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(hint);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* copy = buttons->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
    copy->setObjectName(QStringLiteral("bench-plugin-list-copy"));
    connect(copy, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(text()); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

QString PluginListWindow::text() const {
    QStringList lines;
    if (!list_.title.isEmpty()) {
        lines << list_.title;
    }
    for (const auto& item : list_.items) {
        lines << (item.detail.isEmpty() ? item.text
                                        : QStringLiteral("%1 -- %2").arg(item.text, item.detail));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace trackknife::bench
