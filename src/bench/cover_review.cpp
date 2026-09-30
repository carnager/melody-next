// SPDX-License-Identifier: GPL-3.0-only
#include "bench/cover_review.hpp"
#include "trackknife/core/local_sources.hpp"
#include "workspace/tagger_session.hpp"
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
    auto* note = new QLabel(QString::fromLatin1(folder_image_review_note), dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* table = new QTableWidget(0, 3, dialog);
    table->setHorizontalHeaderLabels({QStringLiteral("Destination"), QStringLiteral("Change"),
                                      QStringLiteral("Incoming image")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    for (const auto& cells : folderImageReviewRows(images)) {
        const auto row = table->rowCount();
        table->insertRow(row);
        for (int column = 0; column < cells.size(); ++column) {
            table->setItem(row, column, new QTableWidgetItem(cells.at(column)));
        }
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
