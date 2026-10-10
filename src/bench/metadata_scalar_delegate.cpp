// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_scalar_delegate.hpp"

#include "bench/metadata_grid_model.hpp"

#include <QAbstractItemModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QStyledItemDelegate>
#include <QTextCursor>

#include <algorithm>

namespace trackknife::bench {
namespace {

// ADR-0276: lyrics are many lines, and so is any value that already has
// line breaks; one line of text cannot hold them.
[[nodiscard]] bool long_text(const QModelIndex& index) {
    const auto canonical = index.data(metadata_field_canonical_name_role).toString();
    if (canonical == QStringLiteral("lyrics") || canonical == QStringLiteral("unsyncedlyrics") ||
        canonical.startsWith(QStringLiteral("lyrics:"))) {
        return true;
    }
    const auto values = index.data(metadata_cell_values_role).toStringList();
    return std::ranges::any_of(values, [](const QString& value) {
        return value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'));
    });
}

class MetadataScalarDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                                        const QModelIndex& index) const override {
        if (index.column() <= 0) {
            return nullptr;
        }
        if (long_text(index)) {
            // Enter starts a new line; Ctrl+Enter keeps the text, Escape
            // drops it, and leaving the editor keeps it, as a line does.
            auto* editor = new QPlainTextEdit(parent);
            editor->setObjectName(QStringLiteral("metadata-long-text-editor"));
            editor->setTabChangesFocus(true);
            editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
            editor->setToolTip(QStringLiteral("Enter starts a new line · Ctrl+Enter keeps the "
                                              "text · Escape discards it"));
            return editor;
        }
        auto* editor = new QLineEdit(parent);
        editor->setFrame(false);
        return editor;
    }

  protected:
    // The delegate filters its editors' keys: Ctrl+Enter keeps a long text,
    // where plain Enter is the text's own new line.
    bool eventFilter(QObject* object, QEvent* event) override {
        if (auto* text = qobject_cast<QPlainTextEdit*>(object);
            text != nullptr && event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                key->modifiers().testFlag(Qt::ControlModifier)) {
                emit commitData(text);
                emit closeEditor(text, QAbstractItemDelegate::NoHint);
                return true;
            }
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }

  public:
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override {
        auto* text = qobject_cast<QPlainTextEdit*>(editor);
        if (text == nullptr) {
            QStyledItemDelegate::updateEditorGeometry(editor, option, index);
            return;
        }
        // Over the cell and the rows below it, room for a verse, kept inside
        // the view: near the bottom it opens upward.
        auto rect = option.rect;
        rect.setWidth(std::max(rect.width(), 360));
        rect.setHeight(text->fontMetrics().lineSpacing() * 14 + 12);
        if (const auto* area = editor->parentWidget()) {
            const auto bounds = area->rect();
            if (rect.bottom() > bounds.bottom()) {
                rect.moveBottom(std::max(bounds.bottom(), option.rect.bottom()));
            }
            if (rect.top() < bounds.top()) {
                rect.moveTop(bounds.top());
            }
            if (rect.right() > bounds.right()) {
                rect.moveRight(bounds.right());
            }
            rect = rect.intersected(bounds.united(option.rect));
        }
        editor->setGeometry(rect);
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        const auto values = index.data(metadata_cell_values_role).toStringList();
        const auto placeholder =
            values.empty() ? QStringLiteral("Type a value")
                           : QStringLiteral("Type to replace %1 exact values").arg(values.size());
        if (auto* text = qobject_cast<QPlainTextEdit*>(editor)) {
            if (values.size() == 1) {
                text->setPlainText(values.front());
                text->moveCursor(QTextCursor::End);
            } else {
                text->clear();
                text->setPlaceholderText(placeholder);
            }
            text->document()->setModified(false);
            return;
        }
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (line == nullptr) {
            return;
        }
        if (values.size() == 1) {
            line->setText(values.front());
            line->selectAll();
        } else {
            line->clear();
            line->setPlaceholderText(placeholder);
        }
        line->setModified(false);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        if (auto* text = qobject_cast<QPlainTextEdit*>(editor)) {
            if (text->document()->isModified()) {
                model->setData(index, text->toPlainText(), Qt::EditRole);
            }
            return;
        }
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (line != nullptr && line->isModified()) {
            model->setData(index, line->text(), Qt::EditRole);
        }
    }
};

} // namespace

QAbstractItemDelegate* createMetadataScalarDelegate(QObject* parent) {
    return new MetadataScalarDelegate(parent);
}

} // namespace trackknife::bench
