// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/command_palette.hpp"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace trackknife::ui {
namespace {
constexpr int shortcut_role = Qt::UserRole + 1;

class CommandDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& supplied,
               const QModelIndex& index) const override {
        QStyleOptionViewItem option(supplied);
        initStyleOption(&option, index);
        const auto shortcut = index.data(shortcut_role).toString();
        if (shortcut.isEmpty()) {
            QStyledItemDelegate::paint(painter, supplied, index);
            return;
        }
        const auto* style = option.widget ? option.widget->style() : QApplication::style();
        const auto text_rect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget)
                .adjusted(4, 0, -8, 0);
        const auto shortcut_width = option.fontMetrics.horizontalAdvance(shortcut);
        const auto label_width = std::max(0, text_rect.width() - shortcut_width - 24);
        option.text = option.fontMetrics.elidedText(option.text, Qt::ElideRight, label_width);
        // Keep the full item rect for selection/focus styling; only the label is elided.
        style->drawControl(QStyle::CE_ItemViewItem, &option, painter, option.widget);
        painter->save();
        painter->setFont(option.font);
        const auto group =
            option.state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled;
        const auto role =
            option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text;
        painter->setPen(option.palette.color(group, role));
        painter->setClipRect(text_rect);
        painter->drawText(text_rect, Qt::AlignRight | Qt::AlignVCenter, shortcut);
        painter->restore();
    }
};

QString commandName(const QAction* action) {
    auto name = action->text();
    name.remove(QLatin1Char('&'));
    return name.trimmed();
}
} // namespace

CommandPalette::CommandPalette(QList<QAction*> actions, QWidget* parent) : QDialog(parent) {
    for (auto* action : actions) {
        if (action && !action->isSeparator() && !action->menu() &&
            !action->objectName().isEmpty() && !commandName(action).isEmpty() &&
            !actions_.contains(action))
            actions_.append(action);
    }
    std::ranges::sort(actions_, {},
                      [](const auto& action) { return commandName(action).toCaseFolded(); });
    setObjectName(QStringLiteral("command-palette"));
    setWindowTitle(tr("Commands"));
    resize(620, 480);

    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("command-filter"));
    filter_->setPlaceholderText(tr("Find a command by name or shortcut…"));
    filter_->setAccessibleName(tr("Command search"));
    filter_->setClearButtonEnabled(true);
    filter_->installEventFilter(this);
    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("command-results"));
    results_->setAccessibleName(tr("Matching commands"));
    results_->setAlternatingRowColors(true);
    results_->setItemDelegate(new CommandDelegate(results_));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("command-status"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    run_ = buttons_->addButton(tr("Run"), QDialogButtonBox::AcceptRole);
    run_->setObjectName(QStringLiteral("command-run"));
    run_->setDefault(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(filter_);
    layout->addWidget(results_, 1);
    layout->addWidget(status_);
    layout->addWidget(new QLabel(tr("Change shortcuts in Settings → Shortcuts."), this));
    layout->addWidget(buttons_);

    connect(filter_, &QLineEdit::textChanged, this, &CommandPalette::rebuild);
    connect(results_, &QListWidget::currentRowChanged, this, &CommandPalette::updateSelection);
    connect(results_, &QListWidget::itemActivated, this, [this] { runCurrent(); });
    connect(run_, &QPushButton::clicked, this, &CommandPalette::runCurrent);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (const auto& action : actions_) {
        connect(action, &QAction::changed, this, &CommandPalette::rebuild);
        connect(action, &QObject::destroyed, this, [this, pointer = action.data()] {
            // Keep indexes stable until rebuild has discarded the old result items.
            for (auto& item : actions_)
                if (item == pointer)
                    item.clear();
            rebuild();
        });
    }
    rebuild();
    filter_->setFocus();
}

void CommandPalette::rebuild() {
    const QPointer<QAction> selected = currentAction();
    const auto words = filter_->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    results_->clear();
    int selected_row = -1;
    for (qsizetype index = 0; index < actions_.size(); ++index) {
        const auto& action = actions_[index];
        if (!action || !action->isVisible() || !action->isEnabled())
            continue;
        const auto shortcut = action->shortcut().toString(QKeySequence::NativeText);
        const auto name = commandName(action);
        const auto haystack =
            name + QLatin1Char(' ') + shortcut + QLatin1Char(' ') + action->objectName();
        if (!std::ranges::all_of(words, [&](const auto& word) {
                return haystack.contains(word, Qt::CaseInsensitive);
            }))
            continue;
        auto label = name;
        if (action->isCheckable() && action->isChecked())
            label += tr(" · On");
        auto* item = new QListWidgetItem(action->icon(), label, results_);
        item->setData(Qt::UserRole, index);
        item->setData(shortcut_role, shortcut);
        item->setData(Qt::AccessibleTextRole, label + QLatin1Char(' ') + shortcut);
        item->setToolTip(action->toolTip());
        if (action == selected)
            selected_row = results_->count() - 1;
    }
    if (results_->count() > 0)
        results_->setCurrentRow(selected_row >= 0 ? selected_row : 0);
    updateSelection();
}

QAction* CommandPalette::currentAction() const {
    const auto* item = results_->currentItem();
    if (!item)
        return nullptr;
    const auto index = item->data(Qt::UserRole).toLongLong();
    return index >= 0 && index < actions_.size() ? actions_[index].data() : nullptr;
}

void CommandPalette::updateSelection() {
    const auto* action = currentAction();
    run_->setEnabled(action && action->isEnabled() && action->isVisible());
    status_->setText(!action ? tr("No matching command")
                     : !action->isEnabled()
                         ? tr("Unavailable for the current selection or connection.")
                     : action->statusTip().isEmpty() ? tr("Press Enter to run this command.")
                                                     : action->statusTip());
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == filter_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            if (results_->count() > 0)
                results_->setCurrentRow(
                    std::clamp(results_->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1), 0,
                               results_->count() - 1));
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::runCurrent() {
    const QPointer<QAction> action = currentAction();
    if (!action || !action->isEnabled() || !action->isVisible())
        return;
    accept();
    if (action)
        action->trigger();
}
} // namespace trackknife::ui
