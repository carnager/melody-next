// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_exact_value_dialog.hpp"

#include <QAbstractListModel>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QModelIndex>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace trackknife::bench {

class MetadataExactValueModel final : public QAbstractListModel {
  public:
    explicit MetadataExactValueModel(QStringList values, QObject* parent = nullptr)
        : QAbstractListModel(parent), values_(std::move(values)) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0
                                : static_cast<int>(std::min(
                                      static_cast<std::size_t>(values_.size()),
                                      static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= values_.size()) {
            return {};
        }
        const auto& value = values_[index.row()];
        if (role == Qt::DisplayRole) {
            return value.isEmpty() ? QVariant{QStringLiteral("(empty · dropped when written)")}
                                   : QVariant{value};
        }
        if (role == Qt::EditRole) {
            return value;
        }
        if (role == Qt::ToolTipRole && value.isEmpty()) {
            return QStringLiteral("This is one explicit empty metadata value");
        }
        if (role == Qt::ForegroundRole && value.isEmpty()) {
            return QApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    }

    bool setData(const QModelIndex& index, const QVariant& value, const int role) override {
        if (!index.isValid() || index.row() < 0 || index.row() >= values_.size() ||
            role != Qt::EditRole) {
            return false;
        }
        const auto replacement = value.toString();
        if (values_[index.row()] == replacement) {
            return false;
        }
        values_[index.row()] = replacement;
        emit dataChanged(index, index,
                         {Qt::DisplayRole, Qt::EditRole, Qt::ForegroundRole, Qt::ToolTipRole});
        return true;
    }

    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (role != Qt::DisplayRole) {
            return {};
        }
        return orientation == Qt::Horizontal ? QVariant{QStringLiteral("Exact value")}
                                             : QVariant{section + 1};
    }

    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override {
        return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                               : Qt::NoItemFlags;
    }

    [[nodiscard]] QModelIndex appendValue() {
        constexpr auto maximum_values = 16'384;
        if (values_.size() >= maximum_values) {
            return {};
        }
        const auto row = static_cast<int>(values_.size());
        beginInsertRows({}, row, row);
        values_.push_back(QString{});
        endInsertRows();
        return index(row, 0);
    }

    void removeIndexes(const QModelIndexList& indexes) {
        std::vector<int> rows;
        rows.reserve(static_cast<std::size_t>(indexes.size()));
        for (const auto& index : indexes) {
            if (index.isValid() && index.row() >= 0 && index.row() < values_.size()) {
                rows.push_back(index.row());
            }
        }
        std::sort(rows.begin(), rows.end(), std::greater<>{});
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        for (const auto row : rows) {
            beginRemoveRows({}, row, row);
            values_.removeAt(row);
            endRemoveRows();
        }
    }

    [[nodiscard]] QModelIndex moveUp(const QModelIndex& current) {
        if (!current.isValid() || current.row() <= 0 || current.row() >= values_.size()) {
            return current;
        }
        const auto row = current.row();
        beginMoveRows({}, row, row, {}, row - 1);
        values_.move(row, row - 1);
        endMoveRows();
        return index(row - 1, 0);
    }

    [[nodiscard]] QModelIndex moveDown(const QModelIndex& current) {
        if (!current.isValid() || current.row() < 0 || current.row() + 1 >= values_.size()) {
            return current;
        }
        const auto row = current.row();
        beginMoveRows({}, row, row, {}, row + 2);
        values_.move(row, row + 1);
        endMoveRows();
        return index(row + 1, 0);
    }

    [[nodiscard]] const QStringList& values() const noexcept { return values_; }

  private:
    QStringList values_;
};

