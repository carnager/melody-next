// SPDX-License-Identifier: GPL-3.0-only
#include "bench/cover_review.hpp"
#include "trackknife/core/local_sources.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <set>
namespace trackknife::bench {
void reviewFolderImages(QWidget* parent, const std::vector<metadata::FolderImageWritePlan>& images,
                        std::function<void()> apply) {
    if (images.empty()) {
        apply();
        return;
    }
    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("bench-folder-cover-review"));
    dialog->setWindowTitle(QStringLiteral("Review folder covers"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);
    auto* note = new QLabel(
        QStringLiteral("Save publishes these folder images and the reviewed media edits. "
                       "Each file has its own recovery journal; a later failure can leave earlier "
                       "files saved. "
                       "Existing folder images retain a recovery backup."),
        dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* table = new QTableWidget(0, 3, dialog);
    table->setHorizontalHeaderLabels({QStringLiteral("Destination"), QStringLiteral("Change"),
                                      QStringLiteral("Incoming image")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    std::set<std::string> seen;
    for (const auto& image : images) {
        if (!seen.insert(image.raw_path).second)
            continue;
        const auto row = table->rowCount();
        table->insertRow(row);
        table->setItem(
            row, 0,
            new QTableWidgetItem(QString::fromStdString(core::display_raw_path(image.raw_path))));
        const auto identical = image.original && image.original->content_fingerprint ==
                                                     image.image.content_fingerprint;
        table->setItem(row, 1,
                       new QTableWidgetItem(identical ? QStringLiteral("Already matches")
                                            : image.original
                                                ? QStringLiteral("Replace (retain backup)")
                                                : QStringLiteral("Create")));
        table->setItem(row, 2,
                       new QTableWidgetItem(
                           QString::fromStdString(core::display_raw_path(image.image.raw_path))));
    }
    layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    buttons->setObjectName(QStringLiteral("bench-folder-cover-review-buttons"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog,
                     [dialog, apply = std::move(apply)] {
                         dialog->accept();
                         apply();
                     });
    dialog->resize(720, 340);
    dialog->show();
}
} // namespace trackknife::bench
