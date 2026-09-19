// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTableView;
class QTimer;

namespace trackknife::bench {

class MetadataAggregateModel;

// Presentation-only filtering; source indexes and the complete draft stay intact.
class MetadataFieldReviewBar final : public QWidget {
  public:
    MetadataFieldReviewBar(QTableView* fields, MetadataAggregateModel* model, QTableView* files,
                           QWidget* parent = nullptr);
    void revealField(int row);
    void setLayoutFields(QStringList canonical_names);
    [[nodiscard]] QStringList visibleFieldNames() const;
    // ADR-0183 addendum: hidden while the file list is hosted in the
    // sources sidebar, where the toggle has no meaning.
    void setFilesToggleVisible(bool visible);
    [[nodiscard]] bool filesToggleChecked() const;

  private:
    void refresh();

    QTableView* fields_;
    MetadataAggregateModel* model_;
    QCheckBox* show_files_{nullptr};
    QLineEdit* search_;
    QCheckBox* changed_only_;
    QLabel* status_;
    QTimer* debounce_;
    QStringList layout_fields_;
};

} // namespace trackknife::bench