class MetadataExactValueDialog final : public QDialog {
  public:
    MetadataExactValueDialog(const QString& heading, const QString& context, QStringList values,
                             QWidget* parent)
        : QDialog(parent) {
        setObjectName(QStringLiteral("bench-metadata-exact-values"));
        setWindowTitle(QStringLiteral("Edit exact metadata values"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(640, 420);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);
        auto* title = new QLabel(heading, this);
        title->setObjectName(QStringLiteral("bench-metadata-exact-values-heading"));
        layout->addWidget(title);
        auto* explanation = new QLabel(context, this);
        explanation->setObjectName(QStringLiteral("bench-metadata-exact-values-context"));
        explanation->setWordWrap(true);
        layout->addWidget(explanation);

        table_ = new QTableView(this);
        table_->setObjectName(QStringLiteral("bench-metadata-exact-values-table"));
        model_ = new MetadataExactValueModel(std::move(values), table_);
        table_->setModel(model_);
        table_->setAlternatingRowColors(true);
        table_->setShowGrid(false);
        table_->setWordWrap(false);
        table_->setTextElideMode(Qt::ElideRight);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                QAbstractItemView::EditKeyPressed |
                                QAbstractItemView::AnyKeyPressed);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->verticalHeader()->setDefaultSectionSize(24);
        layout->addWidget(table_, 1);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        add_ = new QPushButton(QStringLiteral("Add value"), this);
        add_->setObjectName(QStringLiteral("bench-metadata-exact-values-add"));
        remove_ = new QPushButton(QStringLiteral("Remove value"), this);
        remove_->setObjectName(QStringLiteral("bench-metadata-exact-values-remove"));
        up_ = new QPushButton(QStringLiteral("Move up"), this);
        up_->setObjectName(QStringLiteral("bench-metadata-exact-values-up"));
        down_ = new QPushButton(QStringLiteral("Move down"), this);
        down_->setObjectName(QStringLiteral("bench-metadata-exact-values-down"));
        actions->addWidget(add_);
        actions->addWidget(remove_);
        actions->addWidget(up_);
        actions->addWidget(down_);
        actions->addStretch(1);
        layout->addLayout(actions);

        auto* note = new QLabel(
            QStringLiteral("Each row is one value. Empty rows are preserved; use Delete in "
                           "Properties to remove the field."),
            this);
        note->setWordWrap(true);
        layout->addWidget(note);

        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons_->setObjectName(QStringLiteral("bench-metadata-exact-values-buttons"));
        connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons_);

        connect(add_, &QPushButton::clicked, this, [this] {
            const auto added = model_->appendValue();
            if (added.isValid()) {
                table_->setCurrentIndex(added);
                table_->edit(added);
            }
            updateActions();
        });
        connect(remove_, &QPushButton::clicked, this, [this] {
            model_->removeIndexes(table_->selectionModel()->selectedRows());
            updateActions();
        });
        connect(up_, &QPushButton::clicked, this, [this] {
            table_->setCurrentIndex(model_->moveUp(table_->currentIndex()));
            updateActions();
        });
        connect(down_, &QPushButton::clicked, this, [this] {
            table_->setCurrentIndex(model_->moveDown(table_->currentIndex()));
            updateActions();
        });
        connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this] { updateActions(); });
        connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this] { updateActions(); });
        connect(model_, &QAbstractItemModel::rowsInserted, this, [this] { updateActions(); });
        connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { updateActions(); });
        if (model_->rowCount() > 0) {
            table_->setCurrentIndex(model_->index(0, 0));
        }
        updateActions();
    }

    [[nodiscard]] std::vector<std::string> values() const {
        std::vector<std::string> result;
        result.reserve(static_cast<std::size_t>(model_->values().size()));
        for (const auto& value : model_->values()) {
            const auto encoded = value.toUtf8();
            result.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
        }
        return result;
    }

  private:
    void updateActions() {
        const auto current = table_->currentIndex();
        const auto has_selection = !table_->selectionModel()->selectedRows().empty();
        add_->setEnabled(model_->rowCount() < 16'384);
        remove_->setEnabled(has_selection);
        up_->setEnabled(current.isValid() && current.row() > 0);
        down_->setEnabled(current.isValid() && current.row() + 1 < model_->rowCount());
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(model_->rowCount() > 0);
    }

    MetadataExactValueModel* model_{nullptr};
    QTableView* table_{nullptr};
    QPushButton* add_{nullptr};
    QPushButton* remove_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* down_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

QDialog* createMetadataExactValueDialog(const QString& heading, const QString& context,
                                        QStringList values, QWidget* parent) {
    return new MetadataExactValueDialog(heading, context, std::move(values), parent);
}

std::vector<std::string> metadataExactValueDialogValues(const QDialog* dialog) {
    return static_cast<const MetadataExactValueDialog*>(dialog)->values();
}

} // namespace trackknife::bench
