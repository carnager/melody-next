// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

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

  private:
    void refresh();

    QTableView* fields_;
    MetadataAggregateModel* model_;
    QLineEdit* search_;
    QCheckBox* changed_only_;
    QLabel* status_;
    QTimer* debounce_;
};

} // namespace trackknife::bench
